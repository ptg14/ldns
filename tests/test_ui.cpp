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
}

TEST_CASE("UI: SlashMenu filtering and navigation") {
    ldns::ui::SlashMenu menu;
    CHECK(!menu.filtered_items().empty());

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
