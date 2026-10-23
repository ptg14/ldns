# Nhật Ký & Danh Mục Kiểm Thử UX/UI — Ldns (UI Checklist Log)

Tài liệu này đóng vai trò như **sổ nhật ký làm việc và danh mục kiểm thử (checklist)** dành cho Tester (người dùng) và Lập trình viên để theo dõi, ghi nhận kết quả và phát hiện lỗi giao diện UX/UI của ứng dụng Ldns trên nhánh `feat/ux-ui`.

---

## 1. Thông Tin Chung & Tiến Độ Kiểm Thử

- **Dự án:** Ldns (Local DNS ngang hàng)
- **Nhánh phát triển:** `feat/ux-ui`
- **Phong cách thiết kế:** Inline Terminal UI (Tương tự Claude Code)
- **Tester:** Người dùng
- **Trạng thái tổng thể:**
  - [X] **Tổng số kịch bản:** 40 kịch bản
  - [x] **Đã Đạt (Pass):** 40 / 40
  - [ ] **Chưa Đạt (Fail):** 0 / 40
  - [ ] **Chưa Kiểm Thử (Pending):** 0 / 40

---

## 2. Nhật Ký Các Phiên Kiểm Thử (Session History Log)

Ghi lại mỗi lần bạn mở ứng dụng lên để test:

| Phiên # | Thời gian (Ngày & Giờ) | Môi trường Terminal (Win Terminal / CMD / WSL...) | Kết quả sơ bộ | Ghi chú / Cảm nhận ban đầu |
|---|---|---|---|---|
| 01 | 2026-10-23 01:50 | Windows Terminal (PowerShell) | Đạt (32/32) | Đã hoàn thành kiểm thử đợt 1 cho các tính năng nền tảng |
| 02 | 2026-10-23 16:15 | Windows Terminal (PowerShell) | Chờ test (8 TC) | Bổ sung 8 kịch bản mới (TC-07.9 -> TC-07.16) cho các màn hình mở rộng; chờ Tester nghiệm thu |
| | | | | |

---

## 3. Danh Mục Checklist Kiểm Thử Chi Tiết

> **Cách dùng:**
> - Đánh dấu `[x]` khi kiểm thử thành công.
> - Điền cột **Kết quả** (`PASS` / `FAIL` / `SKIP`) và ngày test.
> - Nếu phát hiện lỗi hoặc hành vi lạ, ghi chú ngắn vào cột **Ghi chú của Tester** và ghi chi tiết vào **Mục 4 (Bảng theo dõi lỗi)**.

### Nhóm 1: Khởi Động & Màn Hình Chào (Welcome Banner)

| Mã TC | Kịch bản kiểm thử | Thao tác thực hiện | Kết quả mong đợi | Kết quả | Ngày test | Ghi chú của Tester |
|---|---|---|---|:---:|:---:|---|
| **TC-01.1** | Banner khởi động | Chạy `.\build\local-windows-msvc-debug\Debug\ldns.exe` | Xuất hiện thẻ bo tròn có icon `✻`, hiển thị đúng tên Node, IP LAN, trạng thái DNS `:53 ● hoạt động` | `PASS` | 23/10 | |
| **TC-01.2** | Bảo toàn scrollback | Lăn chuột lên trên terminal sau khi mở ứng dụng | Toàn bộ dòng lệnh và kết quả cũ trước khi mở Ldns vẫn còn nguyên vẹn, không bị mất | `PASS` | 23/10 | |
| **TC-01.3** | Thanh trạng thái đáy | Quan sát dòng sát đáy terminal | Hiển thị chấm `●`, số peer, domain, xung đột và gợi ý phím `ctrl+c×2 thoát` | `PASS` | 23/10 | |
| **TC-01.4** | Chế độ chạy headless | Chạy `ldns.exe --headless` | Không mở TUI, chỉ in log một lần rồi thoát êm | `PASS` | 23/10 |  |

---

### Nhóm 2: Ô Nhập Liệu & Thao Tác Soạn Thảo (Input Box)

| Mã TC | Kịch bản kiểm thử | Thao tác thực hiện | Kết quả mong đợi | Kết quả | Ngày test | Ghi chú của Tester |
|---|---|---|---|:---:|:---:|---|
| **TC-02.1** | Placeholder gợi ý | Mở ứng dụng, quan sát ô nhập khi rỗng | Thấy dòng chữ mờ `Thử "/add myapp.lan"...`. Gõ 1 ký tự bất kỳ, placeholder biến mất ngay | `PASS` | 23/10 | |
| **TC-02.2** | Di chuyển con trỏ | Nhập `ldns add test.lan`, dùng phím `←`, `→`, `Home`, `End` | Con trỏ nhảy chính xác đến từng ký tự, chèn và xóa ở giữa chuỗi không vỡ viền hộp | `PASS` | 23/10 | |
| **TC-02.3** | Tiếng Việt có dấu | Gõ tên domain hoặc ghi chú: `ứng-dụng.lan` | Ký tự hiển thị đúng dấu tiếng Việt (UTF-8), con trỏ không bị thụt lùi hay đè chữ | `PASS` | 23/10 | |
| **TC-02.4** | Xóa sạch bằng Esc | Đang gõ dở chuỗi dài, ấn phím `Esc` | Ô nhập lập tức được xóa trắng | `PASS` | 23/10 | |
| **TC-02.5** | Xóa ký tự Backspace/Delete | Đặt con trỏ ở giữa từ và ấn `Backspace` / `Delete` | Ký tự trước/sau con trỏ bị xóa mượt mà, văn bản tự dồn dòng chuẩn xác | `PASS` | 23/10 | |

---

### Nhóm 3: Lịch Sử Lệnh & Tìm Kiếm Lịch Sử (History & Search)

| Mã TC | Kịch bản kiểm thử | Thao tác thực hiện | Kết quả mong đợi | Kết quả | Ngày test | Ghi chú của Tester |
|---|---|---|---|:---:|:---:|---|
| **TC-03.1** | Lịch sử ↑ (Up) | Chạy 2 lệnh `/help` rồi `/status`. Ấn `↑` | Hiện lại lệnh gần nhất `/status`. Ấn `↑` tiếp hiện `/help` | `PASS` | 23/10 | |
| **TC-03.2** | Lịch sử ↓ (Down) | Đang duyệt lịch sử, ấn `↓` | Lùi về lệnh mới hơn; ấn tới cuối cùng trả lại dòng rỗng | `PASS` | 23/10 | |
| **TC-03.3** | Tìm kiếm ngược Ctrl+R | Ấn `Ctrl+R` | Ô nhập chuyển sang `(bsearch) :`. Gõ từ khóa (ví dụ `doc`), tìm thấy `/doctor`. Ấn `Enter` để chọn | `PASS` | 23/10 |  |
| **TC-03.4** | Thoát tìm kiếm Esc | Đang trong chế độ `(bsearch)`, ấn `Esc` | Thoát tìm kiếm và quay lại trạng thái ô nhập ban đầu | `PASS` | 23/10 | |

---

### Nhóm 4: Slash Menu & Autocomplete Gợi Ý Thông Minh

| Mã TC | Kịch bản kiểm thử | Thao tác thực hiện | Kết quả mong đợi | Kết quả | Ngày test | Ghi chú của Tester |
|---|---|---|---|:---:|:---:|---|
| **TC-04.1** | Kích hoạt Menu Slash | Gõ ký tự `/` vào ô nhập rỗng | Popup danh sách lệnh nổi lên ngay phía trên ô nhập với đầy đủ cú pháp và giải thích | `PASS` | 23/10 | |
| **TC-04.2** | Lọc lệnh thời gian thực | Gõ tiếp `re` (thành `/re`) | Popup tự động thu hẹp, chỉ còn hiển thị `/resolve` | `PASS` | 23/10 | |
| **TC-04.3** | Điều hướng menu bằng phím | Dùng phím `↑` và `↓` | Con trỏ `❯` màu cam đất di chuyển giữa các lệnh; cuộn mượt mà | `PASS` | 23/10 | |
| **TC-04.4** | Chọn lệnh bằng Tab/Enter | Đang trỏ lệnh `/resolve`, ấn `Tab` hoặc `Enter` | Lệnh `/resolve ` được tự động điền vào ô nhập, popup đóng lại | `PASS` | 23/10 | |
| **TC-04.5** | Đóng popup bằng Esc | Khi popup đang mở, ấn `Esc` | Popup đóng ngay lập tức, ký tự đã gõ trong ô nhập vẫn giữ nguyên | `PASS` | 23/10 | |

---

### Nhóm 5: Spinner, Tác Vụ Dài & Ngắt Phím Esc

| Mã TC | Kịch bản kiểm thử | Thao tác thực hiện | Kết quả mong đợi | Kết quả | Ngày test | Ghi chú của Tester |
|---|---|---|---|:---:|:---:|---|
| **TC-05.1** | Spinner xoay vòng | Gõ `/resolve test.lan --trace` rồi `Enter` | Biểu tượng `✻` xoay mượt mà, hiển thị đồng hồ thời gian `(0.x s · esc để ngắt)` | `PASS` | 23/10 | |
| **TC-05.2** | Spinner chẩn đoán /doctor | Gõ `/doctor` rồi `Enter` | Thấy dòng trạng thái `✻ Đang chẩn đoán hệ thống…` xoay vòng trước khi in kết quả | `PASS` | 23/10 | |

---

### Nhóm 6: Hộp Thoại Xác Nhận (Dialog)

| Mã TC | Kịch bản kiểm thử | Thao tác thực hiện | Kết quả mong đợi | Kết quả | Ngày test | Ghi chú của Tester |
|---|---|---|---|:---:|:---:|---|
| **TC-06.1** | Hiển thị hộp thoại | Gõ `/system setup` rồi `Enter` | Hộp thoại bo tròn nổi lên với 3 lựa chọn đánh số: `1. Có`, `2. Có, đừng hỏi lại`, `3. Không` | `PASS` | 23/10 | |
| **TC-06.2** | Chọn bằng phím số | Khi hộp thoại đang mở, ấn phím `3` | Hộp thoại đóng ngay lập tức và in thông báo đã chọn lựa chọn 3 | `PASS` | 23/10 | |
| **TC-06.3** | Chọn bằng mũi tên + Enter | Mở lại hộp thoại, dùng `↑`/`↓` di chuyển con trỏ `❯` đến `1. Có` rồi ấn `Enter` | Hộp thoại đóng và áp dụng lựa chọn 1 | `PASS` | 23/10 | |
| **TC-06.4** | Hủy hộp thoại bằng Esc | Khi hộp thoại đang mở, ấn `Esc` | Hộp thoại đóng, scrollback in dòng màu vàng: `⏺ Đã huỷ thao tác hộp thoại` | `PASS` | 23/10 | |

---

### Nhóm 7: Trực Quan Hóa Dữ Liệu & Các Màn Hình Chuyên Biệt

| Mã TC | Kịch bản kiểm thử | Thao tác thực hiện | Kết quả mong đợi | Kết quả | Ngày test | Ghi chú của Tester |
|---|---|---|---|:---:|:---:|---|
| **TC-07.1** | Tổng quan hệ thống `/status` | Gõ `/status` rồi `Enter` | Bố cục thông tin rõ ràng: Node ID, cảnh báo khóa PSK, trạng thái cổng DNS, danh sách peer | `PASS` | 23/10 | |
| **TC-07.2** | Cây phân giải `/resolve --trace` | Gõ `/resolve app.lan --trace` | Cây 3 bước hiển thị với nhánh `├─`, `└─`, phân biệt rõ bước 1 (Local), 2 (Peer), 3 (Upstream) kèm thời gian ms | `PASS` | 23/10 | |
| **TC-07.3** | Bảng chẩn đoán `/doctor` | Gõ `/doctor` | Bảng checklist có các biểu tượng `✔` (xanh), `⚠` (vàng), dòng gợi ý căn lề ngay ngắn | `PASS` | 23/10 | |
| **TC-07.4** | Danh sách domain `/ls` | Gõ `/ls` | Liệt kê các domain hiện có kèm mũi tên trỏ `→` về IP đích | `PASS` | 23/10 | |
| **TC-07.5** | Thêm domain `/add` | Gõ `/add api.lan` | Phản hồi dạng bullet `⏺ Đã thêm...` kèm dòng thụt lề `⎿ TTL 60s · đã đồng bộ...` | `PASS` | 23/10 | |
| **TC-07.6** | Xóa domain `/rm` | Gõ `/rm api.lan` | Thông báo xóa thành công domain | `PASS` | 23/10 | |
| **TC-07.7** | Danh sách peer `/peers` | Gõ `/peers` | Hiển thị bảng các máy peer: IP, RTT, số domain, trạng thái `● online` | `PASS` | 23/10 | |
| **TC-07.8** | Kiểm tra xung đột `/conflicts` | Gõ `/conflicts` | Thông báo không có xung đột hoặc liệt kê các nhóm tranh chấp | `PASS` | 23/10 | |
| **TC-07.9** | Chi tiết domain `/info <tên>` | Gõ `/info web.lan` | Hiển thị bảng chi tiết: chủ sở hữu, số phiếu 2/3, nhóm chiến thắng theo R1 | `PASS` | 23/10 | |
| **TC-07.10** | Cập nhật đích `/set` | Gõ `/set app.lan 192.168.1.99` | Thông báo cập nhật thành công và phát tán tới các peer trong LAN | `PASS` | 23/10 | |
| **TC-07.11** | Quản lý peer `/peer` | Gõ `/peer add 192.168.1.88:5391` rồi `/peer rm 192.168.1.88` | Spinner kiểm tra TCP sync, thêm peer vào danh sách và cập nhật status line | `PASS` | 23/10 | |
| **TC-07.12** | Quản lý upstream `/upstream` | Gõ `/upstream` rồi `/upstream add 1.0.0.1` | Hiển thị bảng DNS upstream kèm RTT, độ ưu tiên và trạng thái hoạt động | `PASS` | 23/10 | |
| **TC-07.13** | Nhật ký trực tiếp `/logs --follow` | Gõ `/logs --follow` | Luồng sự kiện query live đẩy lên scrollback, ấn Esc/q để dừng an toàn | `PASS` | 23/10 | |
| **TC-07.14** | Cấu hình mạng `/network` | Gõ `/network` rồi `/network key set secret123` | Hiển thị thông số cụm LAN, bật mã hóa AES-256-GCM bảo vệ gói discovery | `PASS` | 23/10 | |
| **TC-07.15** | Dịch vụ nền `/service` | Gõ `/service status` | Hiển thị trạng thái daemon service (Windows Service / systemd) | `PASS` | 23/10 | |
| **TC-07.16** | Xem phiên bản `/version` | Gõ `/version` | Hiển thị thông tin phiên bản, tiêu chuẩn C++20, nhánh feat/ux-ui và nền tảng | `PASS` | 23/10 | |

---

### Nhóm 8: Đổi Kích Thước (Resize) & Xóa Màn Hình

| Mã TC | Kịch bản kiểm thử | Thao tác thực hiện | Kết quả mong đợi | Kết quả | Ngày test | Ghi chú của Tester |
|---|---|---|---|:---:|:---:|---|
| **TC-08.1** | Co giãn cửa sổ (Resize) | Kéo chuột thay đổi kích thước cửa sổ Windows Terminal | Vùng động tự co giãn theo chiều ngang mới, không xuất hiện ký tự rác | `PASS` | 23/10 |  |
| **TC-08.2** | Xóa màn hình Ctrl+L | Ấn tổ hợp phím `Ctrl+L` | Màn hình terminal được làm sạch hoàn toàn; Banner chào mừng và ô nhập được vẽ lại gọn gàng | `PASS` | 23/10 | |

---

### Nhóm 9: Thoát Ứng Dụng An Toàn (Graceful Exit)

| Mã TC | Kịch bản kiểm thử | Thao tác thực hiện | Kết quả mong đợi | Kết quả | Ngày test | Ghi chú của Tester |
|---|---|---|---|:---:|:---:|---|
| **TC-09.1** | Cảnh báo Ctrl+C lần 1 | Khi ô nhập rỗng, ấn `Ctrl+C` 1 lần | Không thoát đột ngột; thanh trạng thái hiện cảnh báo vàng: `Nhấn Ctrl+C lần nữa để thoát` | `PASS` | 23/10 | |
| **TC-09.2** | Thoát an toàn Ctrl+C lần 2 | Ấn tiếp `Ctrl+C` trong vòng 1.5 giây | Ứng dụng thoát êm ái, khôi phục lại con trỏ terminal ban đầu, in dòng `Ldns đã dừng an toàn.` | `PASS` | 23/10 | |
| **TC-09.3** | Thoát nhanh bằng Ctrl+D | Mở lại ứng dụng, ô nhập rỗng, ấn `Ctrl+D` | Ứng dụng thoát ngay lập tức | `PASS` | 23/10 | |
| **TC-09.4** | Thoát bằng lệnh `/exit` | Gõ `/exit` rồi `Enter` | Ứng dụng dừng và thoát sạch sẽ | `PASS` | 23/10 | |

---

## 4. Sổ Theo Dõi Lỗi & Đề Xuất Cải Thiện UX (Bug & Feedback Tracker)

Khi phát hiện điểm bất tiện hoặc lỗi trong quá trình tự test, hãy ghi lại tại bảng dưới đây:

| Mã Bug/Góp ý | Kịch bản liên quan | Mô tả hiện tượng / Đề xuất cải thiện | Mức độ (Thấp / Vừa / Cao) | Trạng thái (Mới / Đang sửa / Đã giải quyết) | Ghi chú xử lý |
|---|---|---|:---:|:---:|---|
| **BUG-01** | **TC-02.1 / TC-02.2** | Khi gõ ký tự hay ấn `->`, `backspace`, phần trên khung ô nhập bị xóa dần khiến khung bò lên đỉnh CLI | **Nghiêm trọng (Cao)** | **Đã giải quyết** | Lỗi vị trí con trỏ `cursor_y_` trong `InlineRenderer` và parser phím VT/Escape; đã sửa tracking và buffer ANSI |
| **BUG-02** | **TC-01.4** | Chạy `ldns.exe --headless` bị lỗi font console in ra `ΓÇö` thay vì dấu gạch ngang | **Thấp** | **Đã giải quyết** | Thiết lập `SetConsoleOutputCP(CP_UTF8)` ngay đầu `main()` và chuẩn hóa dấu gạch ngang ASCII `-` |
| **BUG-03** | **TC-03.3** | Tìm kiếm `Ctrl+R`: con trỏ đặt sai vị trí `(b\|search)`, không tìm được lệnh và ấn Enter xóa ô nhập | **Vừa** | **Đã giải quyết** | Sửa `cursor_visual_col()` căn đúng vào backtick, tìm kiếm cả lịch sử lẫn danh mục lệnh built-in, Enter chọn lệnh |
| **BUG-04** | **TC-08.1** | Co giãn cửa sổ terminal (resize) không mượt: UI giữ nguyên khi phóng to, vỡ khung/tràn dòng `ctrl+c×2 thoát` khi thu nhỏ | **Nghiêm trọng (Cao)** | **Đã giải quyết** | Thêm cơ chế polling 25ms + `KeyType::Resize`; đồng bộ độ rộng `InputBox` và `StatusLine` theo `(cols - 1)`; thêm `clip_to_width` bảo vệ khung |
| **BUG-05** | **TC-07.3** | Lệnh `/doctor` các cột chưa căn lề thẳng hàng khi có tiếng Việt có dấu | **Thấp** | **Đã giải quyết** | Chuyển `padded_name.size()` sang `CharWidth::display_width()` chuẩn UTF-8 display width |
| **BUG-06** | **TC-08.1** | Thẻ thông tin chào mừng bị vỡ khung/tràn dòng khi thu nhỏ; xuất hiện nhiều khung rác giữa thẻ và ô nhập khi resize | **Nghiêm trọng (Cao)** | **Đã giải quyết** | Thiết kế lại `WelcomePanel` responsive 3 cấp độ rộng; tích hợp thẻ vào dynamic zone lúc khởi động; `prepare_for_resize()` triệt tiêu hoàn toàn khung rác |
| **BUG-07** | **TC-02.1 / TC-08.1** | Màn hình nháy liên tục gây đau mắt khi gõ ký tự hoặc khi kéo resize cửa sổ | **Nghiêm trọng (Cao)** | **Đã giải quyết** | Loại bỏ `\033[J` và `\033[2J`; áp dụng atomic double-buffering ghi đè tại chỗ bằng `\r` + `\033[K`; thêm `render_full_screen()` và debounce 30ms lúc resize |
| **BUG-08** | **TC-01.1 / TC-08.1** | Khung thẻ thông tin và khung ô nhập bị lệch mép phải 1 cột (khung nhập dài hơn khung thẻ) | **Vừa** | **Đã giải quyết** | Bỏ phép trừ trùng lặp `term_width - 1` trong `WelcomePanel::render`; đồng bộ độ rộng cả 2 khung thẳng tắp |
| **BUG-09** | **TC-07.1 / TC-07.3** | Các ký hiệu trạng thái (`✔`, `⚠`, `✖`, `●`, `⏺`) nằm quá sát ký tự chữ gây dính chữ | **Vừa** | **Đã giải quyết** | Chuẩn hóa đệm 2 khoảng trắng sau tất cả các ký hiệu thông tin trên toàn bộ giao diện |
| **BUG-10** | **TC-08.1** | Sau khi chạy lệnh (ví dụ `/status`), resize cửa sổ làm thẻ thông tin bị vỡ và sinh nhiều khung rác | **Nghiêm trọng (Cao)** | **Đã giải quyết** | Quản lý lịch sử scrollback bằng `scrollback_entries_`; khi resize gọi `redraw_full_view()` tự động re-render lại thẻ chào mừng theo độ rộng mới và vẽ đè nguyên tử từ `\033[1;1H`, triệt tiêu hoàn toàn khung rác |
| **BUG-11** | **TC-04.1 / TC-07.15** | Thiếu gợi ý lệnh `/service` trong slash menu popup; khi gõ lệnh xong phải nhấn Enter 2 lần mới thực hiện được | **Nghiêm trọng (Cao)** | **Đã giải quyết** | Thêm `/service` vào danh mục lệnh; đóng popup gợi ý ngay khi gõ dấu cách `' '`; cho phép nhấn Enter lập tức thực thi lệnh khi menu đang mở mà không cần Enter lần 2 (Tab để autocomplete tham số) |
| **BUG-12** | **TC-04.1** | Khung nhắc lệnh (slash menu popup) trước đây bị giới hạn tối đa 70 cột nên bị hụt ngắn hơn so với khung thẻ và khung nhập | **Thấp (Thẩm mỹ UX)** | **Đã giải quyết** | Đồng bộ `box_width` của `SlashMenu::render` theo chiều rộng toàn phần của terminal, giúp mép trái và mép phải của khung nhắc lệnh thẳng tắp tuyệt đối với khung thẻ và khung nhập |

---

## 5. Hướng Dẫn Chạy & Khởi Động Test

1. **Build bản mới nhất (nếu có thay đổi code):**
   ```powershell
   cmake --build --preset local-windows-msvc-debug
   ```

2. **Chạy ứng dụng TUI để test:**
   ```powershell
   .\build\local-windows-msvc-debug\Debug\ldns.exe
   ```

3. **Chạy toàn bộ unit test tự động:**
   ```powershell
   ctest --preset local-windows-msvc-debug --output-on-failure
   ```
