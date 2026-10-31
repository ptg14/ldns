# Ldns — Peer-to-Peer Local DNS for LAN

[![CI](https://github.com/ptg14/ldns/actions/workflows/ci.yml/badge.svg)](https://github.com/ptg14/ldns/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)](https://en.wikipedia.org/wiki/C%2B%2B20)
[![Version](https://img.shields.io/badge/release-v1.0.0-green.svg)](https://github.com/ldns/ldns/releases)

Ldns là hệ thống phân giải tên miền nội bộ mạng LAN ngang hàng (peer-to-peer), hoạt động không cần máy chủ trung tâm (serverless), viết bằng C++20 cho **Windows** và **Linux**.

---

## 3 Trụ Cột Cốt Lõi (Core Pillars)

1. **R1 — Phân giải 3 bước thông minh:**
   - **Bước 1 (Cục bộ):** Bản ghi do chính máy sở hữu (trả lời tức thì trong `< 1 ms`).
   - **Bước 2 (Peer LAN):** Tự động truy vấn mạng peer với cơ chế xếp hạng độ trễ, gửi truy vấn đón đầu (hedging) và bỏ phiếu đa số giải quyết xung đột xác định (deterministic tie-breaking).
   - **Bước 3 (Upstream):** Chuyển tiếp ra Internet (Cloudflare, Google hoặc DNS hệ thống) mượt mà cho các tên miền ngoài mạng.
2. **R2 — Tự phát hiện & Đồng bộ thời gian thực (Zero-Config):**
   - Các node tự tìm thấy nhau qua UDP Multicast (`239.76.68.83:5390`) và Broadcast trong `<= 5 s`.
   - Thay đổi tên miền (thêm, sửa, xóa) được lan truyền và phản ánh trên toàn mạng trong `< 500 ms`.
3. **R3 — Không giới hạn tên miền:**
   - Tạo không giới hạn domain trỏ về chính IP máy mình (`self`), IP tĩnh hoặc CNAME. Hỗ trợ đầy đủ Wildcard (`*.dev.local`).

---

## Bắt đầu nhanh trong 5 phút (Quickstart)

### 1. Cài đặt & Chạy Daemon

**Trên Linux (Ubuntu / Debian):**
```bash
# Tải và cài đặt gói .deb từ trang GitHub Releases
sudo dpkg -i ldns_1.0.0_amd64.deb

# Khởi chạy dịch vụ nền
sudo systemctl enable --now ldns
```

**Trên Windows:**
```powershell
# Tải và giải nén ldns-1.0.0-windows-x64.zip
# Mở PowerShell với quyền Administrator và cài đặt Service:
ldns service install
ldns service start
```

*Hoặc chạy trực tiếp trên cửa sổ dòng lệnh (Foreground mode):*
```bash
ldns daemon
```

---

### 2. Thêm domain nội bộ

Trên **Máy A** (ví dụ IP LAN `192.168.1.10`):
```bash
# Tạo domain trỏ về chính máy mình
ldns add api.local

# Tạo domain trỏ về IP cụ thể
ldns add db.local --ip 192.168.1.50
```

---

### 3. Phân giải thử từ Máy B

Trên **Máy B** trong cùng mạng LAN:
```bash
# Phân giải bằng công cụ tích hợp
ldns resolve api.local --trace

# Hoặc dùng lệnh dig / nslookup chuẩn của hệ điều hành
dig @127.0.0.1 api.local
nslookup api.local 127.0.0.1
```
Kết quả trả về chính xác IP của Máy A trong nháy mắt!

---

### 4. Bật bảo mật & Mã hóa mạng (Tùy chọn nhưng Khuyến nghị)

Để ngăn thiết bị lạ trong mạng Wi-Fi nghe lén hoặc giả mạo bản ghi:

```bash
# Trên một máy: sinh khóa mạng
ldns network key generate cluster.key

# Đặt khóa cho cụm
ldns network key set cluster.key

# Sao chép cluster.key sang các máy đồng nghiệp và chạy:
ldns network key set cluster.key
```
Toàn bộ lưu lượng beacon và đồng bộ TCP giờ đây được bảo vệ bằng **HMAC-SHA256** và mã hóa **XChaCha20-Poly1305** (`libsodium`).

---

## Bảng tra cứu lệnh CLI (Cheat Sheet)

| Mục | Lệnh | Mô tả |
|---|---|---|
| **Domain** | `ldns add <domain> [--ip <addr>] [--ttl <sec>]` | Thêm domain trỏ về máy mình hoặc IP chỉ định |
| | `ldns del <domain>` | Xóa domain khỏi mạng |
| | `ldns list [--json]` | Liệt kê các domain do máy này sở hữu |
| | `ldns export <file>` | Xuất danh sách domain ra định dạng TOML hoặc CSV |
| | `ldns import <file>` | Nhập hàng loạt domain từ file sao lưu |
| **Phân giải** | `ldns resolve <domain> [--trace]` | Truy vấn domain và theo dõi chi tiết đường đi L/P/U |
| **Peer** | `ldns peers [--json]` | Xem danh sách các peer đang kết nối và độ trễ RTT |
| | `ldns peers add <ip:port>` | Thêm peer thủ công khi mạng chặn multicast |
| **Xung đột** | `ldns conflicts` | Kiểm tra các domain đang bị trùng tên giữa các máy |
| **Bảo mật** | `ldns network key generate [path]` | Tạo khóa Pre-Shared Key (PSK) 256-bit an toàn |
| | `ldns network key set <path>` | Kích hoạt khóa mạng cho node |
| | `ldns network key clear` | Xóa khóa mạng (quay về chế độ không mã hóa) |
| | `ldns network key status` | Kiểm tra trạng thái bảo mật hiện tại |
| **Chẩn đoán** | `ldns doctor` | Chẩn đoán toàn diện: cổng 53, tường lửa, multicast, upstream |
| **Tích hợp HĐH** | `ldns system setup` | Tự động cấu hình DNS hệ điều hành trỏ về Ldns |
| | `ldns system restore` | Trả lại cấu hình DNS hệ điều hành về nguyên bản |
| **Dịch vụ** | `ldns service install` | Đăng ký Ldns thành systemd service (Linux) hoặc SCM service (Windows) |
| | `ldns service status` | Xem trạng thái hoạt động của daemon nền |
| | `ldns logs [--follow]` | Xem nhật ký truy vấn và sự kiện thời gian thực |

---

## Kiến trúc hệ thống (Architecture)

```
                    ┌───────────────────────────┐
                    │     DNS Client (Port 53)  │
                    └─────────────┬─────────────┘
                                  │
                                  ▼
                    ┌───────────────────────────┐
                    │   Resolution Pipeline     │
                    └─────────────┬─────────────┘
                                  │
         ┌────────────────────────┼────────────────────────┐
         │ (1) Local Zone         │ (2) Peer Resolver      │ (3) Upstream
         ▼                        ▼                        ▼
┌──────────────────┐    ┌──────────────────┐     ┌──────────────────┐
│  SQLite Store    │    │  Voting & Rank   │     │ Public DNS       │
│  (Single-Writer) │    │  Hedge (250 ms)  │     │ 1.1.1.1, 8.8.8.8 │
└──────────────────┘    └─────────┬────────┘     └──────────────────┘
                                  │
                        ┌─────────┴────────┐
                        │ Peer Connections │
                        │  (TCP / 5391)    │
                        │ AEAD Encrypted   │
                        └──────────────────┘
```

---

## Cấu hình (`config.toml`)

Vị trí tệp cấu hình mặc định:
- **Linux (Service):** `/etc/ldns/config.toml`
- **Linux (User):** `~/.config/ldns/config.toml`
- **Windows (Service):** `C:\ProgramData\ldns\config.toml`
- **Windows (User):** `%APPDATA%\ldns\config.toml`

Xem tệp mẫu có chú thích đầy đủ tại [`config/config.example.toml`](config/config.example.toml). Mọi giá trị cấu hình đều có thể được ghi đè bằng biến môi trường dạng `LDNS_<SECTION>_<KEY>` (ví dụ: `LDNS_NETWORK_NAME=devgroup`).

---

## Biên dịch từ mã nguồn (Build from Source)

Yêu cầu:
- Trình biên dịch C++20: MSVC 2022 (v143), GCC 12+, hoặc Clang 15+.
- CMake ≥ 3.25 và công cụ quản lý gói [vcpkg](https://github.com/microsoft/vcpkg).

```bash
# 1. Tải repository
git clone https://github.com/ldns/ldns.git
cd ldns

# 2. Biên dịch trên Linux
export VCPKG_ROOT=/path/to/vcpkg
cmake --preset linux-release
cmake --build --preset linux-release
ctest --preset linux-release

# 3. Biên dịch trên Windows (Visual Studio Developer Prompt)
cmake --preset windows-ci
cmake --build --preset windows-ci
ctest --preset windows-ci
```

---

## Tài liệu tham khảo (Documentation)

- [Tài liệu Đặc tả Kỹ thuật (SPEC.md)](docs/SPEC.md) — Chi tiết thiết kế toàn diện và các quyết định D1–D14.
- [Giao thức Ngang hàng (protocol.md)](docs/protocol.md) — Định dạng wire frame, MessagePack schema, và handshake.
- [Kế hoạch Phát triển (PLAN.md)](docs/PLAN.md) — Lộ trình và bảng theo dõi các milestone.
- [Câu hỏi Thường Gặp (FAQ.md)](docs/FAQ.md) — Hướng dẫn xử lý xung đột cổng 53, tường lửa, và phục hồi hệ thống.
- [Mô hình Đe dọa (threat-model.md)](docs/threat-model.md) — Phân tích bảo mật, ranh giới tin cậy, và chống tấn công mạng LAN.

---

## Giấy phép (License)

Dự án phát hành theo giấy phép [MIT License](LICENSE).
