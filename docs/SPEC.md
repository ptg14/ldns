# Ldns — Đặc tả (SPEC) v0.1

> Local DNS ngang hàng (peer-to-peer) cho mạng LAN. Ứng dụng CLI/TUI, C++20, chạy trên Windows và Linux.

---

## 0. Mục đích (đọc lại mỗi khi bắt đầu một milestone)

Ldns tồn tại để **bất kỳ ai trong mạng LAN cũng có thể tự đăng ký và dùng domain riêng của mình, mà mọi người khác trong LAN truy cập được ngay, không cần server trung tâm, không cần cấu hình tay từng máy.**

Ba yêu cầu cốt lõi (nguồn gốc của mọi quyết định thiết kế bên dưới):

| # | Yêu cầu cốt lõi | Mục SPEC chính |
|---|---|---|
| R1 | **Luồng phân giải khi thất bại, theo thứ tự:** (1) DNS của chính ứng dụng người dùng → (2) DNS của ứng dụng người dùng khác trong mạng (nhiều bản khác nhau thì chọn bản có nhiều người đưa ra nhất; thất bại thì lần lượt theo xếp hạng đến khi hết) → (3) DNS server mặc định hoặc được cấu hình | §6 |
| R2 | **Các ứng dụng trong mạng tự động kết nối**, cho nhau biết domain mình đang nắm giữ, **cập nhật theo thời gian thực** | §7 |
| R3 | **Người dùng tạo vô hạn domain**; mỗi domain trỏ về địa chỉ của chính họ hoặc về địa chỉ họ cấu hình | §9 |

Ràng buộc nền tảng: CLI cho **Windows + Linux**, ngôn ngữ **C++**, tên **Ldns**, UX/UI **giống Claude Code** (§10).

---

## 1. Phạm vi

### 1.1 Trong phạm vi (v1.0)
- DNS server UDP+TCP (RFC 1035, EDNS0 cơ bản) phục vụ chính máy chạy Ldns và các thiết bị khác trong LAN.
- Domain tự đăng ký không giới hạn số lượng: record A, AAAA, CNAME, TXT, wildcard `*.x`.
- Tự phát hiện peer, đồng bộ registry thời gian thực, bỏ phiếu chọn bản trả lời.
- Forward lên upstream DNS.
- TUI tương tác kiểu Claude Code + CLI không tương tác (`ldns add ...`) + chế độ daemon/service.
- Tích hợp hệ điều hành: chiếm port 53, đặt DNS hệ thống, cài service, mở firewall.
- Bảo mật cơ bản: khóa mạng tùy chọn (PSK), ACL theo subnet, rate limit.

### 1.2 Ngoài phạm vi (v1.0)
- DNSSEC, DoH/DoT, DHCP server, GUI đồ họa, macOS (code vẫn viết portable, chưa hứa hỗ trợ).
- Đồng bộ qua Internet/WAN, NAT traversal.
- Xác thực danh tính từng người dùng (v1 chỉ có tin cậy mức mạng + PSK tùy chọn).

---

## 2. Thuật ngữ

| Thuật ngữ | Nghĩa |
|---|---|
| **Node** | Một tiến trình Ldns đang chạy trên một máy. Có `node_id` ngẫu nhiên 128-bit, lưu bền. |
| **Owner** | Node đã đăng ký một domain. Owner là nguồn sự thật duy nhất (single writer) cho record của mình. |
| **Zone cục bộ (LocalZone)** | Tập record mà node này sở hữu. |
| **Registry** | Bản sao nhân bản (replicated) của zone của **mọi** owner mà node này biết. |
| **Claim** | Một owner khẳng định "domain X có RRset là Y". |
| **Nhóm bản (answer group)** | Các claim cho cùng domain/type có RRset giống hệt nhau. |
| **Phiếu (votes)** | Số owner (đang online) trong một nhóm bản. |
| **Upstream** | DNS server bên ngoài (mặc định hoặc do người dùng cấu hình). |
| **Mạng Ldns (network)** | Tập node có cùng `network_name` (mặc định `default`) và cùng PSK (nếu có). |

---

## 3. Quyết định đã chốt (do người dùng không trả lời thêm câu hỏi)

| # | Chủ đề | Quyết định | Lý do |
|---|---|---|---|
| D1 | Ngôn ngữ | C++20, CMake, vcpkg manifest | Yêu cầu + cross-platform |
| D2 | Mạng | Standalone **Asio** (async, cross-platform) | Chín muồi trên Win/Linux |
| D3 | DNS codec | **Tự viết** (parser/encoder tối giản, fuzz-test) | Thư viện DNS C++ có sẵn nặng/không hợp |
| D4 | Lưu trữ | **SQLite** (WAL) + file TOML cho cấu hình | Giao dịch nguyên tử, seq an toàn khi crash |
| D5 | Serialization giữa peer | MessagePack (qua nlohmann::json), frame có tiền tố độ dài | Gọn, dễ debug, ít dependency |
| D6 | Discovery | UDP multicast `239.76.68.83:5390` + broadcast dự phòng + peer thêm tay | Zero-config, vẫn chạy khi multicast bị chặn |
| D7 | Đồng bộ | TCP `5391`, lưới đầy đủ (full mesh) + gossip relay | LAN nhỏ-vừa; relay chịu được mạng chia đoạn |
| D8 | Mô hình dữ liệu | **Mỗi owner là single-writer, có `seq` tăng dần** | Không cần CRDT, không xung đột trong một owner |
| D9 | Xung đột giữa owner | Không cấm. Giải quyết **lúc phân giải** bằng bỏ phiếu (R1) | Đúng mô tả của người dùng |
| D10 | Khi hòa phiếu | Nhóm có claim **cũ nhất** thắng; còn hòa thì `node_id` nhỏ nhất | Xác định, giống nhau trên mọi node |
| D11 | Ưu tiên bước 1 | Domain **do mình sở hữu luôn thắng**, kể cả khi người khác bỏ phiếu khác | Đúng thứ tự R1 |
| D12 | TUI | Renderer **inline tự viết** (không dùng alt-screen), giống Claude Code | Giữ scrollback như Claude Code |
| D13 | Process model | Một binary `ldns`: TUI nhúng core, hoặc gắn vào daemon nếu đang chạy | Dùng thử đơn giản; production dùng service |
| D14 | Bind DNS | Bind từng địa chỉ cụ thể (127.0.0.1 + IP LAN), **không** bind 0.0.0.0 | Tránh đụng `systemd-resolved` (127.0.0.53) và tránh lộ ra mạng công cộng |
| D15 | TLD | Cho phép mọi tên; cảnh báo + xác nhận nếu trùng TLD công cộng phổ biến; khuyên dùng `.lan`, `.home.arpa`, `.test` | Linh hoạt nhưng tránh chiếm nhầm `google.com` |
| D16 | Bảo mật mặc định | Mở (zero-config) nhưng `/status` luôn cảnh báo "chưa đặt khóa mạng" | Đúng mục tiêu "tự động kết nối" |

---

## 4. Kiến trúc

```
┌──────────────────────────── ldns (binary duy nhất) ───────────────────────────┐
│ Frontend:   TUI (mặc định) │ CLI một lần (ldns add ...) │ daemon (headless)   │
│                      │  IPC: JSON-RPC 2.0 (Unix socket / Named pipe)           │
│ ┌────────────────────▼──────────────────────────────────────────────────────┐ │
│ │ CORE (thư viện ldns_core)                                                 │ │
│ │                                                                           │ │
│ │  DnsServer (UDP+TCP) ──► Resolver ─┬─ 1. LocalZone   (record của mình)    │ │
│ │                                    ├─ 2. PeerResolver (bỏ phiếu+xếp hạng) │ │
│ │                                    └─ 3. Upstream     (forward)           │ │
│ │                                                                           │ │
│ │  Registry ◄── SyncEngine ◄── PeerManager ◄── Discovery (multicast/bcast)  │ │
│ │     │                             │                                        │ │
│ │  Store(SQLite) · Config(TOML) · EventBus · QueryLog · Metrics             │ │
│ └───────────────────────────────────────────────────────────────────────────┘ │
└────────────────────────────────────────────────────────────────────────────────┘
```

Nguyên tắc:
- **Core không biết gì về UI.** Mọi thao tác UI đi qua `CoreApi` (gọi trực tiếp khi nhúng, hoặc qua IPC khi gắn daemon). Nhờ vậy CLI một lần, TUI và daemon dùng chung một đường.
- **Một io_context cho mạng**, thread pool nhỏ cho SQLite; không khóa chéo giữa DNS hot-path và đồng bộ (Registry đọc qua snapshot bất biến / RCU).
- Tất cả thay đổi trạng thái phát ra `Event` qua EventBus (TUI đăng ký để hiển thị thời gian thực).

---

## 5. Mô hình dữ liệu

### 5.1 Tên miền
- Chuẩn hóa: chữ thường, bỏ dấu chấm cuối, IDN → punycode (`xn--`).
- Nhãn 1–63 byte, tổng ≤ 253. Ký tự `a-z 0-9 - _`. Wildcard chỉ hợp lệ ở nhãn trái cùng (`*.app.lan`).
- Không giới hạn số domain/người dùng. Giới hạn mềm chỉ để bảo vệ: kích thước một frame ≤ 4 MiB, ≤ 1000 op/UPDATE (snapshot được chia khúc).

### 5.2 Record (RR)
| Trường | Mô tả |
|---|---|
| `name` | tên đã chuẩn hóa |
| `type` | `A`, `AAAA`, `CNAME`, `TXT` (v1); `MX`, `SRV`, `PTR` (v1.1) |
| `ttl` | giây, mặc định 60 |
| `data` | danh sách giá trị (RRset) |
| `target_kind` | `self` \| `static` (chỉ lưu cục bộ) |
| `created_at`, `updated_at` | HLC timestamp (Hybrid Logical Clock) |

`target_kind = self`: trỏ về **IP LAN hiện tại của máy này**, tự cập nhật khi IP đổi (xem §9.2). Owner luôn **công bố giá trị cụ thể** (đã giải `self` thành IP) cho peer, nên peer không cần biết `self`.

### 5.3 Trạng thái owner trong Registry
```
OwnerState {
  node_id, name, epoch,        // epoch: số ngẫu nhiên, đổi khi owner mất dữ liệu/reset
  seq,                         // tăng mỗi lần owner thay đổi zone
  addr[], sync_port, dns_port,
  status: online | offline,
  last_seen,
  records: map<(name,type) -> RR>
}
```
- Owner offline: giữ trong registry tới `peers.offline_retention` (mặc định 24h) hoặc đến khi `/peer forget`.

### 5.4 Lược đồ SQLite (rút gọn)
```
meta(key, value)                       -- node_id, epoch, schema_version, seq_self
my_records(name, type, ttl, target_kind, data_json, created_at, updated_at, PRIMARY KEY(name,type))
peers(node_id PK, name, epoch, seq, last_addr, last_seen)
peer_records(owner_id, name, type, ttl, data_json, created_at, PRIMARY KEY(owner_id,name,type))
upstreams(idx, address, port, proto)
query_log(ts, client, qname, qtype, step, rcode, latency_us)   -- vòng, giới hạn dòng
```

---

## 6. Luồng phân giải (R1) — phần cốt lõi

### 6.1 Định nghĩa "thất bại" của mỗi bước
| Bước | Thất bại khi |
|---|---|
| 1. Local | Tên **không tồn tại** trong zone mình sở hữu (kể cả wildcard, kể cả sau khi theo CNAME nội bộ) |
| 2. Peer | Không có ứng viên nào trong registry, **hoặc** mọi ứng viên đều thất bại (timeout, không kết nối được, peer báo không có/lỗi) |
| 3. Upstream | Mọi upstream đều timeout/SERVFAIL. NXDOMAIN thật từ upstream là **câu trả lời hợp lệ**, trả nguyên cho client |

### 6.2 Pipeline

```
Query (qname, qtype) từ client
  │
  ├─0. Chuẩn hóa tên → tra cache trong bộ nhớ (hit → trả luôn)
  │
  ├─1. LocalZone ───────── có (qname) ? ──► trả lời, AA=1  (dừng)
  │       tên có nhưng khác type → NOERROR/NODATA, AA=1 (dừng; KHÔNG đi tiếp)
  │       (không có) ▼
  │
  ├─2. PeerResolver
  │     a. Tra Registry (không dùng mạng): với mỗi owner, tính câu trả lời của owner cho (qname,qtype)
  │        (khớp chính xác trước, wildcard sau).
  │     b. Gom thành nhóm bản theo RRset giống hệt. Chỉ tính owner ONLINE.
  │     c. Xếp hạng nhóm: phiếu giảm dần → claim cũ nhất → node_id nhỏ nhất.
  │     d. Duyệt nhóm theo thứ tự xếp hạng; trong nhóm duyệt peer (RTT thấp trước):
  │          gửi RESOLVE tới peer qua kênh sync. Peer CHỈ trả lời từ LocalZone của nó (không forward).
  │          thành công → trả lời (AA=0), TTL = min(ttl, 30s), dừng.
  │          thất bại → peer kế tiếp trong nhóm → nhóm kế tiếp → hết.
  │     e. Hết ứng viên online: nếu `peers.serve_stale=true`, thử nhóm của owner OFFLINE
  │        dùng giá trị lưu trong registry (TTL=10s). Vẫn không có → thất bại bước 2.
  │       (không có ứng viên) ▼
  │
  └─3. Upstream: lần lượt từng server trong danh sách cấu hình (timeout 2s, thử lại 1 lần)
        trả lời đầu tiên hợp lệ (kể cả NXDOMAIN) → trả client.
        tất cả thất bại → SERVFAIL.
```

### 6.3 Quy tắc chi tiết
1. **Bước 2 không tốn mạng khi tên không có trong registry** → truy vấn Internet bình thường (google.com…) không bị chậm.
2. **Hedging:** các ứng viên vẫn được duyệt *theo thứ tự ưu tiên*, nhưng nếu ứng viên hiện tại chưa trả lời sau `hedge_ms` (mặc định 250 ms), khởi chạy ứng viên kế tiếp song song; câu trả lời hợp lệ **đến đầu tiên** thắng và hủy phần còn lại. Tổng ngân sách bước 2: `peers.budget_ms` (mặc định 1500 ms) để client không timeout trước khi sang bước 3.
3. **Chống vòng lặp:** `RESOLVE` giữa peer chỉ tra LocalZone. Upstream không được trùng địa chỉ lắng nghe của chính mình (kiểm tra khi nạp cấu hình).
4. **CNAME:** theo chuỗi tối đa 8 bước; mỗi mắt xích đi qua lại toàn bộ pipeline.
5. **Cache:** LRU trong bộ nhớ (mặc định 50 000 mục). Positive: `min(ttl, 30s)` cho dữ liệu từ peer; theo TTL upstream (tối đa 300s) cho dữ liệu upstream. Negative: 5s. **Cache peer bị xóa ngay khi Registry nhận UPDATE liên quan** → thay đổi real-time hiệu lực tức thì.
6. **Xung đột:** nếu cùng tên có ≥ 2 nhóm bản (hoặc khác với zone cục bộ), ghi sự kiện `Conflict`; hiển thị trong `/conflicts`. Không chặn việc phân giải.
7. **Cờ trả lời:** zone cục bộ AA=1; còn lại AA=0; luôn RA=1.
8. **Tin nhắn lớn:** UDP > 512B (hoặc kích thước EDNS0 của client) → đặt TC=1, client thử lại bằng TCP.

### 6.4 Ví dụ
- A đăng ký `app.lan → 192.168.1.10`. B, C, D (không sở hữu) cùng hỏi `app.lan`:
  bước 1 thất bại → bước 2 thấy 1 nhóm (1 phiếu, A) → hỏi A → trả `192.168.1.10`.
- A và E đều có `app.lan → 192.168.1.10`; F có `app.lan → 192.168.1.99`. Máy G hỏi:
  nhóm {A,E} = 2 phiếu > nhóm {F} = 1 phiếu → thử A (hoặc E); nếu cả A, E đều lỗi → thử F.
- Máy A hỏi `app.lan`: bước 1 thành công → trả luôn bản của mình (D11), dù E và F có ý kiến khác.

---

## 7. Tự động kết nối & đồng bộ thời gian thực (R2)

### 7.1 Cổng mặc định (đều cấu hình được)
| Mục đích | Giao thức | Cổng |
|---|---|---|
| DNS | UDP+TCP | 53 |
| Discovery | UDP multicast `239.76.68.83` (+ broadcast dự phòng) | 5390 |
| Sync/peer | TCP | 5391 |

### 7.2 Discovery
- Mỗi node gửi **beacon** trên mọi interface LAN hợp lệ (bỏ loopback/virtual theo `network.interfaces`): lúc khởi động ngay lập tức, sau đó mỗi 2s trong 10s đầu, rồi mỗi 10s ± 20% jitter. Gửi `bye` khi thoát êm.
- Beacon: `{v, node, name, net(hash 8B của network_name), sync_port, dns_port, ips[], epoch, seq, ts}` (có HMAC nếu bật PSK).
- Nhận beacon của node lạ cùng `net` → mở kết nối TCP.
  **Quy tắc tránh kết nối đôi:** node có `node_id` **nhỏ hơn** là bên chủ động dial; nếu vẫn lỡ có 2 kết nối, giữ kết nối do node nhỏ hơn dial, đóng cái kia.
- **Peer exchange:** sau handshake, node gửi danh sách địa chỉ peer nó biết → mạng nhiều subnet/multicast bị chặn vẫn nối được khi có ít nhất một node cầu nối.
- `/peer add <ip[:port]>` dành cho mạng chặn multicast (VLAN, AP isolation). Danh sách lưu bền và được thử lại với exponential backoff (1s → 60s).

### 7.3 Kết nối peer
**Handshake**
1. `HELLO {v, node, name, net, epoch, seq, ips, sync_port, dns_port, caps, nonce}`.
2. Nếu PSK: trao đổi nonce, kiểm tra HMAC-SHA256; sau đó mọi frame mã hóa AEAD (XChaCha20-Poly1305) với khóa phiên dẫn xuất từ PSK + hai nonce.
3. Hai bên so `net`, `v` (khác major → từ chối) rồi vào trạng thái `ESTABLISHED`.

**Thông điệp**
| Tin | Nội dung / ý nghĩa |
|---|---|
| `PING`/`PONG` | Heartbeat mỗi 5s; mất 3 lần (15s) → peer `offline`, đóng kết nối, thử nối lại |
| `SYNC_REQ {owners: {id: {epoch, since_seq}}}` | Xin delta |
| `SYNC_DATA {owner, epoch, from_seq, to_seq, ops[], snapshot?}` | Trả delta, hoặc **snapshot** (chia khúc) nếu epoch khác / không còn log |
| `UPDATE {owner, epoch, seq, ops[]}` | Đẩy thay đổi tức thì |
| `RESOLVE {id, name, type}` / `RESOLVE_R {id, rcode, answers[]}` | Hỏi thẳng LocalZone của peer (§6.2 d) |
| `PEERS {list}` | Peer exchange |
| `BYE` | Thoát êm |

**Op**: `{op: "put", name, type, ttl, data[], ts}` hoặc `{op: "del", name, type, ts}`.

### 7.4 Quy tắc đồng bộ
1. **Single-writer:** chỉ owner được phát hành op cho zone của mình. Node khác chỉ chấp nhận op của owner X nếu nó đến từ owner X **hoặc** từ node relay có `seq` mới hơn bản đã biết (đã xác thực bằng PSK/phiên).
2. **Thay đổi cục bộ** → ghi SQLite (transaction) → tăng `seq` → đẩy `UPDATE` tới mọi peer đang kết nối → **relay**: node nhận `UPDATE` mới (seq > đã biết) chuyển tiếp cho các peer khác (trừ nơi gửi). Trùng/cũ bị bỏ (dedupe theo `(owner,epoch,seq)`).
3. **Phát hiện lỗ hổng:** `UPDATE.seq != last_seq+1` → gửi `SYNC_REQ` thay vì áp dụng bừa.
4. **Epoch:** owner mất DB/đặt lại → epoch mới, seq về 0; peer bỏ dữ liệu cũ của owner đó và nhận snapshot.
5. **Mục tiêu độ trễ:** thay đổi lan tới mọi peer đang kết nối < **500 ms** trên LAN; node mới vào có đủ dữ liệu < **2 s** với ≤ 10 000 record.
6. **Đổi IP:** theo dõi sự kiện đổi địa chỉ (Linux netlink / Windows `NotifyIpInterfaceChange`, dự phòng thăm dò 5s) → tính lại record `self` → phát `UPDATE` + beacon mới.

### 7.5 Peer ngoại tuyến
Chuyển `offline` → xóa khỏi bỏ phiếu bước 2d (chỉ còn dùng ở bước 2e khi `serve_stale`). Khi quay lại cùng `epoch` → `SYNC_REQ` bằng delta; khác epoch → snapshot.

---

## 8. DNS server

- UDP + TCP; mỗi interface hợp lệ một socket; **SO_REUSEADDR/REUSEPORT không dùng để ép chiếm** (§13.1).
- Parser nghiêm ngặt: giới hạn độ sâu nén con trỏ, độ dài, số record; tuyệt đối không crash với gói hỏng (có fuzz test).
- Hỗ trợ: opcode QUERY; type A, AAAA, CNAME, TXT, NS/SOA (SOA tối thiểu cho NODATA/NXDOMAIN của zone cục bộ), ANY (trả tối thiểu), EDNS0 (UDP size, bỏ qua option lạ).
- **ACL mặc định:** chỉ phục vụ nguồn thuộc RFC1918, `fc00::/7`, link-local, loopback. Cấu hình `server.allow` để mở rộng. Không phục vụ đệ quy cho nguồn công cộng (chống open resolver/khuếch đại).
- **Rate limit:** mặc định 200 qps/nguồn (token bucket), vượt thì bỏ gói.
- Đồng thời: ≥ 10 000 qps cục bộ trên máy phổ thông (xem §15).

---

## 9. Quản lý domain (R3)

### 9.1 Tạo/sửa/xóa
- Không giới hạn số domain. Tên trùng tên mình đã có → báo "đã tồn tại, dùng `/set`".
- Mặc định đích `self`. Có thể đặt: IPv4/IPv6 tường minh, hoặc `CNAME` tới tên khác, hoặc `TXT`.
- Nhiều giá trị cho một tên (round-robin): `/add app.lan 10.0.0.5 10.0.0.6`.
- Wildcard: `/add *.app.lan` phủ mọi nhãn con chưa có bản ghi riêng.
- Chính sách TLD (D15): tên có đuôi trùng danh sách TLD công cộng nhúng sẵn (com, net, org, vn, io, dev…) → hộp thoại xác nhận (hoặc cờ `--force`). `.local` bị cảnh báo vì xung đột mDNS.
- Nhập/xuất hàng loạt: `/import file.toml|csv`, `/export`.

### 9.2 `self` chi tiết
- Tập IP của `self` = địa chỉ IPv4 unicast không phải loopback/link-local trên interface thuộc mạng Ldns (ưu tiên interface có route mặc định). Có thể ghim bằng `network.self_interface` hoặc `network.self_ip`.
- Khi nhiều IP → publish tất cả (RRset nhiều giá trị), trừ khi ghim.
- IP đổi → record tự cập nhật + đồng bộ (§7.4.6).

### 9.3 Tên mặc định của node
Không có record tự sinh trong v1 (v1.1: `<node-name>.ldns` tự trỏ về node).

---

## 10. CLI & TUI (UX giống Claude Code)

### 10.1 Chế độ chạy
```
ldns                         # mở TUI; gắn vào daemon nếu có, không thì nhúng core
ldns daemon [--foreground]   # chạy headless (dùng bởi service)
ldns <lệnh> [tham số]        # CLI một lần; in kết quả rồi thoát (hỗ trợ --json)
ldns --version | --help
```
Mã thoát: 0 thành công; 1 lỗi chung; 2 sai tham số; 3 không kết nối được daemon; 4 thiếu quyền; 5 xung đột/đã tồn tại.

### 10.2 Màn hình chính (inline, không alt-screen)

```
╭──────────────────────────────────────────────────────────╮
│ ✻ Chào mừng đến với Ldns!                                │
│                                                          │
│   /help để xem lệnh · /add app.lan để tạo domain đầu tiên │
│                                                          │
│   node:  thang-pc  (a3f9…)       mạng: default           │
│   ip:    192.168.1.10             dns:  :53  ● hoạt động │
╰──────────────────────────────────────────────────────────╯

> /add app.lan

⏺ Đã thêm app.lan → 192.168.1.10 (self)
  ⎿ TTL 60s · đã đồng bộ tới 4 peer trong 38 ms

⏺ Peer mới: minh-laptop (192.168.1.22) · 12 domain
⏺ minh-laptop vừa thêm db.lan → 192.168.1.22

╭──────────────────────────────────────────────────────────╮
│ > Thử "/add myapp.lan"                                   │
╰──────────────────────────────────────────────────────────╯
  ● 5 peer · 14 domain · 0 xung đột         ctrl+c×2 thoát
```

Quy ước hiển thị (tương ứng Claude Code):
- **Bản ghi bất biến** (lệnh đã chạy, kết quả, sự kiện) được in vào scrollback của terminal; **vùng động** phía dưới (spinner, ô nhập, dòng trạng thái) được vẽ lại tại chỗ.
- Tiền tố `>` cho lệnh người dùng, `⏺` cho kết quả/sự kiện, `⎿` cho dòng chi tiết thụt lề, `✻` cho spinner/tiêu đề. Màu nhấn cam đất (terracotta); lỗi đỏ, cảnh báo vàng, thành công xanh; chữ phụ mờ (dim).
- **Ô nhập** viền bo tròn, placeholder gợi ý, lịch sử ↑/↓ (lưu bền), nhập nhiều dòng bằng `\`+Enter hoặc Shift+Enter.
- **Menu slash:** gõ `/` hiện danh sách lệnh + mô tả, lọc theo ký tự gõ, ↑/↓ chọn, Tab/Enter hoàn thành.
- **Spinner** có động từ xoay vòng khi tác vụ chạy > 300 ms: `✻ Đang phân giải… (2s · esc để ngắt)`.
- **Hộp thoại xác nhận** (thao tác nhạy cảm) dạng danh sách đánh số điều hướng bằng phím:
  ```
  ╭───────────────────────────────────────────────╮
  │ Đặt DNS hệ thống về 127.0.0.1?                │
  │ Bản gốc sẽ được sao lưu và khôi phục bằng     │
  │ /system restore.                              │
  │ ❯ 1. Có                                       │
  │   2. Có, đừng hỏi lại                         │
  │   3. Không                                    │
  ╰───────────────────────────────────────────────╯
  ```
- **Dòng trạng thái:** `● <số peer> peer · <số domain> domain · <số xung đột> xung đột`, bên phải là gợi ý phím theo ngữ cảnh.
- Phím tắt: `Enter` gửi · `Esc` ngắt tác vụ / xóa ô nhập · `Ctrl+C` (2 lần) thoát · `Ctrl+D` thoát khi ô rỗng · `Ctrl+L` xóa màn hình · `Ctrl+R` tìm lịch sử · `Tab` hoàn thành · `?` xem phím tắt.
- Khả năng thích nghi: nhận biết `NO_COLOR`; terminal không có Unicode → fallback ASCII; tự đổi kích thước khi cửa sổ đổi; tôn trọng độ rộng ký tự (CJK/emoji, tiếng Việt có dấu).
- Windows: bật VT processing + UTF-8 (code page 65001); khuyến nghị Windows Terminal; conhost cũ dùng fallback ASCII.
- Hỗ trợ tiếng Việt và tiếng Anh (`ui.language = "vi" | "en"`, mặc định theo locale).

### 10.3 Danh sách lệnh (slash trong TUI = lệnh CLI cùng tên)

| Lệnh | Chức năng |
|---|---|
| `/add <tên> [đích…] [--type A\|AAAA\|CNAME\|TXT] [--ttl N] [--force]` | Tạo domain (đích mặc định `self`) |
| `/set <tên> <đích…>` | Đổi đích |
| `/rm <tên> [--type T]` | Xóa |
| `/ls [--mine\|--all\|--peer <tên>] [--json]` | Liệt kê domain |
| `/info <tên>` | Ai đang claim, các nhóm bản, số phiếu, thứ hạng |
| `/resolve <tên> [type] [--trace]` | Chạy pipeline; `--trace` in từng bước (1/2/3), peer nào trả lời, thời gian |
| `/peers` | Danh sách peer: trạng thái, RTT, số domain, phiên bản |
| `/peer add <ip[:port]>` · `/peer rm <ip>` · `/peer forget <node>` | Quản lý peer |
| `/upstream [list\|add <ip>\|rm <ip>\|set <ip…>]` | DNS mặc định/cấu hình |
| `/conflicts` | Tên có nhiều bản khác nhau |
| `/logs [--follow] [--filter …]` | Nhật ký truy vấn thời gian thực (cột: bước L/P/U, rcode, µs) |
| `/status` | Tổng quan: bind, quyền, peer, cảnh báo bảo mật |
| `/network [name <tên>\|key set\|key clear]` | Tên mạng, khóa PSK |
| `/system setup\|restore` | Đặt/khôi phục DNS hệ điều hành |
| `/service install\|uninstall\|start\|stop\|status` | Dịch vụ nền |
| `/doctor` | Chẩn đoán: port 53, systemd-resolved, firewall, multicast, quyền |
| `/config get\|set <khóa> [giá trị]` · `/import` · `/export` | Cấu hình, nhập/xuất |
| `/theme` · `/clear` · `/help` · `/version` · `/exit` | Tiện ích |

Mọi thông báo lỗi phải có **nguyên nhân + cách khắc phục** (ví dụ: "Không bind được :53 — `systemd-resolved` đang giữ 127.0.0.53. Ldns dùng 127.0.0.1 nên bình thường vẫn chạy; nếu vẫn lỗi, chạy `/doctor`").

---

## 11. Cấu hình (`config.toml`)

```toml
[network]
name = "default"               # đổi để tách mạng Ldns trong cùng LAN
key_file = ""                  # đường dẫn PSK; để trống = không mã hóa
interfaces = "auto"            # hoặc ["eth0","Wi-Fi"]
self_interface = ""            # ghim interface cho record self
self_ip = ""                   # ghim IP cho record self
discovery_port = 5390
sync_port = 5391
manual_peers = []              # ["192.168.2.5:5391"]

[server]
listen = "auto"                # hoặc ["127.0.0.1:53","192.168.1.10:53"]
allow = ["private"]            # "private" | CIDR…
rate_limit_qps = 200

[peers]
hedge_ms = 250
budget_ms = 1500
serve_stale = true
offline_retention = "24h"

[upstream]
servers = ["auto"]             # "auto" = DNS hệ thống đã lưu (loại chính mình), nếu trống dùng fallback
fallback = ["1.1.1.1", "8.8.8.8"]
timeout_ms = 2000
retries = 1

[cache]
max_entries = 50000
peer_ttl_cap = 30
upstream_ttl_cap = 300
negative_ttl = 5

[domains]
default_ttl = 60
warn_public_tld = true

[system]
manage_os_dns = false
restore_on_exit = true

[ui]
language = "auto"
theme = "auto"                 # auto | dark | light | none
```

Thứ tự ưu tiên: tham số CLI > biến môi trường `LDNS_*` > `config.toml` > mặc định.

---

## 12. Lưu trữ & đường dẫn

| | Linux (người dùng) | Linux (service) | Windows (người dùng) | Windows (service) |
|---|---|---|---|---|
| Cấu hình | `~/.config/ldns/config.toml` | `/etc/ldns/config.toml` | `%APPDATA%\ldns\config.toml` | `%ProgramData%\ldns\config.toml` |
| Dữ liệu | `~/.local/share/ldns/ldns.db` | `/var/lib/ldns/ldns.db` | `%APPDATA%\ldns\ldns.db` | `%ProgramData%\ldns\ldns.db` |
| IPC | `$XDG_RUNTIME_DIR/ldns.sock` | `/run/ldns/ldns.sock` | `\\.\pipe\ldns` | `\\.\pipe\ldns` |

Khi đã có service chạy, TUI/CLI người dùng luôn gắn vào service (một DB duy nhất, không hai tiến trình tranh port).

---

## 13. Tích hợp hệ điều hành

### 13.1 Port 53
- Bind **địa chỉ cụ thể** (D14). Ubuntu/Debian có `systemd-resolved` giữ `127.0.0.53:53` — bind `127.0.0.1:53` và IP LAN không đụng nhau.
- Quyền: Linux cần root hoặc `CAP_NET_BIND_SERVICE` (service: `AmbientCapabilities=CAP_NET_BIND_SERVICE`; thủ công: `setcap`). Windows cần Administrator cho firewall; bind 53 thường không cần nhưng có thể đụng ICS/Hyper-V/WSL.
- Bind thất bại → hộp thoại gợi ý: chạy với quyền cao, hoặc dùng cổng thử nghiệm `5300` (chỉ dùng được với client chủ động chỉ cổng), hoặc chạy `/doctor`.

### 13.2 Đặt DNS hệ thống (`/system setup`) — luôn có sao lưu + khôi phục
| HĐH | Cách làm |
|---|---|
| Linux + systemd-resolved | `resolvectl dns <iface> 127.0.0.1` (+ `resolvectl domain` nếu cần), khôi phục bằng `resolvectl revert` |
| Linux + NetworkManager | `nmcli con mod <con> ipv4.dns 127.0.0.1 ipv4.ignore-auto-dns yes` |
| Linux khác | sửa `/etc/resolv.conf` (sao lưu `.ldns.bak`) |
| Windows | `Set-DnsClientServerAddress -InterfaceIndex <i> -ServerAddresses 127.0.0.1` (sao lưu giá trị cũ vào DB) |

Rủi ro "Ldns tắt thì mất DNS" → giảm nhẹ bằng: service tự khởi động lại, `restore_on_exit`, và danh sách DNS hệ thống = `[127.0.0.1, <DNS gốc>]` khi người dùng chọn chế độ an toàn.

### 13.3 Service
- Linux: unit systemd `ldns.service` (User=ldns, hardening: `NoNewPrivileges`, `ProtectSystem=strict`…).
- Windows: dịch vụ SCM (`ldns`), khởi động tự động, khôi phục khi lỗi.

### 13.4 Firewall (`/doctor` đề xuất, `/service install` có thể tự áp dụng khi được phép)
Mở: UDP/TCP 53, UDP 5390 (multicast), TCP 5391 — chỉ cho subnet LAN. Linux: `ufw`/`firewalld`/`nft` được nhận diện; Windows: `New-NetFirewallRule`.

---

## 14. Bảo mật

**Mô hình đe dọa v1:** mạng LAN *bán tin cậy* (nhà, văn phòng nhỏ, lớp học). Mục tiêu: ngăn người ngoài mạng Ldns can thiệp, giảm thiệt hại từ node xấu; **không** chống được chủ máy cố tình trong cùng mạng.

| Biện pháp | Chi tiết |
|---|---|
| Khóa mạng (PSK) | HMAC beacon; handshake xác thực; AEAD toàn bộ phiên. Không có khóa → mạng mở (cảnh báo ở `/status`) |
| Bỏ phiếu ≠ bảo mật | Quy tắc đa số chỉ chọn bản, **không** chống Sybil. Mô tả rõ trong tài liệu; khuyến nghị bật PSK trong mạng không tin cậy |
| ACL DNS | Chỉ phục vụ nguồn private (mặc định); không đệ quy cho Internet |
| Rate limit | DNS theo nguồn; kết nối peer: tối đa 64 peer, giới hạn tốc độ UPDATE/peer (mặc định 200 op/s) |
| Kích thước | Frame ≤ 4 MiB; từ chối op sai định dạng; đóng kết nối vi phạm lặp lại |
| Chống chiếm tên công cộng | Xác nhận TLD công cộng (§9.1) |
| IPC | Unix socket 0600 / pipe ACL theo người dùng + Administrators/nhóm `ldns` |
| Tệp cấu hình/khóa | Quyền 0600 / ACL người dùng |
| Nhật ký | Không ghi PSK; nội dung truy vấn lưu cục bộ, giới hạn dòng, tắt được (`log.queries=false`) |

---

## 15. Yêu cầu phi chức năng

| Hạng mục | Mục tiêu |
|---|---|
| Độ trễ trả lời cục bộ | p99 < 1 ms |
| Độ trễ bước 2 | thường < 50 ms; ngân sách tối đa 1500 ms |
| Lan truyền thay đổi | < 500 ms tới mọi peer đang kết nối |
| Node mới đồng bộ đủ | < 2 s với ≤ 10 000 record |
| Thông lượng | ≥ 10 000 qps (hit cache/local) trên máy 4 nhân |
| Quy mô | ≥ 100 peer, ≥ 100 000 record tổng (RAM ≤ 100 MB) |
| Khởi động | < 300 ms tới khi DNS sẵn sàng |
| Độ bền | Crash giữa chừng không hỏng DB (WAL); không mất domain đã báo thành công |
| Tài nguyên rảnh | CPU < 1%, RAM < 30 MB khi không có tải |
| Kích thước | Binary tĩnh < 15 MB |
| Nền tảng | Windows 10/11 x64, Ubuntu 22.04+/Debian 12+/Fedora (glibc), x86_64 và aarch64 (Linux) |

---

## 16. Quan sát & nhật ký

- `spdlog`: mức error/warn/info/debug/trace; xoay vòng tệp; `--log-level`.
- `QueryLog` trong DB (vòng) phục vụ `/logs`.
- Sự kiện real-time (`EventBus`): `PeerUp/Down`, `RecordPut/Del{owner}`, `Conflict`, `BindFail`, `IpChanged`, `ConfigChanged`.
- Metrics nội bộ (xem bằng `/status --verbose`): qps theo bước L/P/U, tỉ lệ cache, số peer, độ trễ bước 2, số UPDATE nhận/gửi.

---

## 17. Kiểm thử & tiêu chí chấp nhận

### 17.1 Chiến lược
- **Unit:** codec DNS, chuẩn hóa tên, Registry/bỏ phiếu/xếp hạng/hòa phiếu, HLC, state machine kết nối, parser TUI (phím, độ rộng ký tự).
- **Fuzz:** parser DNS và parser frame peer (libFuzzer/AFL++, chạy trong CI).
- **Property/Simulation:** mô phỏng N node với mạng giả (mất gói, trễ, chia mạng); khẳng định hội tụ.
- **Integration:** nhiều tiến trình Ldns trên một máy (mỗi tiến trình một IP loopback `127.0.0.x` + cổng riêng); Linux network namespaces + `tc netem`; Windows runner + hai tiến trình.
- **TUI:** snapshot test với terminal ảo (ghi lại chuỗi ANSI → so khớp lưới ký tự).
- **Hệ thống:** chạy thật trên 3 máy (2 Linux + 1 Windows) cho kịch bản nghiệm thu.

### 17.2 Tiêu chí chấp nhận theo yêu cầu cốt lõi

**R1 — Luồng phân giải**
- AC-1.1 Tên do mình sở hữu: trả từ bước 1, không phát sinh gói mạng nào.
- AC-1.2 Tên chỉ peer có: bước 2 trả đúng; `--trace` cho thấy peer nào trả lời.
- AC-1.3 Nhiều bản khác nhau: nhóm nhiều phiếu hơn được chọn; hòa phiếu giải quyết xác định theo D10 (cùng kết quả trên mọi node).
- AC-1.4 Peer của nhóm đầu bị tắt đột ngột: tự động thử peer khác trong nhóm, rồi nhóm kế, đến hết; tổng thời gian ≤ ngân sách.
- AC-1.5 Không còn ứng viên/tất cả lỗi: chuyển upstream; domain Internet (vd `example.com`) vẫn phân giải bình thường và không bị chậm thêm > 5 ms so với forward trực tiếp.
- AC-1.6 Tên chủ sở hữu có giá trị khác số đông: máy chủ sở hữu vẫn nhận bản của mình (D11).
- AC-1.7 Không có vòng lặp khi hai node cấu hình upstream trỏ vào nhau.

**R2 — Tự kết nối & real-time**
- AC-2.1 Hai máy cùng LAN, mở Ldns, **không cấu hình gì**: thấy nhau trong ≤ 5 s.
- AC-2.2 Thêm domain trên máy A: máy B hỏi được trong < 500 ms.
- AC-2.3 Xóa/sửa domain: các máy khác phản ánh trong < 500 ms; cache peer không còn trả giá trị cũ.
- AC-2.4 Tắt đột ngột máy A: các máy khác đánh dấu offline trong ≤ 15 s; mở lại thì tự đồng bộ delta.
- AC-2.5 Đổi IP máy A: record `self` được cập nhật và lan truyền ≤ 10 s.
- AC-2.6 Multicast bị chặn: `/peer add` (hoặc peer exchange qua một node cầu nối) vẫn tạo mạng đầy đủ.
- AC-2.7 Chia mạng rồi nối lại: hội tụ về cùng trạng thái, không mất domain của owner nào.

**R3 — Vô hạn domain**
- AC-3.1 Tạo 100 000 domain bằng `/import`: không lỗi, tiêu thụ RAM trong ngưỡng §15, đồng bộ cho peer thành công.
- AC-3.2 Domain `self` trỏ đúng IP LAN của máy tạo; domain tĩnh trỏ đúng IP/CNAME đã cấu hình.
- AC-3.3 Wildcard hoạt động; bản ghi chính xác ưu tiên hơn wildcard.
- AC-3.4 Tên trùng TLD công cộng yêu cầu xác nhận.

**UX & nền tảng**
- AC-4.1 Cùng bộ lệnh và hành vi trên Windows Terminal và terminal Linux (gnome-terminal, kitty, tmux, ssh).
- AC-4.2 TUI giữ scrollback; thay đổi kích thước không làm hỏng bố cục; `NO_COLOR` được tôn trọng.
- AC-4.3 Mọi lệnh slash có bản CLI một lần và `--json` cho script.
- AC-4.4 Thiết bị không chạy Ldns (điện thoại) đặt DNS = IP LAN của một máy Ldns vẫn phân giải được domain của cả mạng.

---

## 18. Phụ lục

### A. Cấu trúc thư mục dự kiến
```
ldns/
├─ CMakeLists.txt, vcpkg.json, CMakePresets.json
├─ cmake/                      # toolchain, helper
├─ src/
│  ├─ core/                    # logic độc lập UI
│  │  ├─ dns/                  # codec, message, name, rr
│  │  ├─ server/               # udp/tcp listener, acl, rate_limit
│  │  ├─ resolver/             # pipeline, local_zone, peer_resolver, upstream, cache
│  │  ├─ registry/             # owner_state, ops, voting
│  │  ├─ net/                  # discovery, peer_manager, session, protocol, crypto
│  │  ├─ store/                # sqlite, migrations
│  │  ├─ config/, events/, util/
│  │  └─ core_api.hpp          # giao diện duy nhất cho frontend
│  ├─ ipc/                     # json-rpc server/client (unix socket, named pipe)
│  ├─ os/                      # platform: linux/, windows/ (dns setup, service, firewall, netchange)
│  ├─ ui/
│  │  ├─ term/                 # raw mode, VT input parser, width, inline renderer
│  │  ├─ widgets/              # input_box, slash_menu, spinner, dialog, panel, theme
│  │  └─ commands/             # khai báo lệnh dùng chung cho TUI và CLI
│  └─ main.cpp
├─ tests/ (unit/, fuzz/, sim/, integration/, tui/)
├─ packaging/ (systemd/, wix/, deb/)
└─ docs/ (SPEC.md, PLAN.md, protocol.md)
```

### B. Phụ thuộc
Asio (standalone), nlohmann::json, SQLite3, toml++, libsodium, spdlog, CLI11, doctest (hoặc Catch2), tùy chọn utf8proc. Quản lý bằng vcpkg manifest, liên kết tĩnh.

### C. Giá trị mặc định thời gian (tóm tắt)
Beacon 2s→10s ±20% · Heartbeat 5s / chết sau 15s · Hedge 250ms · Ngân sách bước 2 1500ms · Upstream timeout 2s ×2 · TTL record 60s · Trần TTL peer 30s · Giữ owner offline 24h · Negative cache 5s.

### D. Câu hỏi để mở cho v1.1 (không chặn v1.0)
Tên `<node>.ldns` tự sinh · PTR/reverse · MX/SRV · DoH/DoT cho upstream · xác thực owner bằng chữ ký khóa công khai (chống giả mạo trong cùng mạng) · macOS · gói winget/apt/brew.
