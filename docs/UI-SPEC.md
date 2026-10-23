# Ldns — Tài liệu Đặc tả Thiết kế UX/UI (UI-SPEC)

Tài liệu này là đặc tả chi tiết về Trải nghiệm Người dùng (UX) và Giao diện Dòng lệnh/Giao diện Terminal Tương tác (CLI/TUI) của dự án **Ldns**.
Nguồn tham chiếu: `docs/SPEC.md` (§10, §11, §13), `docs/PLAN.md` (Spike S1, Milestone M7).

---

## 1. Triết lý Thiết kế (Design Philosophy)

Giao diện của Ldns được lấy cảm hứng trực tiếp từ phong cách tương tác của **Claude Code**, tối ưu hóa cho lập trình viên và quản trị mạng LAN:

1. **Inline Terminal UI (Không sử dụng Alternate Screen):**
   - Không chuyển sang chế độ toàn màn hình kín (alt-screen buffer) như `htop`, `vim` hay `nano`.
   - Giữ nguyên lịch sử cuộn (scrollback) của terminal. Người dùng có thể lăn chuột lên để xem lại mọi log, mọi truy vấn và thao tác trước đó.
   - Khi thoát Ldns, toàn bộ kết quả làm việc vẫn lưu lại trên terminal chứ không biến mất.

2. **Cấu trúc Hiển thị 2 Vùng (Dual-Zone Rendering):**
   - **Vùng Bất Biến (Immutable Scrollback Zone - Phía trên):** Các lệnh người dùng đã thực thi (`> /add ...`), kết quả trả về (`⏺ Đã thêm ...`), thông báo chi tiết (`⎿ ...`), và sự kiện mạng thời gian thực (`⏺ Peer mới: ...`) được đẩy liên tục lên dòng scrollback.
   - **Vùng Động (Dynamic Active Zone - Ở đáy):** Chỉ chiếm một số dòng cố định dưới cùng của terminal. Vùng này chứa ô nhập lệnh (Input Box), menu gợi ý (Slash Menu popup) và thanh trạng thái (Status Line). Vùng này được vẽ đè tại chỗ (cursor repositioning & line diffing), loại bỏ hiện tượng chớp nháy (flicker-free).

3. **Chống phân tâm & Thông tin có thứ bậc:**
   - Sử dụng tiền tố biểu tượng nhất quán:
     * `>` : Dòng lệnh người dùng nhập.
     * `⏺` : Kết quả thực thi hoặc sự kiện nền từ mạng LAN.
     * `⎿` : Thông tin chi tiết, metadata bổ trợ thụt lề.
     * `✻` : Spinner đang xử lý hoặc biểu tượng tiêu đề.
     * `●` : Đèn tín hiệu trạng thái (Xanh = Hoạt động, Đỏ = Lỗi, Vàng = Cảnh báo).
     * `❯` : Con trỏ lựa chọn trong menu danh sách.

4. **Tôn trọng môi trường & Phục hồi lỗi:**
   - Mọi thông báo lỗi phải có **[Lý do cụ thể] + [Gợi ý khắc phục bằng lệnh hoặc hành động]**.
   - Hỗ trợ đầy đủ tiếng Việt có dấu (UTF-8) và tiếng Anh theo biến cấu hình `ui.language`.
   - Tự động nhận diện `NO_COLOR`, tự chuyển về ký tự chuẩn ASCII khi terminal không hỗ trợ Unicode.

---

## 2. Bảng Màu, Typography & Tokens

### 2.1 Bảng màu (Color Palette)

Ldns sử dụng màu **Cam đất (Terracotta / Warm Amber)** làm màu nhận diện chủ đạo, kết hợp với các màu ngữ nghĩa chuẩn:

| Token | Tên màu | Giá trị Hex / ANSI 24-bit | ANSI 256 | Vai trò |
|---|---|---|---|---|
| `accent` | Terracotta (Cam đất) | `#D97706` / `rgb(217, 119, 6)` | `214` | Viền ô nhập khi focus, con trỏ `❯`, spinner `✻`, nút bấm active |
| `primary` | Light Amber | `#F59E0B` / `rgb(245, 158, 11)` | `215` | Tiêu đề chào mừng, highlight tên domain, lệnh slash |
| `success` | Soft Green | `#10B981` / `rgb(16, 185, 129)` | `35` | Đèn online `●`, trạng thái đồng bộ hoàn tất, bước phân giải thành công |
| `warning` | Warm Yellow | `#FBBF24` / `rgb(251, 191, 36)` | `221` | Cảnh báo chưa đặt khóa PSK, hòa phiếu, thời gian trễ cao |
| `error` | Crimson Red | `#EF4444` / `rgb(239, 68, 68)` | `196` | Bind port 53 thất bại, timeout, SERVFAIL, xung đột gay gắt |
| `text` | Pure White / Off-white | `#F9FAFB` / `rgb(249, 250, 251)` | `231` | Văn bản chính, dữ liệu địa chỉ IP, câu lệnh đã gõ |
| `muted` | Dim / Slate Gray | `#6B7280` / `rgb(107, 114, 128)` | `243` | Gợi ý placeholder, thời gian ms/µs, viền hộp thoại phụ, TTL |
| `border` | Subdued Gray | `#4B5563` / `rgb(75, 85, 99)` | `240` | Viền mặc định của ô nhập và hộp thông báo |

### 2.2 Fallback ASCII khi không có Unicode hoặc `NO_COLOR`

| Thành phần | Unicode Mode (Mặc định) | ASCII Fallback Mode | NO_COLOR Mode |
|---|---|---|---|
| Spinner | `✻`, `✼`, `✽`, `✹` | `*`, `+`, `o`, `.` | Ký tự Unicode/ASCII, không mã màu ANSI |
| Kết quả | `⏺` (bullet) | `o` | `o` |
| Thụt lề | `⎿` | `\-` hoặc `\|_` | `\|_` |
| Con trỏ chọn | `❯` | `>` | `>` |
| Trạng thái | `●` | `[+]` / `[x]` | `[+]` / `[x]` |
| Viền hộp | `╭──╮`, `╰──╯`, `│` | `+--+`, `+--+`, `\|` | `+--+`, `+--+`, `\|` |

---

## 3. Cấu trúc Các Thành Phần Giao Diện (UI Components)

### 3.1 Khối Chào Mừng (Welcome Banner)
Được in ra một lần duy nhất lúc khởi chạy Ldns TUI:
```text
╭──────────────────────────────────────────────────────────╮
│ ✻ Chào mừng đến với Ldns!                                │
│                                                          │
│   /help để xem lệnh · /add app.lan để tạo domain đầu tiên │
│                                                          │
│   node:  thang-pc  (a3f9…)       mạng: default           │
│   ip:    192.168.1.10             dns:  :53  ● hoạt động │
╰──────────────────────────────────────────────────────────╯
```
- Khung viền bo tròn nhẹ bằng ký tự UTF-8 mảnh.
- Tự động hiển thị tên máy (`node`), ID rút gọn 6 ký tự hex, tên mạng LAN, địa chỉ IP LAN tự nhận diện, và trạng thái lắng nghe DNS port 53.

### 3.2 Hộp Nhập Lệnh (Input Box)
Nằm cố định ở vùng động dưới đáy màn hình:
```text
╭──────────────────────────────────────────────────────────╮
│ > Thử "/add myapp.lan"                                   │
╰──────────────────────────────────────────────────────────╯
```
- Dòng nhắc lệnh bắt đầu bằng ký tự `> ` màu nhấn (Terracotta).
- Text gợi ý (Placeholder) màu mờ (`muted`), biến mất ngay khi người dùng gõ ký tự đầu tiên. Placeholder có thể luân chuyển ngẫu nhiên các mẹo sử dụng khi nhàn rỗi.
- Hỗ trợ gõ nhiều dòng (Multi-line): Khi kết thúc dòng bằng dấu `\` + `Enter` hoặc ấn tổ hợp `Shift+Enter`, hộp nhập sẽ tự mở rộng thêm dòng:
```text
╭──────────────────────────────────────────────────────────╮
│ > /add api.lan \                                         │
│     --type A --ttl 120                                   │
╰──────────────────────────────────────────────────────────╯
```
- Cho phép copy-paste nhiều dòng hoặc dán văn bản có cấu trúc an toàn (Bracketed Paste Mode).

### 3.3 Menu Lệnh Slash (Slash Autocomplete Popup)
Khi người dùng gõ ký tự `/`, một danh sách popup xuất hiện ngay phía trên ô nhập:
```text
╭──────────────────────────────────────────────────────────╮
│ ❯ /add       Tạo domain mới trỏ về IP máy này hoặc đích  │
│   /conflicts Xem các domain đang có xung đột trên mạng   │
│   /doctor    Chẩn đoán cổng 53, firewall, quyền hệ thống │
│   /info      Xem thông tin chi tiết, số phiếu của domain │
│   /logs      Xem nhật ký truy vấn DNS theo thời gian thực│
│   /ls        Liệt kê các domain trong mạng LAN           │
│   /peers     Danh sách các máy khác trong mạng           │
│   /resolve   Thử phân giải một tên miền (kèm --trace)    │
│   /status    Tổng quan trạng thái mạng và an toàn        │
╰──────────────────────────────────────────────────────────╯
╭──────────────────────────────────────────────────────────╮
│ > /ad                                                    │
╰──────────────────────────────────────────────────────────╯
  ↑/↓ điều hướng · tab/enter chọn · esc đóng
```
- **Lọc tức thì (Real-time Filter):** Người dùng gõ tiếp `/ad`, danh sách tự động thu hẹp lại chỉ còn `/add`.
- **Điều hướng phím:** Phím mũi tên `↑`/`↓` di chuyển con trỏ `❯`. Phím `Tab` hoặc `Enter` điền lệnh vào ô nhập. Phím `Esc` đóng menu gợi ý.
- **Gợi ý tham số thông minh (Contextual Autocomplete):**
  * Sau `/rm `, ấn `Tab` sẽ gợi ý danh sách các domain của chính mình đang sở hữu.
  * Sau `/info `, ấn `Tab` sẽ gợi ý toàn bộ domain đang có trong mạng LAN.
  * Sau `/peer rm `, ấn `Tab` sẽ gợi ý IP của các peer đang kết nối.

### 3.4 Con Trỏ Chờ Xoay Vòng (Spinner)
Khi người dùng chạy một lệnh hoặc một truy vấn mất hơn 300 ms (ví dụ: truy vấn upstream hoặc quét mạng):
```text
✻ Đang phân giải myapp.lan… (1.2s · esc để ngắt)
```
- Biểu tượng `✻` xoay vòng liên tục theo chu kỳ 80 ms (`✻` → `✼` → `✽` → `✹`).
- Luôn hiển thị thời gian đã trôi qua và hướng dẫn phím `Esc` để ngắt tác vụ, giữ người dùng luôn làm chủ tiến trình.

### 3.5 Hộp Thoại Xác Nhận Đánh Số (Numbered Confirmation Dialog)
Đối với các thao tác ảnh hưởng đến hệ thống (ví dụ: sửa DNS của card mạng, xóa bản ghi gốc, ghi đè domain có cảnh báo TLD công cộng):
```text
╭───────────────────────────────────────────────╮
│ Đặt DNS hệ thống về 127.0.0.1?                │
│ Bản gốc sẽ được sao lưu và khôi phục bằng     │
│ /system restore.                              │
│                                               │
│ ❯ 1. Có                                       │
│   2. Có, đừng hỏi lại                         │
│   3. Không                                    │
╰───────────────────────────────────────────────╯
```
- Điều hướng bằng mũi tên `↑`/`↓` rồi `Enter`, hoặc ấn phím số trực tiếp (`1`, `2`, `3`) để phản hồi ngay lập tức mà không cần di chuyển con trỏ.

### 3.6 Thanh Trạng Thái Chân Trang (Status Line)
Nằm sát đáy màn hình bên dưới hộp nhập lệnh:
```text
  ● 5 peer · 14 domain · 0 xung đột         ctrl+c×2 thoát
```
- Phía bên trái:
  * Đèn tròn `●` (màu xanh lá: mạng bình thường; màu vàng: có cảnh báo/xung đột; màu đỏ: mất mạng/lỗi bind).
  * Bộ đếm tóm tắt: số lượng peer đang online, tổng số domain trong mạng, số lượng tên miền đang bị xung đột phiếu bầu.
- Phía bên phải: Gợi ý phím tắt linh hoạt theo trạng thái hiện tại (`ctrl+c×2 thoát`, `esc để huỷ`, `tab để chọn`).

### 3.7 Dòng Kết Quả & Sự Kiện Đẩy Lên Scrollback
Khi một lệnh hoàn tất:
```text
> /add app.lan

⏺ Đã thêm app.lan → 192.168.1.10 (self)
  ⎿ TTL 60s · đã đồng bộ tới 4 peer trong 38 ms
```
Khi máy khác trong mạng có thay đổi (chèn mượt mà vào scrollback mà không làm ảnh hưởng ô gõ của người dùng):
```text
⏺ Peer mới: minh-laptop (192.168.1.22) · 12 domain
⏺ minh-laptop vừa thêm db.lan → 192.168.1.22
```

---

## 4. Các Màn Hình & Trực Quan Hóa Dữ Liệu Chuyên Biệt

### 4.1 Lệnh `/status` (Trang Tổng Quan Hệ Thống)
```text
> /status

⏺ Tổng quan Ldns (node: thang-pc · a3f9e1)
  ⎿ Phiên bản: 0.1.0 (Windows x64) · Khởi động: 42 phút trước
  ⎿ Mạng: "default" · Cổng discovery: 5390 · Cổng sync: 5391
  ⎿ Cảnh báo bảo mật: Chưa đặt khóa mạng PSK (dùng /network key set để bảo mật)

  Dịch vụ DNS:
    ● 127.0.0.1:53       Hoạt động (4,821 truy vấn · avg 0.8ms)
    ● 192.168.1.10:53    Hoạt động (LAN interface: Wi-Fi)

  Kết nối Peer (3 peer online):
    minh-laptop (192.168.1.22)   RTT 2.1ms   8 domain   v0.1.0
    nam-desktop (192.168.1.45)   RTT 1.4ms  15 domain   v0.1.0
    pi-server   (192.168.1.100)  RTT 0.9ms   3 domain   v0.1.0

  Upstream Fallback:
    1.1.1.1:53 (Cloudflare)      RTT 14ms    ● OK
    8.8.8.8:53 (Google)          RTT 18ms    ● OK
```

### 4.2 Lệnh `/resolve <tên> --trace` (Cây Phân Giải DNS 3 Bước)
```text
> /resolve myapp.lan --trace

✻ Đang phân giải myapp.lan…

⏺ Kết quả: 192.168.1.22 (A) · TTL 30s
  ⎿ Quá trình phân giải (tổng thời gian: 18.4 ms):
    ├─ [1. LocalZone]    myapp.lan: Không tìm thấy (NXDOMAIN cục bộ) [0.1 ms]
    ├─ [2. PeerResolver] myapp.lan: Tìm thấy 1 ứng viên hợp lệ      [18.2 ms]
    │   ├─ minh-laptop (192.168.1.22): Đã claim (1 phiếu, nhóm A)
    │   └─ Trả lời: 192.168.1.22 (RTT 17.8 ms)
    └─ [3. Upstream]     Bỏ qua (đã giải quyết ở bước 2)
```

Khi phân giải một tên miền ngoài Internet:
```text
> /resolve github.com --trace

⏺ Kết quả: 20.205.243.166 (A) · TTL 60s
  ⎿ Quá trình phân giải (tổng thời gian: 15.2 ms):
    ├─ [1. LocalZone]    github.com: Bỏ qua (không có trong local zone)
    ├─ [2. PeerResolver] github.com: Bỏ qua (không có peer nào claim)
    └─ [3. Upstream]     Hỏi upstream 1.1.1.1:53                    [15.1 ms]
        └─ Trả lời: 20.205.243.166 (NOERROR, AA=0)
```

### 4.3 Lệnh `/conflicts` (Trực Quan Hóa Xung Đột Tên Miền)
```text
> /conflicts

⚠ Phát hiện 1 domain có xung đột bản ghi trên mạng LAN:

  Domain: web.lan (Loại: A)
  ──────────────────────────────────────────────────────────────────
  Nhóm 1 (Chiến thắng - 2 phiếu):
    Đích: 192.168.1.10 (thang-pc - bạn), 192.168.1.45 (nam-desktop)
    Trạng thái: Đang được phân giải cho toàn mạng
  Nhóm 2 (Thua - 1 phiếu):
    Đích: 192.168.1.22 (minh-laptop)
  ──────────────────────────────────────────────────────────────────
  Gợi ý: Dùng "/info web.lan" để xem thời điểm claim hoặc đổi tên miền.
```

### 4.4 Lệnh `/doctor` (Kiểm Tra Sức Khỏe Toàn Diện)
```text
> /doctor

✻ Đang kiểm tra hệ thống…

⏺ Kết quả chẩn đoán Ldns:
  ✔ Cổng DNS 53:           Đã bind thành công trên 127.0.0.1 và 192.168.1.10
  ✔ Quyền hệ thống:        Đang chạy với quyền Administrator/CAP_NET_BIND_SERVICE
  ✔ Tường lửa (Firewall):  Đã mở cổng UDP 53, UDP 5390, TCP 5391 cho subnet LAN
  ✔ Multicast Discovery:   Nhận beacon bình thường trên interface Wi-Fi
  ⚠ DNS Hệ điều hành:      Chưa cấu hình DNS trỏ về 127.0.0.1
                           Gợi ý: Chạy lệnh "/system setup" để tự động kích hoạt
  ✔ Upstream DNS:          Kết nối tốt tới 1.1.1.1 (14 ms) và 8.8.8.8 (18 ms)

Không có lỗi nghiêm trọng. Hệ thống sẵn sàng phục vụ.
```

### 4.5 Lệnh `/logs --follow` (Xem Nhật Ký Truy Vấn DNS Live)
```text
> /logs --follow
(Ấn Esc để dừng theo dõi)

14:02:11.104  127.0.0.1       A      app.lan       [L:Local]    NOERROR    180 µs
14:02:12.350  192.168.1.50    A      api.lan       [P:Peer]     NOERROR   2.4 ms
14:02:15.891  127.0.0.1       AAAA   google.com    [U:Upstream] NOERROR  16.1 ms
14:02:18.012  192.168.1.22    A      test.invalid  [U:Upstream] NXDOMAIN 12.0 ms
```

---

## 5. Tổ Hợp Phím Tắt & Hành Vi Tương Tác (Keymap & Gestures)

| Phím tắt | Ngữ cảnh | Hành vi UX |
|---|---|---|
| `Enter` | Ô nhập có lệnh | Gửi lệnh thực thi |
| `Enter` | Đang mở Slash Menu / Dialog | Chọn mục đang trỏ bởi `❯` |
| `\` + `Enter` | Đang trong ô nhập | Xuống dòng, mở rộng hộp nhập nhiều dòng |
| `Shift` + `Enter` | Terminal hỗ trợ modifyOtherKeys | Xuống dòng nhiều dòng |
| `Esc` | Tác vụ đang chạy (>300ms) | Huỷ tác vụ ngay lập tức (`Canceled`) |
| `Esc` | Slash Menu đang mở | Đóng Slash Menu, giữ nguyên chữ đã gõ |
| `Esc` | Ô nhập có chữ | Xóa sạch nội dung trong ô nhập |
| `Tab` | Khi đang gõ lệnh/tham số | Hoàn thành từ tiếp theo (Autocomplete) |
| `↑` (Up Arrow) | Ô nhập rỗng | Lấy lại lệnh trước đó từ lịch sử (History) |
| `↓` (Down Arrow) | Đang duyệt lịch sử | Di chuyển tới lệnh mới hơn |
| `↑` / `↓` | Đang mở Slash Menu / Dialog | Di chuyển con trỏ chọn `❯` |
| `Ctrl + R` | Ô nhập | Bật thanh tìm kiếm lịch sử lệnh (Reverse search) |
| `Ctrl + C` (lần 1) | Ô nhập có chữ | Huỷ dòng hiện tại, xuống dòng mới |
| `Ctrl + C` (lần 1) | Ô nhập rỗng | Hiển thị cảnh báo: `Nhấn Ctrl+C lần nữa để thoát` |
| `Ctrl + C` (lần 2) | Trong vòng 1.5 giây | Thoát Ldns an toàn, khôi phục terminal |
| `Ctrl + D` | Ô nhập hoàn toàn rỗng | Thoát Ldns ngay lập tức |
| `Ctrl + L` | Bất kỳ lúc nào | Xóa sạch màn hình scrollback, vẽ lại Banner & Input |
| `Home` / `End` | Ô nhập | Về đầu dòng / cuối dòng |
| `Ctrl + ←` / `→` | Ô nhập | Nhảy con trỏ theo từng từ (Word jump) |
| `?` | Ô nhập rỗng | Mở nhanh bảng trợ giúp phím tắt |

---

## 6. Danh Mục Kiểm Thử UX/UI Dành Cho Tester (Testing Checklist)

Bộ checklist này được thiết kế để **Tester** (người dùng) trực tiếp kiểm thử bằng tay từng khía cạnh giao diện, đảm bảo chuẩn mực UX Claude Code.

### Nhóm 1: Khởi động & Màn hình Chào (Welcome Banner)
- [ ] **TC-01.1 (Khởi động mặc định):** Gõ `ldns` không tham số. Kiểm tra màn hình xuất hiện Banner chào mừng có viền bo tròn, hiển thị đúng tên Node, IP LAN, trạng thái DNS port 53.
- [ ] **TC-01.2 (Scrollback bảo toàn):** Lăn chuột lên trên terminal, xác nhận các dòng lệnh trước khi mở Ldns vẫn còn nguyên vẹn, không bị xóa hay đè vào buffer ẩn.
- [ ] **TC-01.3 (Hiển thị Status Line):** Dưới đáy màn hình có thanh trạng thái hiển thị số lượng peer, số domain, số xung đột và gợi ý phím tắt `ctrl+c×2 thoát`.

### Nhóm 2: Ô Nhập Liệu & Thao Tác Phím (Input Box)
- [ ] **TC-02.1 (Placeholder gợi ý):** Khi ô nhập rỗng, thấy dòng mờ gợi ý (ví dụ: `Thử "/add myapp.lan"`). Gõ 1 ký tự bất kỳ, placeholder lập tức biến mất.
- [ ] **TC-02.2 (Di chuyển con trỏ):** Nhập `ldns add test.lan`, dùng phím mũi tên `←`/`→`, `Home`, `End` để di chuyển con trỏ. Thêm/xóa ký tự ở giữa chuỗi mượt mà, không vỡ layout.
- [ ] **TC-02.3 (Nhập nhiều dòng):** Nhập `dòng 1 \` rồi ấn `Enter`. Ô nhập tự động tăng lên 2 dòng, con trỏ thụt lề hợp lý.
- [ ] **TC-02.4 (Dán văn bản nhiều dòng):** Copy một đoạn văn bản 3 dòng và Paste vào terminal. Ô nhập nhận toàn bộ nội dung mà không tự động ấn Enter thực thi vội vã.

### Nhóm 3: Lịch Sử Lệnh (History & Search)
- [ ] **TC-03.1 (Lịch sử Up/Down):** Chạy 2 lệnh `/help` và `/status`. Ấn mũi tên `↑`, thấy hiện lại `/status`, ấn `↑` tiếp thấy `/help`. Ấn `↓` quay lại dòng mới.
- [ ] **TC-03.2 (Tìm kiếm Ctrl+R):** Ấn `Ctrl+R`, giao diện chuyển sang chế độ tìm kiếm lịch sử: `(bsearch):`. Gõ `doc`, thấy hiển thị lệnh `/doctor`. Ấn `Enter` để chọn.
- [ ] **TC-03.3 (Lưu bền lịch sử):** Thoát Ldns, khởi động lại `ldns`. Ấn `↑`, xác nhận các lệnh đã gõ từ phiên trước vẫn được nhớ.

### Nhóm 4: Slash Menu & Autocomplete
- [ ] **TC-04.1 (Mở Slash Menu):** Gõ phím `/` vào ô nhập rỗng. Một danh sách menu popup xuất hiện ngay phía trên ô nhập với đầy đủ mô tả lệnh.
- [ ] **TC-04.2 (Lọc lệnh):** Gõ tiếp `re`, menu lập tức lọc chỉ còn `/resolve`. Ấn `Tab` hoặc `Enter`, ô nhập tự động điền đầy đủ `/resolve `.
- [ ] **TC-04.3 (Đóng Slash Menu):** Gõ `/`, menu mở ra. Ấn `Esc`, menu đóng lại, ký tự `/` vẫn còn trong ô nhập. Ấn `Esc` lần nữa, ô nhập được xóa sạch.
- [ ] **TC-04.4 (Gợi ý tham số ngữ cảnh):** Gõ `/rm `, ấn `Tab`. Hệ thống gợi ý danh sách domain hiện tại để chọn mà không cần gõ tay từ đầu.

### Nhóm 5: Spinner, Tác Vụ Dài & Ngắt Phím Esc
- [ ] **TC-05.1 (Hiển thị Spinner khi chờ):** Thực hiện một tác vụ mô phỏng chờ > 300ms. Ký tự `✻` xoay mượt mà, kèm dòng thời gian `(1s · esc để ngắt)`.
- [ ] **TC-05.2 (Ngắt bằng Esc):** Đang trong lúc spinner chạy, ấn phím `Esc`. Tác vụ lập tức dừng lại, in dòng thông báo màu vàng/đỏ: `⏺ Tác vụ đã bị huỷ bởi người dùng`.

### Nhóm 6: Hộp Thoại Xác Nhận (Dialog)
- [ ] **TC-06.1 (Hiển thị lựa chọn đánh số):** Kích hoạt thao tác nhạy cảm (như `/system setup`). Hộp thoại mở ra với các lựa chọn: `1. Có`, `2. Có, đừng hỏi lại`, `3. Không`.
- [ ] **TC-06.2 (Điều hướng phím số):** Nhấn trực tiếp phím `3`. Hộp thoại đóng lại và huỷ thao tác ngay lập tức mà không cần di chuyển mũi tên.
- [ ] **TC-06.3 (Điều hướng mũi tên):** Sử dụng `↑`/`↓` để di chuyển con trỏ `❯` đến `1. Có`, sau đó ấn `Enter` để xác nhận.

### Nhóm 7: Thanh Trạng Thái & Thay Đổi Kích Thước (Resize)
- [ ] **TC-07.1 (Đổi kích thước terminal):** Kéo thả thu nhỏ hoặc phóng to cửa sổ terminal. Vùng động (Input Box + Status Line) tự động căn chỉnh lại kích thước mà không để lại các vệt ký tự rác (ghost artifacts) trên màn hình.
- [ ] **TC-07.2 (Co hẹp chiều ngang):** Thu nhỏ chiều ngang cửa sổ xuống dưới 60 cột. Text trong thanh trạng thái tự động rút gọn thông minh mà không bị tràn vỡ sang dòng mới.

### Nhóm 8: Sự Kiện Thời Gian Thực (Live Events)
- [ ] **TC-08.1 (Chèn sự kiện khi đang gõ dở):** Đang gõ dở câu lệnh `/add test-`, nhận một sự kiện peer kết nối. Sự kiện được in đẩy lên phía trên scrollback, nội dung `/add test-` và vị trí con trỏ trong ô nhập vẫn giữ nguyên vẹn, không bị gián đoạn.

### Nhóm 9: Thẩm Mỹ Bảng Màu, Tiếng Việt & Fallback
- [ ] **TC-09.1 (Hiển thị tiếng Việt UTF-8):** Thử nhập domain hoặc ghi chú bằng tiếng Việt có dấu (`ứng-dụng.lan`). Con trỏ hiển thị đúng vị trí ký tự, không bị lệch hoặc mất dấu.
- [ ] **TC-09.2 (Chế độ NO_COLOR):** Chạy `export NO_COLOR=1` (hoặc `$env:NO_COLOR=1` trên PowerShell) rồi mở `ldns`. Giao diện hiển thị rõ ràng, không có bất kỳ ký tự escape mã màu lạ mắt nào.
- [ ] **TC-09.3 (Chế độ ASCII Fallback):** Thử nghiệm trên terminal cũ không có font Unicode. Các ký tự `✻`, `⏺`, `⎿`, `╭─╮` tự động chuyển đổi sang `*`, `o`, `|_`, `+-+` tương ứng, thông tin hiển thị ngay ngắn.

### Nhóm 10: Trực Quan Hóa Lệnh Chuyên Biệt
- [ ] **TC-10.1 (Kiểm tra /status):** Gõ `/status`. Kết quả định dạng khối gọn gàng, có màu phân biệt giữa các interface và danh sách peer.
- [ ] **TC-10.2 (Kiểm tra /resolve --trace):** Gõ `/resolve app.lan --trace`. Thấy sơ đồ cây trực quan hiển thị rõ bước 1 (Local), bước 2 (Peer), bước 3 (Upstream) kèm thời gian nano/mili-giây.
- [ ] **TC-10.3 (Kiểm tra /doctor):** Gõ `/doctor`. Danh sách kiểm tra có biểu tượng `✔`, `⚠`, `✖` rõ ràng, các lỗi đều đi kèm hướng dẫn lệnh khắc phục cụ thể.
- [ ] **TC-10.4 (Kiểm tra /logs --follow):** Gõ `/logs --follow`. Xem bảng log trực tiếp với các cột căn lề thẳng hàng, ấn `Esc` để dừng lại an toàn.

### Nhóm 11: Thoát Ứng Dụng An Toàn
- [ ] **TC-11.1 (Ctrl+C lần 1):** Ô nhập rỗng, ấn `Ctrl+C`. Giao diện hiện dòng nhắc: `Nhấn Ctrl+C lần nữa để thoát`.
- [ ] **TC-11.2 (Ctrl+C lần 2):** Ấn `Ctrl+C` lần 2 ngay sau đó. Ứng dụng thoát êm ái, con trỏ terminal (terminal cursor) được khôi phục trạng thái bình thường.
- [ ] **TC-11.3 (Ctrl+D):** Ô nhập rỗng, ấn `Ctrl+D`. Ứng dụng thoát ngay lập tức.
- [ ] **TC-11.4 (Lệnh /exit):** Gõ `/exit` và `Enter`. Ứng dụng dừng và thoát mã lỗi 0.
