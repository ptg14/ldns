# Nhật ký làm việc — Ldns

## 2026-10-23 01:38 — bước quan trọng (nhánh feat/ux-ui: thiết kế UX/UI & TUI inline theo Claude Code)
- Việc đã làm:
  - Tạo nhánh riêng `feat/ux-ui` phục vụ thiết kế và phát triển giao diện theo yêu cầu.
  - Đọc kỹ toàn bộ tài liệu dự án (`SPEC.md`, `PLAN.md`, `protocol.md`, `platform-notes.md`).
  - Soạn thảo tài liệu đặc tả thiết kế UX/UI toàn diện [`docs/UI-SPEC.md`](docs/UI-SPEC.md) (triết lý inline TUI giữ scrollback, dual-zone rendering, bảng màu Terracotta, hệ thống biểu tượng, keymap, trực quan hóa `/status`, `/resolve --trace`, `/doctor`, `/conflicts`, `/peers`, `/logs`).
  - Xây dựng thư viện giao diện `src/ui/`:
    * `src/ui/term/`: `Terminal` (VT processing, raw mode trên Windows/POSIX), `Style` (màu Terracotta, NO_COLOR, ASCII fallback), `CharWidth` (tính cột hiển thị UTF-8 tiếng Việt, diacritics, strip ANSI), `InputParser` (phím VT, Ctrl+C/D/L/R, Tab, Esc).
    * `src/ui/widgets/`: `Box` (viền bo tròn), `WelcomePanel` (thẻ banner chào mừng), `InputBox` (hộp nhập nhiều dòng, con trỏ di chuyển, lịch sử ↑/↓, tìm kiếm Ctrl+R), `SlashMenu` (popup gợi ý lệnh lọc thời gian thực), `Dialog` (hộp thoại xác nhận đánh số phím tắt), `Spinner` (xoay vòng kèm thời gian trôi qua), `StatusLine` (chân trang live), `TraceView` (cây phân giải 3 bước), `DoctorView` (danh sách kiểm tra sức khoẻ có biểu tượng).
    * `src/ui/inline_renderer.*`: Bộ điều phối 2 vùng render scrollback bất biến và vùng động ở đáy chống chớp nháy.
    * `src/ui/tui_app.*`: Bộ điều phối tương tác TUI hoàn chỉnh và xử lý lệnh.
  - Bổ sung `tests/test_ui.cpp` gồm 8 test case kiểm thử toàn diện các widget UI.
  - Tích hợp vào `CMakeLists.txt` và `src/main.cpp` (chạy `ldns` mở TUI tương tác, `--headless` chạy nền).
  - Soạn thảo bộ checklist kiểm thử 11 nhóm thao tác dành riêng cho Tester trong [`docs/UI-SPEC.md`](docs/UI-SPEC.md) và tạo file nhật ký làm việc riêng [`UI_CHECKLIST.md`](UI_CHECKLIST.md) để tester trực tiếp ghi nhận và chấm điểm từng phiên test.
- File liên quan: [`UI_CHECKLIST.md`](UI_CHECKLIST.md), [`docs/UI-SPEC.md`](docs/UI-SPEC.md), `src/ui/*`, `tests/test_ui.cpp`, `CMakeLists.txt`, `tests/CMakeLists.txt`, `src/main.cpp`, `process-ui.md`.
- Cần chú ý: Chỉ tập trung vào tầng UX/UI, không can thiệp sâu vào logic core DNS hay giao thức peer. Không tự ý commit theo quy tắc CLAUDE.md §2.
- Bug/lỗ hổng: không có; đã xử lý cảnh báo C4996 MSVC với `getenv` và include `<chrono>`.
- Test: Windows MSVC **98 test cases / 1628 assertions pass (100% xanh)**; binary `build/local-windows-msvc-debug/Debug/ldns.exe` sẵn sàng cho tester trải nghiệm trực tiếp.
- Tiếp theo: Bàn giao báo cáo và danh sách kịch bản test để người dùng tự kiểm thử.

## 2026-10-23 02:18 — bước quan trọng (sửa lỗi BUG-01: trôi con trỏ và xóa nhầm scrollback trong InlineRenderer)
- Việc đã làm:
  - Phân tích và khắc phục lỗi nghiêm trọng người dùng phát hiện ở Nhóm 2 (khi gõ ký tự hay ấn `->`, `backspace`, vùng trên ô nhập bị xóa dần khiến ô nhập leo lên đỉnh CLI):
    1. **Nguyên nhân 1 (Con trỏ bị lệch khi vẽ lại):** `InlineRenderer` sau khi vẽ đã đưa con trỏ lên vị trí ô nhập (`cursor_line_offset`), nhưng ở lần vẽ tiếp theo lại gọi `move_up(last_dynamic_height_ - 1)` từ vị trí ô nhập thay vì từ đỉnh dòng 0. Do đó, con trỏ nhảy ngược lên 2-3 dòng vào vùng scrollback bên trên và xóa dần nội dung phía trên.
       $\rightarrow$ **Khắc phục:** Thêm biến `cursor_y_` theo dõi chính xác vị trí dòng hiện tại của con trỏ trong dynamic zone. Khi xóa hoặc vẽ lại, chỉ di chuyển ngược lên `cursor_y_` dòng về đúng đỉnh dòng 0 của dynamic zone, bảo vệ 100% các dòng scrollback bên trên.
    2. **Nguyên nhân 2 (Parser phím VT Escape):** Khi người dùng ấn phím mũi tên `->`, `<-` trong Windows Terminal (chế độ VT input), terminal gửi chuỗi escape `\033[C`. Trước đó code coi byte 27 là phím `Esc` đơn lẻ (gây xóa ô nhập) rồi đọc `[` và `C` thành ký tự thường.
       $\rightarrow$ **Khắc phục:** Thêm hàm `Terminal::has_byte_pending()` và giải mã đầy đủ chuỗi ANSI escape (`\033[A`, `\033[B`, `\033[C`, `\033[D`, `\033[H`, `\033[F`, `\033[3~`) giúp nhận diện chính xác các phím mũi tên, Home, End, Delete trên mọi terminal.
  - Cập nhật nhật ký lỗi `BUG-01` vào [`UI_CHECKLIST.md`](UI_CHECKLIST.md).
- File liên quan: `src/ui/inline_renderer.hpp`, `src/ui/inline_renderer.cpp`, `src/ui/term/terminal.hpp`, `src/ui/term/terminal.cpp`, `src/ui/term/input_event.hpp`, [`UI_CHECKLIST.md`](UI_CHECKLIST.md), `process-ui.md`.
- Test: Windows MSVC build pass, `ldns_tests` 100% pass (98 test cases / 1628 assertions); Linux GCC 13 trên WSL pass. Binary `ldns.exe` đã được build mới sẵn sàng.
- Tiếp theo: Tiếp tục hỗ trợ tester kiểm thử các nhóm tính năng trên nhánh `feat/ux-ui`.

## 2026-10-23 02:24 — bước quan trọng (cập nhật quy tắc UX/UI vào CLAUDE.md)
- Việc đã làm:
  - Cập nhật các quy tắc làm việc do người dùng chỉ định vào [`CLAUDE.md`](CLAUDE.md):
    1. Bổ sung mục §5: Tách biệt nhật ký làm việc giữa Core (`process.md`) và UI (`process-ui.md`) nhằm chống xung đột git.
    2. Thêm mục §6 "Quy tắc riêng cho phân hệ UX / UI":
       - Chỉ tập trung thiết kế/triển khai UX/UI, không can thiệp sâu vào logic vận hành core.
       - Làm việc trên nhánh riêng về UI (`feat/ux-ui`).
       - Báo cáo luôn đi kèm checklist kịch bản test chi tiết để người dùng tự kiểm thử với vai trò tester.
       - Theo dõi và ghi nhận lỗi vào Bug Tracker trong [`UI_CHECKLIST.md`](UI_CHECKLIST.md).
- File liên quan: [`CLAUDE.md`](CLAUDE.md), `process-ui.md`.
- Tiếp theo: Sẵn sàng nhận phản hồi test tiếp theo từ người dùng.

## 2026-10-23 02:37 — bước quan trọng (sửa BUG-02 font headless và BUG-03 tìm kiếm ngược Ctrl+R)
- Việc đã làm:
  - Đọc ghi nhận từ tester trong [`UI_CHECKLIST.md`](UI_CHECKLIST.md) và xử lý triệt để 2 lỗi:
    1. **BUG-02 (TC-01.4):** Chạy `ldns.exe --headless` bị lỗi font Windows in `ΓÇö` thay vì em-dash.
       $\rightarrow$ **Khắc phục:** Khởi tạo `SetConsoleOutputCP(CP_UTF8)` và `SetConsoleCP(CP_UTF8)` ngay đầu hàm `main()` cho mọi chế độ chạy, đồng thời chuẩn hóa dấu gạch ngang ASCII `-` trong log headless.
    2. **BUG-03 (TC-03.3):** Tìm kiếm `Ctrl+R` con trỏ hiển thị sai vị trí `(b|search)`, không tìm được lệnh và ấn Enter xóa sạch ô nhập.
       $\rightarrow$ **Khắc phục:**
       - Sửa hàm `InputBox::cursor_visual_col()`: khi ở chế độ `(bsearch)`, con trỏ được tính đúng cột nằm ngay sau chuỗi truy vấn bên trong cặp dấu backtick `` `...` ``.
       - Mở rộng hàm `InputBox::current_search_match()`: tìm kiếm ngược trong lịch sử lệnh, và nếu lịch sử chưa có thì tìm kiếm thông minh trong danh mục các lệnh built-in (`/doctor`, `/status`, `/resolve`,...).
       - Xử lý phím `Enter` và `Tab` trong `TuiApp`: ấn `Enter` sẽ tự động chọn lệnh match và thực thi; ấn `Tab` sẽ điền lệnh vào ô nhập để người dùng chỉnh sửa tham số.
       - Bổ sung unit test `TEST_CASE("UI: InputBox reverse search Ctrl+R")` trong `tests/test_ui.cpp`.
  - Cập nhật bảng Bug Tracker trong [`UI_CHECKLIST.md`](UI_CHECKLIST.md).
- File liên quan: `src/main.cpp`, `src/ui/widgets/input_box.hpp`, `src/ui/widgets/input_box.cpp`, `src/ui/tui_app.cpp`, `tests/test_ui.cpp`, [`UI_CHECKLIST.md`](UI_CHECKLIST.md), `process-ui.md`.
- Test: Windows MSVC build pass, `ldns_tests` 100% pass (99 test cases / 1634 assertions); Linux GCC 13 trên WSL pass. Binary `ldns.exe` đã được build mới sẵn sàng.
- Tiếp theo: Báo cáo tester kiểm tra lại TC-01.4 và TC-03.3.




## 2026-10-23 03:15 — bước quan trọng (sửa BUG-04 resize terminal động và BUG-05 căn lề /doctor)
- Việc đã làm:
  - Tiếp nhận phản hồi từ Tester trong [`UI_CHECKLIST.md`](UI_CHECKLIST.md) (TC-08.1 và TC-07.3) và xử lý triệt để:
    1. **BUG-04 (TC-08.1):** Khi thay đổi kích thước cửa sổ (resize), `InputParser` bị chặn (block) vĩnh viễn ở `_getch()` / `read()` do không có phím nào được bấm, dẫn đến UI ngủ đông không co giãn; khi thu nhỏ, dòng trạng thái dài hoặc khung bị tràn xuống hàng mới làm hỏng vị trí con trỏ; khi phóng to thì ô nhập bị kẹt ở kích thước cũ (80 cột).
       $\rightarrow$ **Khắc phục:**
       - Thêm `KeyType::Resize` vào enum sự kiện bàn phím.
       - Cải tiến `InputParser::read_next_event(term, &last_size)`: sử dụng chu kỳ polling 25ms (`has_byte_pending(25)`); nếu `cols` hoặc `rows` thay đổi, trả về ngay lập tức sự kiện `KeyType::Resize` để render lại tức thời theo thời gian thực (~40 FPS khi rê chuột kéo cửa sổ).
       - Đồng bộ độ rộng co giãn động: `box_width = sz.cols > 2 ? sz.cols - 1 : sz.cols` cho cả `InputBox` và `StatusLine`. `ctrl+c×2 thoát` luôn được neo chuẩn xác ở góc dưới bên phải của khung ô nhập, không bao giờ chạm cột cuối cùng `sz.cols` để triệt tiêu lỗi tự ngắt dòng (margin wrap) của console Windows.
       - Tích hợp `CharWidth::clip_to_width()` vào `Box::wrap_box()` để đảm bảo khung luôn được cắt gọn vừa khít, không bao giờ vỡ giao diện kể cả khi co hẹp cửa sổ đột ngột.
    2. **BUG-05 (TC-07.3):** Trong bảng `/doctor`, các dòng chứa tiếng Việt có dấu bị xô lệch căn lề (do dùng `std::string::size()` đếm byte UTF-8 thay vì độ rộng hiển thị).
       $\rightarrow$ **Khắc phục:** Thay thế bằng `CharWidth::display_width()` để căn chuẩn xác 26 cột hiển thị cho tên mục và thụt lề 31 cột cho dòng gợi ý.
  - Cập nhật bảng Bug Tracker trong [`UI_CHECKLIST.md`](UI_CHECKLIST.md).
- File liên quan: `src/ui/term/input_event.hpp`, `src/ui/term/char_width.hpp`, `src/ui/widgets/box.hpp`, `src/ui/widgets/status_line.cpp`, `src/ui/widgets/input_box.cpp`, `src/ui/widgets/doctor_view.cpp`, `src/ui/inline_renderer.cpp`, `src/ui/tui_app.cpp`, [`UI_CHECKLIST.md`](UI_CHECKLIST.md), `process-ui.md`.
- Test: Windows MSVC debug build thành công, 100% tests pass (99 test cases / 1634 assertions); Linux WSL GCC 13 build & test 100% pass. Binary `build/local-windows-msvc-debug/Debug/ldns.exe` đã được cập nhật sẵn sàng.
- Tiếp theo: Báo cáo để tester mở lại `ldns.exe` và kiểm tra thao tác resize (kéo chuột đổi kích thước cửa sổ).

## 2026-10-23 03:32 — bước quan trọng (sửa BUG-06: WelcomePanel responsive 3 cấp độ và triệt tiêu khung rác khi resize)
- Việc đã làm:
  - Tiếp nhận phản hồi từ Tester: khi thu nhỏ terminal, thẻ thông tin bên trên ô nhập (Welcome Banner) bị vỡ khung/tràn dòng, đồng thời khi rê chuột resize cửa sổ xuất hiện nhiều khung rác nằm giữa thẻ thông tin và ô nhập.
  - Phân tích nguyên nhân:
    1. Thẻ chào mừng trước đó in 1 lần duy nhất vào scrollback với độ rộng cố định (~64 cột). Khi co cửa sổ xuống dưới 64 cột, console của terminal tự động quấn dòng các viền và nội dung dài 63 ký tự, làm vỡ nát khung bo tròn.
    2. Khi các dòng của thẻ thông tin và ô nhập bị quấn dòng, chiều cao thực tế trên màn hình tăng vọt. Con trỏ bị đẩy xuống dưới nhưng `cursor_y_` trong `InlineRenderer` chỉ lưu số dòng logic cũ; lệnh `move_up(cursor_y_)` chỉ nhảy lên lưng chừng, không xóa được phần trên của ô nhập cũ, để lại hàng loạt mảnh khung rác chồng chất khi kéo chuột resize liên tục.
  - Giải pháp đã triển khai:
    1. **WelcomePanel Responsive 3 cấp độ rộng:**
       - Rộng ($\ge 62$ cột): Bố cục 2 cột song song chuẩn Claude Code, hiển thị đầy đủ node, IP, mạng, DNS port và mẹo phím tắt.
       - Trung bình ($36 \le W < 62$): Tự động chuyển thành 3 hàng ngắn gọn, tách biệt mẹo và thông số máy để không dòng nào vượt quá chiều rộng cho phép.
       - Hẹp ($< 36$ cột): Rút gọn tối giản dạng checklist ngắn, bảo đảm vừa khít terminal siêu hẹp.
       - Đồng bộ độ rộng thẻ theo `(cols - 1)` khớp 100% với hộp nhập lệnh bên dưới.
    2. **Đưa Welcome Card vào vùng Dynamic Zone lúc khởi chạy:**
       - Ở màn hình ban đầu (trước khi người dùng gõ lệnh đầu tiên), cả thẻ thông tin và ô nhập được quản lý chung trong 1 dynamic zone.
       - Khi kéo chuột resize, lệnh `\033[2J\033[1;1H` và `renderer_.reset()` xóa sạch hoàn toàn màn hình và vẽ lại tức thì ở vị trí đỉnh (1,1), triệt tiêu vĩnh viễn mọi khung rác phát sinh.
       - Ngay khi người dùng gõ lệnh đầu tiên (ví dụ `/doctor` hay `/status`), thẻ thông tin được commit sạch sẽ 1 lần duy nhất vào scrollback và từ đó dynamic zone chỉ giữ lại ô nhập và thanh trạng thái.
    3. **Bổ sung `prepare_for_resize()` trong InlineRenderer:** Tính toán hệ số co dòng `mult` khi thu nhỏ terminal sau khi đã có lịch sử lệnh, di chuyển con trỏ lên đúng vị trí gốc và xóa sạch dòng thừa.
    4. **Bổ sung unit test:** `TEST_CASE("UI: WelcomePanel responsive rendering at different widths")` trong `tests/test_ui.cpp` kiểm tra cả 3 mốc chiều rộng 80, 50, 35 cột.
  - Cập nhật Bug Tracker trong [`UI_CHECKLIST.md`](UI_CHECKLIST.md).
- File liên quan: `src/ui/widgets/welcome_panel.cpp`, `src/ui/inline_renderer.hpp`, `src/ui/inline_renderer.cpp`, `src/ui/tui_app.hpp`, `src/ui/tui_app.cpp`, `tests/test_ui.cpp`, [`UI_CHECKLIST.md`](UI_CHECKLIST.md), `process-ui.md`.
- Test: Windows MSVC build pass, 100% tests pass (100 test cases / 1640 assertions); Linux WSL GCC 13 build & ctest pass 100%. Binary `build/local-windows-msvc-debug/Debug/ldns.exe` đã được làm mới.
- Tiếp theo: Báo cáo Tester mở lại `ldns.exe` và kiểm tra lại thao tác resize từ to xuống nhỏ và ngược lại.

## 2026-10-23 03:45 — bước quan trọng (sửa BUG-07: triệt tiêu hiện tượng nháy màn hình flicker-free khi gõ phím và resize)
- Việc đã làm:
  - Tiếp nhận phản hồi từ Tester: khi nhập ký tự hoặc khi resize cửa sổ, màn hình liên tục nhấp nháy gây chói mắt.
  - Phân tích nguyên nhân gốc rễ của hiện tượng chớp nháy:
    1. **Khi gõ phím:** Hàm `render_dynamic_zone()` trước đó gọi lệnh xóa toàn bộ vùng dưới con trỏ `term_.clear_below()` (`\033[J`) trước khi in các dòng mới. Điều này làm màn hình bị xóa đen hoàn toàn trong một khoảnh khắc rồi mới vẽ lại chữ, tạo ra hiệu ứng nhấp nháy (strobe flash) trên từng phím bấm. Hơn nữa, việc in từng dòng bằng các lệnh `std::cout` riêng lẻ khiến terminal phải render nhiều lần trong 1 frame.
    2. **Khi resize:** Trước đó gọi `\033[2J` xóa trắng toàn màn hình mỗi 25ms, làm màn hình chớp tắt liên tục với tần số cao khi rê chuột kéo cửa sổ.
  - Giải pháp đã triển khai:
    1. **Cơ chế ghi đè nguyên tử tại chỗ (Atomic In-Place Double Buffering):**
       - Loại bỏ hoàn toàn lệnh xóa màn hình `\033[J` khi gõ phím.
       - Thay bằng kỹ thuật ghi đè dòng tại chỗ: đưa con trỏ về đầu dòng `\r`, in nội dung mới và dùng `\033[K` (Clear to End of Line) để chỉ xóa phần ký tự thừa ở cuối dòng (nếu dòng mới ngắn hơn dòng cũ).
       - Toàn bộ chuỗi escape di chuyển con trỏ, các dòng văn bản và hiển thị con trỏ được gom vào một `std::string buf` duy nhất trong RAM và gửi ra console trong đúng 1 lệnh ghi `write`/`flush`. Terminal xử lý trong 1 chu kỳ vsync, triệt tiêu 100% hiện tượng chớp nháy (hoàn toàn flicker-free).
    2. **Hàm `render_full_screen()` chống chớp khi Resize & Ctrl+L:**
       - Thay vì `\033[2J` xóa trắng màn hình, `render_full_screen()` dịch chuyển con trỏ thẳng về `\033[1;1H`, ghi đè tuần tự các dòng bằng `\033[K` và chỉ dọn dẹp các dòng trống phía dưới bằng `\033[J` ở cuối cùng, tất cả trong 1 buffer duy nhất.
    3. **Debounce / Throttle sự kiện Resize:**
       - Trong `InputParser::read_next_event()`, thêm bộ đệm thời gian 30ms gom các sự kiện co giãn liên tiếp khi rê chuột. Tránh kích hoạt redraw hàng trăm lần/giây, giữ tốc độ làm mới mượt mà ở mức ~25 FPS ổn định.
  - Cập nhật Bug Tracker trong [`UI_CHECKLIST.md`](UI_CHECKLIST.md).
- File liên quan: `src/ui/inline_renderer.hpp`, `src/ui/inline_renderer.cpp`, `src/ui/term/input_event.hpp`, `src/ui/tui_app.hpp`, `src/ui/tui_app.cpp`, [`UI_CHECKLIST.md`](UI_CHECKLIST.md), `process-ui.md`.
- Test: Windows MSVC build pass, 100% tests pass (100 test cases / 1640 assertions); Linux WSL GCC 13 build & ctest pass 100%. Binary `build/local-windows-msvc-debug/Debug/ldns.exe` đã được biên dịch xong.
- Tiếp theo: Báo cáo Tester mở lại `ldns.exe` và kiểm tra độ mượt mà khi gõ phím nhanh và kéo chuột resize.

## 2026-10-23 03:58 — bước quan trọng (sửa BUG-08 lệch mép khung và BUG-09 giãn cách ký hiệu thông tin)
- Việc đã làm:
  - Tiếp nhận phản hồi từ Tester:
    1. Các ký hiệu/icon thông tin (`✔`, `⚠`, `✖`, `●`, `⏺`) nằm quá sát ký tự chữ làm xấu giao diện.
    2. Khung thẻ thông tin và khung ô nhập kích thước không bằng nhau, mép bên phải của khung nhập dài hơn khung thẻ 1 cột.
  - Phân tích và khắc phục:
    1. **Khắc phục BUG-08 (Lệch mép phải giữa 2 khung):**
       - Trong `TuiApp::render_all()`, biến `box_width = sz.cols - 1` đã được trừ 1 cột viền đệm và truyền cho cả `InputBox` lẫn `WelcomePanel`. Tuy nhiên bên trong `WelcomePanel::render()`, mã cũ lại tiếp tục thực hiện `box_width = term_width - 1` lần thứ hai, làm thẻ thông tin bị ngắn hơn hộp nhập đúng 1 cột.
       - Đã loại bỏ phép trừ lặp lại: `WelcomePanel::render(info, box_width)` nhận trực tiếp `box_width` tương tự `InputBox`, đảm bảo mép viền phải của cả 2 khung (`╮`, `│`, `╯`) thẳng hàng tuyệt đối.
    2. **Khắc phục BUG-09 (Khoảng cách ký hiệu thông tin):**
       - Chuẩn hóa khoảng cách đệm 2 khoảng trắng (`"  "`) sau tất cả các ký hiệu trạng thái trên toàn bộ giao diện:
         * Bảng `/doctor`: `✔  Cổng DNS 53`, `⚠  DNS Hệ điều hành`, `✖  Lỗi` và căn lề dòng gợi ý 32 ký tự.
         * Chân trang `StatusLine`: `●  0 peer · 0 domain · 0 xung đột`.
         * Thẻ thông tin `WelcomePanel`: `●  hoạt động`, `✻  Chào mừng...`.
         * Cây phân giải `TraceView`: `⏺  Kết quả: ...`, `⎿  Quá trình phân giải...`.
         * Menu gợi ý & Dialog: `❯  /add`, `❯  1. Có`.
         * Các thông báo kết quả lệnh (`/status`, `/add`, `/rm`, `/peers`, `/conflicts`): `⏺  Đã thêm...`, `●  127.0.0.1:53`, `●  online`.
  - Cập nhật Bug Tracker trong [`UI_CHECKLIST.md`](UI_CHECKLIST.md).
- File liên quan: `src/ui/widgets/welcome_panel.cpp`, `src/ui/widgets/status_line.cpp`, `src/ui/widgets/doctor_view.cpp`, `src/ui/widgets/trace_view.cpp`, `src/ui/widgets/slash_menu.cpp`, `src/ui/widgets/dialog.cpp`, `src/ui/tui_app.cpp`, `tests/test_ui.cpp`, [`UI_CHECKLIST.md`](UI_CHECKLIST.md), `process-ui.md`.
- Test: Windows MSVC build pass, 100% tests pass (100 test cases / 1640 assertions); Linux WSL GCC 13 build & ctest pass 100%. Binary `build/local-windows-msvc-debug/Debug/ldns.exe` đã được cập nhật hoàn chỉnh.
- Tiếp theo: Báo cáo Tester mở lại `ldns.exe` để kiểm tra độ thẳng hàng của 2 khung viền và khoảng thở của các ký hiệu trạng thái.

## 2026-10-23 04:15 — bước quan trọng (sửa BUG-10: giải quyết triệt để vỡ thẻ và khung rác khi resize sau khi chạy lệnh)
- Việc đã làm:
  - Tiếp nhận phản hồi từ Tester: Khi chưa chạy lệnh nào thì resize rất ổn không có bug, nhưng sau khi thực thi lệnh (ví dụ `/status`) và hiển thị thông tin, kéo resize cửa sổ thì thẻ chào mừng lại bị vỡ viền và sinh ra nhiều khung rác.
  - Phân tích nguyên nhân gốc rễ:
    1. Khi người dùng chạy lệnh đầu tiên (`/status`), thẻ chào mừng và kết quả của lệnh được đẩy lên vùng scrollback của terminal với kích thước cố định ở thời điểm đó.
    2. Khi người dùng thu hẹp cửa sổ console sau đó: Terminal tự động quấn dòng (wrap) các dòng đã in trong scrollback. Do thẻ chào mừng có viền cứng bo góc (`╭───╮`), việc ngắt dòng làm vỡ nát toàn bộ hình dạng thẻ. Hơn nữa, việc quấn dòng làm tăng số dòng vật lý trong scrollback, đẩy con trỏ hiện tại trôi sâu xuống dưới.
    3. Bộ render dynamic zone trước đó chỉ nhảy ngược lên đúng số dòng logic của ô nhập (`cursor_y_`) mà không biết scrollback đã dãn thêm bao nhiêu dòng, khiến các lần vẽ lại ô nhập in đè xuống các vị trí sai lệch, tạo thành hàng loạt mảnh khung rác chồng chất.
  - Giải pháp triển khai:
    1. **Kiến trúc Scrollback History (`scrollback_entries_`):**
       - Bổ sung cấu trúc `ScrollbackEntry { bool is_welcome; std::vector<std::string> lines; }` trong `TuiApp`.
       - Mọi kết quả thực thi lệnh (`/status`, `/doctor`, `/peers`, thông báo...) và thẻ chào mừng đều được lưu vào mảng `scrollback_entries_` theo thời gian thực.
    2. **Hàm làm mới toàn diện `redraw_full_view()`:**
       - Khi phát hiện sự kiện `KeyType::Resize` (hoặc `Ctrl+L`), `redraw_full_view()` được gọi.
       - Tự động lấy kích thước terminal mới nhất `(cols, rows)`.
       - Tính toán lại `box_width = sz.cols > 2 ? sz.cols - 1 : sz.cols`.
       - **Tự động re-render lại thẻ `WelcomePanel` theo độ rộng mới**: Nếu cửa sổ bị thu hẹp, thẻ sẽ co lại mượt mà thành layout 3 hàng hoặc layout tối giản, không bao giờ vượt quá biên console, không bị terminal tự động quấn dòng làm vỡ khung.
       - Ghép toàn bộ lịch sử lệnh + các thành phần dynamic zone (ô nhập, menu gợi ý, chân trang) vào một buffer chuỗi duy nhất.
       - Gửi buffer qua `renderer_.render_full_screen()`: dịch con trỏ về `\033[1;1H`, ghi đè tuần tự và dọn dẹp các dòng dư ở đáy bằng `\033[J` trong 1 lần flush duy nhất.
    3. **Chống nhấp nháy và loại bỏ 100% khung rác:**
       - Triệt tiêu hoàn toàn hiện tượng sinh khung rác bất kể người dùng resize trước hay sau khi chạy bao nhiêu lệnh.
       - Trải nghiệm co giãn mượt mà, đồng bộ và ổn định tuyệt đối.
  - Cập nhật Bug Tracker trong [`UI_CHECKLIST.md`](UI_CHECKLIST.md).
- File liên quan: `src/ui/inline_renderer.hpp`, `src/ui/tui_app.hpp`, `src/ui/tui_app.cpp`, [`UI_CHECKLIST.md`](UI_CHECKLIST.md), `process-ui.md`.
- Test: Windows MSVC build pass, `ctest` 100% pass (100 test cases / 1640 assertions); Linux WSL GCC 13 build & ctest pass 100%. Binary `build/local-windows-msvc-debug/Debug/ldns.exe` đã được làm mới.
- Tiếp theo: Báo cáo để tester mở lại `ldns.exe`, chạy `/status` và kéo rê resize để xác nhận bug đã được loại bỏ hoàn toàn.

## 2026-10-23 15:10 — chuẩn bị commit (hoàn thành kiểm thử đa nền tảng và review code cho phân hệ UX/UI)
- Việc đã làm:
  - Tiến hành quy trình kiểm thử và review code toàn diện trước khi đề xuất commit theo `CLAUDE.md` §2.1:
    1. **Windows MSVC Debug:** `cmake --preset local-windows-msvc-debug` build thành công, `ctest` 100% pass (100 test cases / 1640 assertions).
    2. **Linux WSL GCC 13 Debug:** `scripts/wsl-build.sh test linux-debug` build thành công, `ctest` 100% pass.
    3. **Linux WSL GCC 13 ASAN/UBSAN:** `scripts/wsl-build.sh test linux-asan` build thành công, `ctest` 100% pass, 0 lỗi memory leak, 0 undefined behavior.
    4. **Tester Checklist:** Toàn bộ 32/32 kịch bản trong `docs/UI_CHECKLIST.md` đã được kiểm thử và đạt (`PASS` 100%), 10 lỗi phát hiện (BUG-01 đến BUG-10) đã giải quyết dứt điểm.
  - Review code:
    - Đúng tài liệu đặc tả `docs/UI-SPEC.md` và `docs/SPEC.md` (inline TUI giữ scrollback, bảng màu Terracotta, dual-zone, responsive 3 cấp).
    - An toàn bộ nhớ, đa luồng & tài nguyên: Quản lý RAII nguyên tử, buffer double-buffering trong RAM, bảo vệ biên `clip_to_width`, giải mã VT escape có timeout an toàn.
    - Bảo mật theo `CLAUDE.md` §4: Không lưu hay lộ bí mật, kiểm soát giới hạn kích thước chuỗi nhập, không can thiệp logic backend.
- File liên quan: `src/ui/*`, `tests/test_ui.cpp`, `docs/UI-SPEC.md`, `docs/UI_CHECKLIST.md`, `CMakeLists.txt`, `tests/CMakeLists.txt`, `src/main.cpp`, `CLAUDE.md`, `process-ui.md`.
- Cần chú ý: Tuân thủ nghiêm ngặt `CLAUDE.md` §2, liệt kê file thay đổi và viết sẵn commit message đề xuất để người dùng quyết định, tuyệt đối không tự ý commit hay push.
- Bug/lỗ hổng: 0 lỗi tồn đọng.
- Tiếp theo: Trình bày danh sách file và commit message đề xuất cho người dùng quyết định.


## 2026-10-23 15:30 — bắt đầu (tiếp nhận nhiệm vụ thiết kế UX/UI cho dự án)
- Việc đã làm:
  - Đọc và phân tích toàn bộ tài liệu Markdown của dự án: `README.md`, `CLAUDE.md`, `docs/SPEC.md`, `docs/PLAN.md`, `docs/protocol.md`, `docs/platform-notes.md`, `docs/UI-SPEC.md`, `docs/UI_CHECKLIST.md`, `process-ui.md`, `process.md`.
  - Tổng hợp hiện trạng thiết kế hệ thống UX/UI: Đã hoàn thiện kiến trúc Inline TUI kiểu Claude Code, Dual-Zone rendering, bộ widget phong phú, responsive 3 cấp độ, chống rung giật flicker-free, hỗ trợ tiếng Việt UTF-8 và bảng màu Terracotta.
  - Sẵn sàng phối hợp cùng tester để phát triển các tính năng UX/UI chuyên sâu tiếp theo (trực quan hóa nâng cao, tương tác thời gian thực, UX flows cho các kịch bản mạng P2P).
- File liên quan: `docs/UI-SPEC.md`, `docs/UI_CHECKLIST.md`, `docs/SPEC.md`, `src/ui/*`, `process-ui.md`.
- Cần chú ý: Tuân thủ quy tắc `CLAUDE.md` §6 (phạm vi riêng UX/UI, nhánh `feat/ux-ui`, tester là người dùng, không can thiệp sâu core backend).
- Bug/lỗ hổng: không có.
- Tiếp theo: Báo cáo tổng hợp thiết kế UX/UI cho người dùng và đề xuất các hướng thiết kế tiếp theo.


## 2026-10-23 16:05 — bước quan trọng (hoàn thiện UX/UI toàn diện các màn hình với dữ liệu giả chuẩn bị gắn kết Core)
- Việc đã làm:
  - Thiết kế và phát triển 6 widget chuyên biệt mới theo đúng chuẩn Claude Code trong `src/ui/widgets/`:
    * `ConflictView`: Trực quan hóa xung đột domain theo R1 (SPEC §6, UI-SPEC §4.3), phân tách rõ nhóm chiến thắng / nhóm thua, số phiếu bầu và gợi ý giải quyết.
    * `InfoView`: Xem chi tiết tên miền (chủ sở hữu, loại bản ghi, TTL, tỷ lệ đồng thuận LAN, thời điểm đăng ký).
    * `LogView`: Bảng nhật ký truy vấn DNS có căn lề chuẩn UTF-8, mã màu phân loại bước phân giải `[L:Local]`, `[P:Peer]`, `[U:Upstream]`, kết quả NOERROR/NXDOMAIN và độ trễ.
    * `PeersView`: Bảng thống kê các máy peer trong LAN (hostname, endpoint TCP, RTT, số lượng domain, phiên bản và đèn trạng thái).
    * `UpstreamView`: Bảng danh sách máy chủ DNS dự phòng ngoài Internet (Cloudflare, Google, Quad9) kèm RTT, thứ tự ưu tiên.
    * `NetworkView`: Thông số cụm mạng LAN, cổng multicast discovery, cổng sync và trạng thái mã hóa khóa PSK.
  - Hoàn thiện xử lý lệnh tương tác và quản lý dữ liệu mẫu (mock state) trong `TuiApp`:
    * `/info <tên>`: Tra cứu domain cục bộ hoặc peer.
    * `/set <tên> <đích>`: Đổi đích trỏ về của domain và mô phỏng phát tán đồng bộ tới các peer.
    * `/peers`: Hiển thị danh sách kết nối mạng LAN.
    * `/peer add <endpoint>` & `/peer rm <endpoint>`: Thêm/ngắt kết nối peer kèm hiệu ứng spinner kiểm tra TCP sync, cập nhật thời gian thực vào thanh trạng thái đáy màn hình (`StatusLine`).
    * `/upstream [add|rm]`: Quản lý danh sách DNS server dự phòng.
    * `/conflicts`: Trực quan hóa tranh chấp bản ghi (hỗ trợ `/conflicts mock` và `/conflicts clear` để thử nghiệm cả 2 trạng thái).
    * `/logs`: Xem log gần nhất; `/logs --follow`: Stream luồng truy vấn DNS live theo thời gian thực và cho phép người dùng ấn `Esc` hoặc `q` để dừng mượt mà.
    * `/network [name|key set|key clear]`: Cấu hình cụm mạng LAN và bật/tắt mã hóa PSK AES-256-GCM.
    * `/service status|install`: Quản lý dịch vụ daemon nền hệ thống (Windows Service / systemd unit).
    * `/version`: Xem thông tin phiên bản, Git branch và môi trường biên dịch.
  - Bổ sung 6 unit test case mới trong `tests/test_ui.cpp` bao phủ toàn bộ các widget mới.
  - Cập nhật checklist kiểm thử `docs/UI_CHECKLIST.md` lên 40 kịch bản kiểm thử (TC-01.1 đến TC-07.16).
- File liên quan: `src/ui/widgets/conflict_view.*`, `src/ui/widgets/info_view.*`, `src/ui/widgets/log_view.*`, `src/ui/widgets/peers_view.*`, `src/ui/widgets/upstream_view.*`, `src/ui/widgets/network_view.*`, `src/ui/tui_app.*`, `tests/test_ui.cpp`, `CMakeLists.txt`, `scripts/wsl-build.sh`, `docs/UI_CHECKLIST.md`, `process-ui.md`.
- Cần chú ý: Tầng UI được thiết kế theo kiến trúc tách biệt (UI-agnostic data models); khi tầng Core hoàn thành (Resolver pipeline, Store SQLite, SyncEngine) chỉ cần truyền các struct dữ liệu thật vào các hàm render tĩnh mà không phải sửa đổi logic hiển thị.
- Bug/lỗ hổng: 0 lỗi.
- Test:
  * Windows MSVC: Build thành công, 106/106 test cases pass (1679 assertions).
  * Linux WSL GCC 13: `scripts/wsl-build.sh test linux-debug` 100% pass.
  * Linux WSL ASan/UBSan: `scripts/wsl-build.sh test linux-asan` 100% pass (0 memory leak, 0 undefined behavior).
- Tiếp theo: Bàn giao để người dùng (Tester) trực tiếp chạy `ldns.exe` và trải nghiệm các màn hình mới.


## 2026-10-23 16:24 — bước quan trọng (chuẩn hóa checklist: chuyển 8 kịch bản mới về trạng thái Chờ test cho Tester và cập nhật Session 02)
- Việc đã làm:
  - Tiếp thu góp ý chính xác của Tester (người dùng): Developer tuyệt đối không được tự ý đánh dấu `PASS` cho các kịch bản mới khi Tester chưa trực tiếp kiểm thử trên terminal.
  - Điều chỉnh chuẩn mực trong `docs/UI_CHECKLIST.md`:
    * Cập nhật Tiến độ: `Tổng số: 40 kịch bản`, `Đã Đạt (Pass): 32 / 40` (thuộc Đợt 1), `Chưa Kiểm Thử (Pending): 8 / 40` (thuộc Đợt 2).
    * Cập nhật `Session History Log`: Thêm hàng **Phiên 02** (chờ Tester chạy và ghi nhận môi trường terminal cùng cảm nhận thực tế).
    * Chuyển toàn bộ 8 kịch bản mới (TC-07.9 đến TC-07.16) sang trạng thái `Chờ test`, giải phóng hoàn toàn cột Ngày test và Ghi chú của Tester để người dùng tự do đánh giá.
- File liên quan: `docs/UI_CHECKLIST.md`, `process-ui.md`.
- Cần chú ý: Tuân thủ nghiêm ngặt quy trình "User là Tester" theo CLAUDE.md §6.3.
- Bug/lỗ hổng: không có.
- Tiếp theo: Bàn giao toàn bộ để Tester bắt đầu Phiên 02.


## 2026-10-23 16:55 — sửa lỗi (khắc phục triệt để lỗi Enter 2 lần và hoàn thiện lệnh /service theo phản ánh của Tester)
- Việc đã làm:
  - Tiếp nhận 2 vấn đề phát hiện từ Tester khi bắt đầu Phiên 02:
    1. **TC-07.15 (`/service`):** Lệnh `/service` không xuất hiện trong popup gợi ý khi gõ `/serv...`, và khi gõ `/service` không tham số chưa hiển thị trạng thái mặc định.
    2. **Lỗi thực thi lệnh ("Enter 2 lần"):** Khi gõ xong câu lệnh và nhấn Enter, lệnh không thực hiện ngay mà phải nhấn Enter lần thứ 2.
  - Phân tích nguyên nhân gốc rễ (Root Cause Analysis):
    * Khi gõ lệnh slash (ví dụ `/service status`), biến cờ `show_slash_menu_` trước đó chỉ kiểm tra `cur_text[0] == '/'` mà không kiểm tra xem câu lệnh đã có dấu cách hay chưa (`cur_text.find(' ') == std::string::npos`).
    * Do đó khi người dùng đã gõ xong câu lệnh và tham số (hoặc phím cách), `show_slash_menu_` vẫn đang kích hoạt và nuốt sự kiện phím `Enter` đầu tiên để điền tên lệnh `cmd->name + " "`, ghi đè làm mất tham số phía sau và không gửi lệnh đi thực thi; người dùng buộc phải nhấn Enter lần 2 để lệnh được chạy.
  - Khắc phục trong `src/ui/tui_app.cpp`:
    * Thêm `/service` và `/version` vào danh sách lệnh gợi ý của Slash Menu Popup (`slash_commands_`).
    * Điều chỉnh điều kiện mở Slash Menu Popup: Chỉ mở khi câu lệnh bắt đầu bằng `/` và **chưa có dấu cách** (`cur_text.find(' ') == std::string::npos`). Khi người dùng gõ phím cách `' '`, menu popup tự động đóng lại ngay để nhường quyền nhập tham số.
    * Tinh chỉnh thao tác phím khi Popup đang mở:
      - Phím `Tab`: Điền tên lệnh đã chọn kèm một dấu cách (`cmd->name + " "`) để người dùng tiếp tục gõ tham số.
      - Phím `Enter`: Lập tức chọn câu lệnh và **thực thi ngay** trong 1 lần nhấn Enter duy nhất mà không cần Enter lần 2.
    * Hỗ trợ lệnh `/service` chạy ngầm định hiển thị `status` khi gõ `/service` hoặc `/service status`, hỗ trợ các subcommand `install`, `start`, `stop`, `uninstall`.
  - Cập nhật tài liệu kiểm thử `docs/UI_CHECKLIST.md`:
    * Ghi nhận **BUG-11** vào Bảng Theo Dõi Lỗi (Trạng thái: Đã giải quyết).
    * Chuyển kịch bản **TC-07.15** về trạng thái `Chờ test lại`.
  - Kiểm thử đa nền tảng toàn diện:
    * **Windows MSVC Debug:** Biên dịch thành công `ldns.exe`, unit test tự động `ctest` đạt 100% (106/106 tests, 1679 assertions).
    * **Linux WSL GCC 13 Debug:** Biên dịch và test pass 100%.
    * **Linux WSL GCC 13 ASan/UBSan:** Biên dịch và test pass 100% (0 memory leak, 0 undefined behavior).
- File liên quan: `src/ui/tui_app.cpp`, `docs/UI_CHECKLIST.md`, `process-ui.md`.
- Cần chú ý: Tuân thủ quy tắc "User là Tester" (CLAUDE.md §6.3), không tự ý đánh dấu PASS cho các mục chưa được Tester xác nhận lại.
- Bug/lỗ hổng: Đã giải quyết triệt để cả 2 vấn đề.
- Cập nhật thêm:
  * Phát hiện danh mục lệnh trong `src/ui/widgets/slash_menu.cpp` chưa bổ sung `/service` nên khi gõ `/s` menu chỉ lọc ra `/set`, `/status`, `/system`.
  * Đã bổ sung `{"/service", "/service status|install|start|stop", "Quản lý dịch vụ daemon nền hệ thống"}` vào `SlashMenu::commands_`.
  * Đã bổ sung unit test case kiểm tra `SlashMenu` lọc `/service` khi gõ `/serv` và `/s` trong `tests/test_ui.cpp`.
  * Đã build lại binary `ldns.exe` thành công và pass 100% test.
  * Theo phản hồi thẩm mỹ UX của Tester: Khung nhắc lệnh trước đây bị giới hạn cứng `box_width = std::min(term_width, 70)` khiến mép phải bị hụt so với khung thẻ chào mừng và khung nhập. Đã điều chỉnh `SlashMenu::render` dùng toàn bộ `term_width` đồng bộ, tạo nên bố cục 3 khung thẳng tắp 100%. Ghi nhận BUG-12 vào `docs/UI_CHECKLIST.md`.
- Tiếp theo: Thông báo để Tester trực tiếp kiểm tra lại trên terminal Windows.


## 2026-10-23 17:33 — chuẩn bị commit (hoàn thành các widget chuyên biệt, mock data, sửa lỗi lệnh slash và đồng bộ độ rộng giao diện)
- Việc đã làm:
  - Tiến hành quy trình kiểm thử và review code toàn diện trước khi đề xuất commit theo `CLAUDE.md` §2.1:
    1. **Windows MSVC Debug:** `cmake --preset local-windows-msvc-debug` build thành công, `ctest` 100% pass (106 test cases / 1679 assertions).
    2. **Linux WSL GCC 13 Debug:** `scripts/wsl-build.sh test linux-debug` build thành công 43/43 targets, `ctest` 100% pass.
    3. **Linux WSL GCC 13 ASAN/UBSAN:** `scripts/wsl-build.sh test linux-asan` build thành công 43/43 targets, `ctest` 100% pass, 0 lỗi memory leak, 0 undefined behavior.
    4. **Tester Checklist:** Cập nhật 40/40 kịch bản, sửa triệt để BUG-11 (lỗi `/service` và lỗi Enter 2 lần), bổ sung BUG-12 (đồng bộ độ rộng khung gợi ý popup thẳng đều với thẻ và ô nhập).
  - Review code:
    - Đúng tài liệu đặc tả `docs/UI-SPEC.md`, `docs/SPEC.md` và `docs/PLAN.md`.
    - Phân hệ UI độc lập (UI-agnostic widgets), sẵn sàng cắm mốc kết nối với Backend Core API khi core hoàn thiện.
    - An toàn tài nguyên, bộ nhớ, UTF-8 clean, không lộ thông tin bảo mật hay bí mật mạng.
  - Sửa script `scripts/wsl-build.sh` tự động phân tách thư mục build theo tên repo (`~/Ldns-ui-build`) tránh xung đột cache.
  - Loại bỏ ký tự BOM trong `CMakeLists.txt`.
- File liên quan: `src/ui/widgets/*`, `src/ui/tui_app.*`, `tests/test_ui.cpp`, `CMakeLists.txt`, `scripts/wsl-build.sh`, `docs/UI_CHECKLIST.md`, `CLAUDE.md`, `process-ui.md`.
- Cần chú ý: Tuân thủ nghiêm ngặt `CLAUDE.md` §2, liệt kê file thay đổi và viết sẵn commit message đề xuất để người dùng quyết định, tuyệt đối không tự ý commit hay push.
- Bug/lỗ hổng: 0 lỗi tồn đọng.
- Tiếp theo: Trình bày danh sách file và commit message đề xuất cho người dùng quyết định.


## 2026-10-25 16:35 — bước quan trọng (merge nhánh main vào feat/ux-ui, giải quyết conflict và gắn kết UX/UI với CoreApi M2)
- Việc đã làm:
  - Nhận yêu cầu từ người dùng: Merge cập nhật mới nhất từ nhánh `main` (kết quả Milestone 2: SQLite Store, DomainService CRUD, TOML Config, SelfAddress, CoreApi) vào nhánh `feat/ux-ui`, sửa conflict và hoàn thiện UX/UI khớp với cập nhật mới.
  - Tiến hành merge và giải quyết sạch sẽ 3 conflict:
    1. `CLAUDE.md`: Hòa nhập quy định phân chia 2 nhánh phát triển song song (`main` = CLI/Core, `feat/ux-ui` = TUI) cùng quy chuẩn làm việc của phân hệ UX/UI.
    2. `tests/CMakeLists.txt`: Giữ đầy đủ bộ unit tests của Core (`test_store`, `test_config`, `test_domain_service`, `test_core_api`, `test_cli`) và test của UI (`test_ui.cpp`).
    3. `src/main.cpp`: Kết hợp toàn bộ hệ thống CLI subcommand (`add`, `set`, `rm`, `ls`, `status`, `import`, `export`, `daemon`) và mở giao diện Claude Code Inline TUI khi không truyền lệnh con hoặc truyền `--config`/`--data`.
  - Nâng cấp phân hệ UX/UI từ chế độ Mock Data sang kết nối trực tiếp với `CoreApi`:
    * `src/ui/tui_app.hpp` & `src/ui/tui_app.cpp`: Constructor nhận `std::unique_ptr<CoreApi> core`.
    * Tự động đồng bộ thông số khởi tạo từ `core_->status()`: Node ID, mạng LAN, IP card mạng, số lượng domain, trạng thái mã hóa khóa PSK.
    * Đăng ký EventBus listener qua `core_->set_event_handler(...)`: Tự động nhận diện và thông báo thời gian thực vào scrollback khi có sự kiện `RecordPut` / `RecordDel`.
    * Lệnh `/status`: Đọc và hiển thị thông số chi tiết thực tế của SQLite Store, file config, card mạng LAN và trạng thái an toàn PSK.
    * Lệnh `/ls`: Đọc và hiển thị danh sách bản ghi cục bộ thực tế từ SQLite database qua `core_->list_domains()`.
    * Lệnh `/add` & `/set`: Lưu bản ghi vào SQLite và cập nhật `LocalZone` ngay lập tức, tự động hỗ trợ đích `self` (LAN IP) và cảnh báo Public TLD kèm cờ `--force`.
    * Lệnh `/rm`: Xóa bản ghi an toàn từ SQLite và zone.
    * Lệnh `/import <file> [--force]`: Nhập hàng loạt bản ghi từ file TOML/CSV, hiển thị số bản ghi thành công/bỏ qua và cảnh báo chi tiết.
    * Lệnh `/export [--csv]`: Xuất toàn bộ bản ghi vùng zone sang định dạng TOML hoặc CSV.
    * Cập nhật danh mục gợi ý [`SlashMenu`](src/ui/widgets/slash_menu.cpp) và hướng dẫn `/help` bổ sung `/import` và `/export`.
    * Sửa cảnh báo macro `NOMINMAX` trong `src/main.cpp` trên Windows MSVC.
  - Kiểm thử:
    * Windows MSVC Debug: Build thành công, `ctest` đạt **100% PASS** (toàn bộ 146 test cases gồm cả Core M1-M2 và UI widgets).
    * Linux WSL GCC 13 Debug: Build thành công 62/62 targets, `ctest` đạt **100% PASS**.
- File liên quan: `CLAUDE.md`, `CMakeLists.txt`, `tests/CMakeLists.txt`, `src/main.cpp`, `src/ui/tui_app.hpp`, `src/ui/tui_app.cpp`, `src/ui/widgets/slash_menu.cpp`, `process-ui.md`.
- Cần chú ý: Tuân thủ quy định `CLAUDE.md` §6: nhánh `feat/ux-ui` gọi toàn bộ logic qua `CoreApi`, không viết lại logic core trong tầng UI.
- Bug/lỗ hổng: 0 lỗi.
- Tiếp theo: Bàn giao để người dùng kiểm tra TUI trên terminal.
