#include "doctest.hpp"

#include "ui/term/char_width.hpp"
#include "ui/term/style.hpp"
#include "ui/widgets/box.hpp"
#include "ui/widgets/dialog.hpp"
#include "ui/widgets/doctor_view.hpp"
#include "ui/widgets/input_box.hpp"
#include "ui/widgets/slash_menu.hpp"
#include "ui/widgets/status_line.hpp"
#include "ui/widgets/trace_view.hpp"
#include "ui/widgets/welcome_panel.hpp"

TEST_CASE("UI: CharWidth strip ANSI and width calculations") {
    SUBCASE("strip_ansi removes color sequences") {
        std::string colored = "\033[38;2;217;119;6mHello World\033[0m";
        CHECK(ldns::ui::CharWidth::strip_ansi(colored) == "Hello World");
    }

    SUBCASE("ASCII width") {
        CHECK(ldns::ui::CharWidth::display_width("abc 123") == 7);
    }

    SUBCASE("Vietnamese UTF-8 width") {
        // "Chào mừng Ldns" -> 14 chars, all 1 column
        CHECK(ldns::ui::CharWidth::display_width("Chào mừng Ldns") == 14);
    }

    SUBCASE("Colored text display width equals plain text width") {
        std::string colored = ldns::ui::Style::accent("app.lan");
        CHECK(ldns::ui::CharWidth::display_width(colored) == 7);
    }
}

TEST_CASE("UI: Box wrapping with rounded corners") {
    std::vector<std::string> lines = {"Line 1", "Line 2 is longer"};
    auto boxed = ldns::ui::Box::wrap_box(lines, 30);
    // 1 top + 2 content + 1 bottom = 4 lines
    CHECK(boxed.size() == 4);

    SUBCASE("wrap_box content line count and border integrity") {
        for (const auto& l : boxed) {
            CHECK(!l.empty());
        }
    }
}

TEST_CASE("UI: InputBox text editing and cursor") {
    ldns::ui::InputBox box;
    box.insert_char("add");
    CHECK(box.text() == "add");
    CHECK(box.cursor_pos() == 3);

    box.insert_char(" test.lan");
    CHECK(box.text() == "add test.lan");

    box.backspace();
    CHECK(box.text() == "add test.la");

    box.move_cursor_left();
    box.insert_char("x");
    CHECK(box.text() == "add test.lxa");

    box.clear();
    CHECK(box.text() == "");
    CHECK(box.cursor_pos() == 0);
}

TEST_CASE("UI: InputBox history navigation") {
    ldns::ui::InputBox box;
    box.add_to_history("/add 1");
    box.add_to_history("/add 2");

    box.history_up();
    CHECK(box.text() == "/add 2");

    box.history_up();
    CHECK(box.text() == "/add 1");

    box.history_down();
    CHECK(box.text() == "/add 2");

    box.history_down();
    CHECK(box.text() == "");
}

TEST_CASE("UI: InputBox reverse search Ctrl+R") {
    ldns::ui::InputBox box;
    box.add_to_history("/status");
    box.add_to_history("/help");

    box.start_search();
    CHECK(box.is_search_mode());

    // Search for history item
    box.insert_char("sta");
    CHECK(box.current_search_match() == "/status");

    // Search for built-in item not in history
    box.backspace();
    box.backspace();
    box.backspace();
    box.insert_char("doc");
    CHECK(box.current_search_match() == "/doctor");

    // Cursor visual position inside (bsearch) `<query>`
    int col = box.cursor_visual_col();
    // 1 (border) + 11 ("(bsearch) `") + 3 ("doc") + 1 = 16
    CHECK(col == 16);

    // Accept search
    box.accept_search();
    CHECK(!box.is_search_mode());
    CHECK(box.text() == "/doctor");

    SUBCASE("UTF-8 multi-byte backspace in search mode") {
        ldns::ui::InputBox sbox;
        sbox.start_search();
        // Insert 2-byte UTF-8 character 'ế'
        sbox.insert_char("ế");
        CHECK(sbox.search_query() == "ế");
        sbox.backspace();
        CHECK(sbox.search_query().empty());
    }
}

TEST_CASE("UI: SlashMenu filtering and navigation") {
    ldns::ui::SlashMenu menu;
    CHECK(!menu.filtered_items().empty());

    menu.set_query("/serv");
    CHECK(menu.filtered_items().size() == 1);
    CHECK(menu.filtered_items()[0].name == "/service");

    menu.set_query("/s");
    bool has_service = false;
    for (const auto& item : menu.filtered_items()) {
        if (item.name == "/service") has_service = true;
    }
    CHECK(has_service);

    menu.set_query("/doc");
    CHECK(menu.filtered_items().size() == 1);
    CHECK(menu.filtered_items()[0].name == "/doctor");

    menu.set_query("/re");
    // Should match /resolve
    bool has_resolve = false;
    for (const auto& item : menu.filtered_items()) {
        if (item.name == "/resolve") has_resolve = true;
    }
    CHECK(has_resolve);
}

TEST_CASE("UI: Dialog option selection") {
    ldns::ui::Dialog dlg(
        "Xác nhận?",
        {"Mô tả 1"},
        {{1, "Có"}, {2, "Không"}}
    );
    CHECK(dlg.is_active());
    CHECK(dlg.selected_num() == 1);

    dlg.move_down();
    CHECK(dlg.selected_num() == 2);

    dlg.select_num(1);
    CHECK(dlg.selected_num() == 1);

    dlg.close();
    CHECK(!dlg.is_active());
}

TEST_CASE("UI: StatusLine formatting") {
    ldns::ui::StatusStats stats;
    stats.peer_count = 5;
    stats.domain_count = 12;
    stats.conflict_count = 0;
    stats.online = true;

    std::string line = ldns::ui::StatusLine::render(stats, "ctrl+c thoát", 80);
    CHECK(line.find("5 peer") != std::string::npos);
    CHECK(line.find("12 domain") != std::string::npos);
    CHECK(line.find("ctrl+c") != std::string::npos);
}

TEST_CASE("UI: TraceView and DoctorView rendering") {
    SUBCASE("TraceView renders 3 steps") {
        ldns::ui::TraceResult tr;
        tr.target_domain = "app.lan";
        tr.final_answer = "127.0.0.1";
        tr.steps = {
            {"1. LocalZone", "found", "0.1 ms", {}, true}
        };
        auto lines = ldns::ui::TraceView::render(tr);
        CHECK(lines.size() >= 3);
    }

    SUBCASE("DoctorView renders items") {
        std::vector<ldns::ui::DoctorItem> items = {
            {"Port 53", ldns::ui::DoctorStatus::Pass, "OK", ""}
        };
        auto lines = ldns::ui::DoctorView::render(items);
        CHECK(lines.size() >= 3);
    }
}

TEST_CASE("UI: WelcomePanel responsive rendering at different widths") {
    ldns::ui::WelcomeInfo info;
    info.node_name = "test-node";
    info.node_id = "123456";
    info.network_name = "lan";
    info.ip_address = "192.168.1.50";
    info.dns_port = ":53";
    info.dns_active = true;

    SUBCASE("Wide terminal (80 cols)") {
        auto lines = ldns::ui::WelcomePanel::render(info, 80);
        CHECK(!lines.empty());
        for (const auto& l : lines) {
            CHECK(ldns::ui::CharWidth::display_width(l) <= 80);
        }
    }

    SUBCASE("Medium terminal (50 cols)") {
        auto lines = ldns::ui::WelcomePanel::render(info, 50);
        CHECK(!lines.empty());
        for (const auto& l : lines) {
            CHECK(ldns::ui::CharWidth::display_width(l) <= 50);
        }
    }

    SUBCASE("Narrow terminal (35 cols)") {
        auto lines = ldns::ui::WelcomePanel::render(info, 35);
        CHECK(!lines.empty());
        for (const auto& l : lines) {
            CHECK(ldns::ui::CharWidth::display_width(l) <= 35);
        }
    }
}

#include "ui/widgets/conflict_view.hpp"
#include "ui/widgets/info_view.hpp"
#include "ui/widgets/log_view.hpp"
#include "ui/widgets/network_view.hpp"
#include "ui/widgets/peers_view.hpp"
#include "ui/widgets/upstream_view.hpp"

TEST_CASE("UI: ConflictView rendering") {
    SUBCASE("No conflicts") {
        auto lines = ldns::ui::ConflictView::render({});
        CHECK(!lines.empty());
        CHECK(lines[0].find("Không có xung đột") != std::string::npos);
    }

    SUBCASE("With conflict groups") {
        ldns::ui::ConflictVoteGroup g1{1, 2, "192.168.1.10", {"node1", "node2"}, true};
        ldns::ui::ConflictVoteGroup g2{2, 1, "192.168.1.22", {"node3"}, false};
        ldns::ui::ConflictDomain cd{"web.lan", "A", {g1, g2}, "Hint"};
        auto lines = ldns::ui::ConflictView::render({cd});
        CHECK(!lines.empty());
        CHECK(lines[0].find("Phát hiện 1 domain") != std::string::npos);
    }
}

TEST_CASE("UI: InfoView rendering") {
    ldns::ui::DomainInfo info;
    info.domain = "app.lan";
    info.owner_node = "thang-pc";
    info.owner_ip = "192.168.1.10";
    info.is_local = true;
    info.record_type = "A";
    info.target = "192.168.1.10";
    info.ttl = "60s";
    info.vote_count = 3;
    info.total_peers = 3;
    info.claim_time = "14:00";
    info.sync_status = "Đồng bộ 100%";

    auto lines = ldns::ui::InfoView::render(info);
    CHECK(!lines.empty());
    CHECK(lines[0].find("app.lan") != std::string::npos);
}

TEST_CASE("UI: LogView formatting") {
    ldns::ui::QueryLogEntry entry{
        "14:00:00.000", "127.0.0.1", "A", "app.lan", "[L:Local]", "NOERROR", "150 µs"
    };
    std::string formatted = ldns::ui::LogView::format_entry(entry);
    CHECK(!formatted.empty());
    CHECK(formatted.find("app.lan") != std::string::npos);

    auto lines = ldns::ui::LogView::render({entry});
    CHECK(lines.size() >= 3);
}

TEST_CASE("UI: PeersView rendering") {
    ldns::ui::PeerNode p{"minh-laptop", "192.168.1.22:5391", "2.1 ms", 8, "v0.1.0", true};
    auto lines = ldns::ui::PeersView::render({p});
    CHECK(!lines.empty());
    CHECK(lines[0].find("Danh sách kết nối Peer LAN") != std::string::npos);
}

TEST_CASE("UI: UpstreamView rendering") {
    ldns::ui::UpstreamServer s{"1.1.1.1:53", "Cloudflare", "14 ms", 1, true};
    auto lines = ldns::ui::UpstreamView::render({s});
    CHECK(!lines.empty());
    CHECK(lines[0].find("Upstream DNS Server") != std::string::npos);
}

TEST_CASE("UI: NetworkView rendering") {
    ldns::ui::NetworkInfo info;
    info.network_name = "default";
    info.psk_enabled = false;
    info.active_peers = 2;

    auto lines = ldns::ui::NetworkView::render(info);
    CHECK(!lines.empty());
    CHECK(lines[0].find("Cấu hình Cụm Mạng Ldns") != std::string::npos);
}
