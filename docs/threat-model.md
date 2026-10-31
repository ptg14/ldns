# Ldns — Mô hình đe dọa (Threat Model) v1.0

> Tài liệu phân tích an toàn, ranh giới tin cậy và các biện pháp giảm thiểu rủi ro cho Ldns v1.0.0.
> Tham chiếu: `docs/SPEC.md` §14, `docs/protocol.md` §5.

---

## 1. Giả định môi trường & Phạm vi (Scope & Assumptions)

Ldns được thiết kế cho mạng nội bộ (LAN): gia đình, văn phòng nhỏ (SOHO), nhóm kỹ sư phát triển phần mềm, lớp học hoặc phòng lab.

### 1.1 Ranh giới tin cậy (Trust Boundaries)
1. **Ranh giới Host (Nội bộ máy):**
   - Người dùng vận hành daemon vs các tài khoản không đặc quyền khác trên cùng hệ thống.
   - Giao tiếp IPC (Unix Domain Socket / Windows Named Pipe) giữa CLI/TUI và tiến trình Daemon.
2. **Ranh giới LAN (Mạng cục bộ):**
   - Thiết bị thuộc nhóm làm việc hợp pháp (có hoặc không có khóa PSK).
   - Thiết bị lạ trong cùng broadcast/multicast domain (ví dụ: máy khách trong mạng Wi-Fi chung).
3. **Ranh giới WAN (Internet & Upstream DNS):**
   - Các máy chủ DNS công cộng (1.1.1.1, 8.8.8.8, …).
   - Lưu lượng truy vấn đi ra ngoài mạng LAN.

---

## 2. Tài sản cần bảo vệ (Protected Assets)

1. **Tính toàn vẹn của bản ghi DNS nội bộ:** Đảm bảo `app.test` trỏ đúng máy của chủ sở hữu, không bị giả mạo chuyển hướng sang máy độc hại.
2. **Tính sẵn sàng của dịch vụ phân giải:** Tránh bị treo do quá tải gói tin (DoS), cạn kiệt bộ nhớ hoặc vòng lặp truy vấn.
3. **Bí mật khóa mạng (PSK):** Khóa dẫn xuất và khóa phiên không bị lộ ra ngoài mạng hoặc trong tệp nhật ký (logs).
4. **Cấu hình DNS của hệ điều hành:** Tránh làm mất kết nối Internet của máy chủ khi daemon tắt đột ngột.
5. **Quyền điều khiển tiến trình daemon (IPC):** Chỉ người dùng sở hữu hoặc quản trị viên mới được thao tác thêm/xóa domain hoặc đổi cấu hình mạng.

---

## 3. Phân tích mối đe dọa & Biện pháp giảm thiểu

### 3.1 Tấn công khuếch đại DNS (DNS Amplification / Reflection)
- **Kịch bản:** Kẻ tấn công trên Internet giả mạo IP nguồn của nạn nhân và gửi gói DNS `ANY` hoặc payload lớn tới cổng UDP 53 của Ldns để khuếch đại lưu lượng.
- **Biện pháp trong Ldns:**
  - **Mặc định Private ACL:** Ldns chỉ chấp nhận truy vấn từ các dải địa chỉ riêng RFC 1918 (`10.0.0.0/8`, `172.16.0.0/12`, `192.168.0.0/16`), Link-local RFC 3927 (`169.254.0.0/16`) và Loopback (`127.0.0.0/8`, `::1`). Các IP công cộng bên ngoài gửi tới đều bị từ chối phục vụ ngay lập tức.
  - **Rate Limiting:** Bộ giới hạn tốc độ (mặc định 200 qps/source IP) tự động ngắt và bỏ qua lưu lượng vượt ngưỡng.
  - **Không đệ quy mở (Closed Recursion):** Ldns không làm Open Resolver cho Internet.

---

### 3.2 Giả mạo Node & Nghe lén trên LAN (Eavesdropping & Spoofing)
- **Kịch bản:** Một máy lạ trong quán café phát hiện beacon Ldns, kết nối vào cổng TCP 5391 và gửi bản ghi giả mạo để chuyển hướng domain của người khác.
- **Biện pháp trong Ldns (Pre-Shared Key - PSK):**
  - **HMAC Beacon Validation:** Khi bật PSK, beacon UDP bắt buộc phải có trường `mac` được tính bằng `HMAC-SHA256(K_beacon, payload)`. Mọi beacon không có khóa hợp lệ bị bỏ qua im lặng ngay tại tầng socket; không tiến hành quay số (dial).
  - **Handshake xác thực 4 bước:** Phiên TCP xác thực chéo qua thông điệp `AUTH` với MAC trên toàn bộ bản ghi bắt tay (`Transcript Hash`).
  - **Mã hóa AEAD toàn diện:** Sau bắt tay, 100% dữ liệu trao đổi (UPDATE, SYNC, RESOLVE, PING) được mã hóa bằng thuật toán `XChaCha20-Poly1305-IETF` của `libsodium`.
  - **Chống Replay:** Nonce 24-byte tăng theo bộ đếm 64-bit nội bộ độc lập ở mỗi chiều. Gói tin bị phát lại hoặc đảo thứ tự sẽ khiến AEAD giải mã thất bại và ngắt phiên ngay lập tức.

---

### 3.3 Tấn công số lượng lớn (Sybil Attack) & Giới hạn của Cơ chế Bỏ phiếu
- **Kịch bản:** Kẻ xấu bật 20 máy ảo trong cùng mạng để bỏ phiếu chiếm quyền domain `api.local`.
- **Ranh giới thiết kế:**
  - **Bỏ phiếu KHÔNG PHẢI là cơ chế Byzantine Fault Tolerance (BFT):** Cơ chế xếp hạng và bỏ phiếu đa số trong Ldns v1.0 nhằm giải quyết tính nhất quán giữa các node thiện chí khi có chia mạng hoặc sai lệch tạm thời.
  - **Quy tắc Local Owner (D11):** Bản ghi do chính máy người dùng tạo (`self` hoặc domain do máy mình sở hữu) **luôn luôn chiến thắng tuyệt đối** trên máy đó, bất kể số lượng peer bên ngoài nói gì. Máy của bạn không bao giờ bị peer khác chiếm mất domain do chính bạn tạo.
  - **Khuyến nghị an toàn:** Trong môi trường mạng mở hoặc bán tin cậy, người dùng **bắt buộc phải bật PSK** (`ldns network key set`). Ldns chủ động cảnh báo `Warning: Network is running in UNENCRYPTED mode` trong lệnh `/status` khi chưa có khóa.

---

### 3.4 Cạn kiệt tài nguyên & Từ chối dịch vụ (DoS / Resource Exhaustion)
- **Kịch bản:** Node độc hại liên tục bơm hàng triệu bản ghi `UPDATE` hoặc gửi gói tin khổng lồ làm tràn RAM của daemon.
- **Biện pháp trong Ldns:**
  - **Giới hạn kích thước khung hình (Frame Cap):** Khung TCP tối đa 4 MiB. Khung vượt quá lập tức bị ngắt kết nối.
  - **Giới hạn tốc độ cập nhật:** Tối đa 200 thao tác UPDATE/giây cho mỗi peer.
  - **Giới hạn khối đồng bộ:** Tối đa 1.000 thao tác trên mỗi mẩu snapshot.
  - **Giới hạn số lượng peer:** Tối đa 64 peer đồng thời.
  - **Bảo vệ SQLite:** Cơ sở dữ liệu chạy chế độ WAL với PRAGMA an toàn, transaction độc lập và giới hạn kích thước cache.

---

### 3.5 Chiếm dụng tên miền công cộng (Public TLD Hijacking)
- **Kịch bản:** Một node tạo bản ghi `google.com` hoặc `github.com` trỏ về IP độc hại nhằm lừa các máy khác trong LAN.
- **Biện pháp trong Ldns:**
  - **Danh sách TLD công cộng tích hợp:** Ldns mang sẵn danh sách kiểm tra các TLD công cộng phổ biến (`.com`, `.net`, `.org`, `.io`, `.dev`, `.edu`, …).
  - **Xác nhận cưỡng bức (Explicit Confirmation):** Khi người dùng gõ lệnh tạo domain có TLD công cộng, CLI yêu cầu gõ cờ `--force` hoặc xác nhận tương tác.
  - **Độ ưu tiên Upstream:** Ldns ghi rõ trong nhật ký truy vấn nguồn gốc phân giải để người dùng dễ dàng kiểm toán bằng `ldns resolve --trace <domain>`.

---

### 3.6 An toàn giao tiếp tiến trình cục bộ (IPC Security)
- **Kịch bản:** Người dùng không đặc quyền trên cùng máy tính gửi lệnh JSON-RPC vào socket để tắt daemon hoặc xóa dữ liệu.
- **Biện pháp trong Ldns:**
  - **Linux / POSIX:** Unix Domain Socket được đặt tại thư mục bảo mật với quyền truy cập `0600` (chỉ user sở hữu tiến trình daemon hoặc `root` mới có quyền đọc/ghi).
  - **Windows:** Named Pipe được gán Security Descriptor (ACL) chặt chẽ, chỉ cho phép `Administrators` và `CURRENT_USER` kết nối.

---

### 3.7 Bảo vệ dữ liệu nhạy cảm & Không lộ khóa trong Log
- **Biện pháp trong Ldns:**
  - Giá trị khóa Pre-Shared Key (PSK) tuyệt đối **không bao giờ được in ra stdout hoặc spdlog** (chỉ hiển thị độ dài hoặc trạng thái `active`).
  - Tệp khóa PSK được tạo tự động với quyền `0600` trên hệ thống Linux/macOS.
  - Nhật ký truy vấn DNS được lưu trong tệp SQLite cục bộ với kích thước giới hạn, có thể tắt hoàn toàn qua cấu hình nếu yêu cầu chính sách bảo mật riêng tư cao.
