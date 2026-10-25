#include "core/domain/import_export.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <sstream>
#include <string_view>

#include <toml++/toml.hpp>

namespace ldns::domain {
namespace {

using dns::RrType;

std::string trim(std::string_view text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;
    return std::string(text.substr(begin, end - begin));
}

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return text;
}

std::vector<std::string> split(std::string_view text, char separator) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t at = text.find(separator, start);
        const std::size_t end = at == std::string_view::npos ? text.size() : at;
        out.push_back(trim(text.substr(start, end - start)));
        if (at == std::string_view::npos) break;
        start = at + 1;
    }
    return out;
}

// TOML export: quoting/escaping per TOML basic strings.
std::string toml_quote(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 2);
    out.push_back('"');
    for (const char c : text) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: {
                const auto byte = static_cast<unsigned char>(c);
                // TOML 1.0 forbids raw control characters in basic strings.
                // Note the doubled backslash: "\u" alone is a C++ universal
                // character name, not the two TOML characters.
                if (byte < 0x20 || byte == 0x7F) {
                    char escaped[8];
                    std::snprintf(escaped, sizeof(escaped), "\\u%04X",
                                  static_cast<unsigned int>(byte));
                    out += escaped;
                } else {
                    out.push_back(c);
                }
                break;
            }
        }
    }
    out.push_back('"');
    return out;
}

std::string csv_quote(std::string_view field) {
    // RFC 4180: quote fields containing a comma, quote, CR or LF; double
    // embedded quotes.
    const bool needs_quote = field.find_first_of(",\"\r\n") != std::string_view::npos;
    if (!needs_quote) return std::string(field);
    std::string out;
    out.reserve(field.size() + 4);
    out.push_back('"');
    for (const char c : field) {
        if (c == '"') out.push_back('"');
        out.push_back(c);
    }
    out.push_back('"');
    return out;
}

// Splits one CSV line into fields, honouring RFC 4180 quoting and doubled
// quotes. Unterminated quotes are an error.
Result<std::vector<std::string>> parse_csv_line(std::string_view line) {
    std::vector<std::string> fields;
    std::string field;
    bool quoted = false;
    std::size_t i = 0;
    while (i <= line.size()) {
        const bool at_end = i == line.size();
        const char c = at_end ? ',' : line[i];
        if (quoted) {
            if (c == '"') {
                if (i + 1 < line.size() && line[i + 1] == '"') {
                    field.push_back('"');
                    i += 2;
                    continue;
                }
                quoted = false;
                ++i;
                continue;
            }
            if (at_end) return Error{723, "unterminated quoted CSV field"};
            field.push_back(c);
            ++i;
            continue;
        }
        if (c == '"' && field.empty()) {
            quoted = true;
            ++i;
            continue;
        }
        if (c == ',') {
            fields.push_back(std::move(field));
            field.clear();
            if (at_end) break;
            ++i;
            continue;
        }
        field.push_back(c);
        ++i;
    }
    return fields;
}

bool is_checked_type(std::uint16_t type) {
    switch (static_cast<RrType>(type)) {
        case RrType::A:
        case RrType::AAAA:
        case RrType::CNAME:
        case RrType::TXT:
            return true;
        default:
            return false;
    }
}

}  // namespace

Result<ImportReport> import_domains(DomainService& service, std::string_view text,
                                    bool toml_format, bool force) {
    if (text.size() > kMaxImportBytes) {
        return Error{720, "import: file exceeds 64 MiB"};
    }

    std::vector<AddRequest> requests;
    ImportReport report;

    if (toml_format) {
        toml::table root;
        try {
            root = toml::parse(text);
        } catch (const toml::parse_error& error) {
            std::ostringstream message;
            message << "import: " << error;
            return Error{720, message.str()};
        }
        const auto* records = root["record"].as_array();
        if (records == nullptr) return Error{721, "import: expected [[record]] tables"};
        if (records->size() > kMaxImportRecords) {
            return Error{721, "import: more than 500 000 records"};
        }

        requests.reserve(records->size());
        for (const auto& element : *records) {
            const auto* table = element.as_table();
            if (table == nullptr) return Error{721, "import: [[record]] must be a table"};

            AddRequest request;
            const auto name = (*table)["name"].value<std::string>();
            if (!name) return Error{721, "import: record without a name"};
            request.name = *name;

            const auto type = (*table)["type"].value<std::string>();
            if (!type) return Error{721, "import: " + request.name + " has no type"};
            request.type = *type;

            if (table->contains("ttl")) {
                const auto ttl = (*table)["ttl"].value<std::int64_t>();
                if (!ttl) return Error{722, "import: " + request.name + " has a non-integer ttl"};
                if (*ttl < 0 || *ttl > 2'147'483'647) {
                    return Error{722, "import: " + request.name + " has an invalid ttl"};
                }
                request.ttl = static_cast<std::uint32_t>(*ttl);
            }
            if (const auto kind = (*table)["target_kind"].value<std::string>()) {
                if (*kind == "self") {
                    request.kind = TargetKind::Self;
                } else if (*kind != "static") {
                    return Error{722, "import: " + request.name + " has an invalid target_kind"};
                }
            }

            if (const auto scalar = (*table)["target"].value<std::string>()) {
                request.targets.push_back(*scalar);
            } else if (const auto* array = (*table)["target"].as_array()) {
                for (const auto& value : *array) {
                    const auto item = value.value<std::string>();
                    if (!item) return Error{722, "import: target must be a string"};
                    request.targets.push_back(*item);
                }
            } else {
                return Error{722, "import: " + request.name + " has no target"};
            }
            if (requests.size() >= kMaxImportRecords) {
                return Error{721, "import: more than 500 000 records"};
            }
            requests.push_back(std::move(request));
        }
    } else {
        std::istringstream stream{std::string(text)};
        std::string line;
        std::size_t line_number = 0;
        bool header_checked = false;
        while (std::getline(stream, line)) {
            ++line_number;
            const std::string cleaned = trim(line);
            if (cleaned.empty() || cleaned[0] == '#') continue;

            // An optional header on the first data line, whatever its line
            // number (comments and blank lines may precede it).
            if (!header_checked) {
                header_checked = true;
                const auto first = split(cleaned, ',');
                if (first.size() >= 2 && lower(first[0]) == "name" && lower(first[1]) == "type") {
                    continue;
                }
            }

            const auto parsed_fields = parse_csv_line(cleaned);
            if (!parsed_fields) return parsed_fields.error();
            const auto& fields = parsed_fields.value();
            if (fields.size() < 2) {
                return Error{723, "import: line " + std::to_string(line_number) +
                                       " needs name and type"};
            }
            AddRequest request;
            request.name = fields[0];
            request.type = fields[1];
            if (fields.size() >= 3 && !fields[2].empty()) {
                try {
                    const auto ttl = std::stoul(fields[2]);
                    if (ttl > 2'147'483'647) throw std::out_of_range("ttl");
                    request.ttl = static_cast<std::uint32_t>(ttl);
                } catch (const std::exception&) {
                    return Error{724, "import: line " + std::to_string(line_number) +
                                           " has an invalid ttl"};
                }
            }
            if (fields.size() >= 4 && !fields[3].empty()) {
                request.targets = split(fields[3], ';');
            }
            if (requests.size() >= kMaxImportRecords) {
                return Error{721, "import: more than 500 000 records"};
            }
            requests.push_back(std::move(request));
        }
    }

    if (requests.empty()) return report;

    auto bulk = service.add_many(requests, force);
    if (!bulk) return bulk.error();
    report.imported = bulk.value().applied;
    report.skipped = requests.size() - report.imported;
    return report;
}

std::string export_domains(const std::vector<DomainEntry>& entries, bool toml_format) {
    std::ostringstream out;
    if (toml_format) {
        for (const auto& entry : entries) {
            out << "[[record]]\n";
            out << "name = " << toml_quote(entry.name) << "\n";
            out << "type = " << toml_quote(entry.type) << "\n";
            out << "ttl = " << entry.ttl << "\n";
            out << "target_kind = " << toml_quote(entry.target_kind) << "\n";
            out << "target = [";
            for (std::size_t i = 0; i < entry.targets.size(); ++i) {
                if (i != 0) out << ", ";
                out << toml_quote(entry.targets[i]);
            }
            out << "]\n\n";
        }
        return out.str();
    }

    out << "name,type,ttl,target\n";
    for (const auto& entry : entries) {
        if (!is_checked_type(dns::rr_type_from_name(entry.type).value_or(0))) continue;
        std::string combined;
        for (std::size_t i = 0; i < entry.targets.size(); ++i) {
            if (i != 0) combined.push_back(';');
            combined += entry.targets[i];
        }
        out << csv_quote(entry.name) << ',' << csv_quote(entry.type) << ',' << entry.ttl << ','
            << csv_quote(combined) << '\n';
    }
    return out.str();
}

}  // namespace ldns::domain
