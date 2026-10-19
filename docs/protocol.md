# Ldns — Giao thức ngang hàng (PROTOCOL) v0.1

> Mô tả **trên dây (wire)** mọi thứ hai node Ldns trao đổi với nhau: phát hiện peer, bắt tay, đồng bộ registry, hỏi đáp `RESOLVE`.
> Tham chiếu: `SPEC.md` §6.2d, §7, §14 · `PLAN.md` M4, M5, M6, M9.
> Khi file này lệch SPEC, **SPEC là nguồn sự thật** — sửa SPEC trước, rồi cập nhật file này. Các điểm file này *bổ sung* so với SPEC v0.1 được liệt kê ở §16 để đưa ngược vào SPEC.

---

## 0. Phạm vi và quy ước

**Trong phạm vi:** giao thức giữa các node Ldns (UDP discovery cổng 5390, TCP sync cổng 5391).
**Ngoài phạm vi:** giao thức DNS với client (RFC 1035, xem SPEC §8) và JSON-RPC IPC giữa TUI/CLI với daemon (SPEC §4, PLAN M8).

Từ khóa **PHẢI**, **KHÔNG ĐƯỢC**, **NÊN**, **CÓ THỂ** hiểu theo RFC 2119.

Nguyên tắc thiết kế (rút từ SPEC D5–D10):
1. **Single-writer:** mỗi owner là nguồn sự thật duy nhất cho zone của mình; thay đổi được đánh số bằng `seq` tăng dần (D8).
2. **Giao thức không phân xử xung đột.** Nó chỉ chuyển dữ liệu trung thực; việc chọn bản trả lời xảy ra lúc phân giải (D9, §8).
3. **Bền với mất gói/chia mạng:** mọi trạng thái sửa được bằng `SYNC_REQ` từ `(epoch, seq)`.
4. **Dễ debug:** MessagePack với khóa chuỗi đọc được; có thể dump ra JSON (D5).

---

## 1. Tổng quan các lớp

| Lớp | Giao thức | Cổng | Vai trò |
|---|---|---|---|
| Discovery | UDP multicast `239.76.68.83` + broadcast dự phòng | 5390 | Báo "tôi đây", tìm node lạ |
| Phiên (session) | TCP, frame có tiền tố độ dài, MessagePack | 5391 | Bắt tay, heartbeat, đồng bộ, `RESOLVE` |
| Mật mã (tùy chọn) | HMAC-SHA256 + XChaCha20-Poly1305 (libsodium) | — | Khi mạng có khóa PSK |

Vòng đời một quan hệ peer:

```
(chưa biết) ──beacon/manual/PEERS──► DIALING ──TCP──► HANDSHAKE ──HELLO(+AUTH)──► ESTABLISHED
     ▲                                  │ lỗi                │ lỗi/timeout              │ PING mất 3 lần / BYE / EOF
     └───────── backoff 1s→60s ◄────────┴────────────────────┴──────────────────────────┘ → OFFLINE
```

---

## 2. Kiểu dữ liệu dùng chung

### 2.1 Mã hóa
- Mọi thông điệp là **một map MessagePack** (qua `nlohmann::json::to_msgpack`). Khóa là chuỗi ASCII.
- Trường nhị phân (`node`, `nonce`, địa chỉ IP, …) dùng kiểu `bin` của MessagePack. Trong tài liệu này viết `bin(n)` = n byte; ví dụ bên dưới hiển thị hex cho dễ đọc.
- Số nguyên không dấu dùng dạng ngắn nhất MessagePack cho phép. Bên nhận **PHẢI** chấp nhận mọi dạng hợp lệ.
- **Bên nhận bỏ qua khóa lạ** (để mở rộng tương thích về sau, §15).

### 2.2 Định danh và thời gian
| Kiểu | Biểu diễn | Ghi chú |
|---|---|---|
| `node_id` | `bin(16)` | Ngẫu nhiên 128-bit, lưu bền (SPEC §2). So sánh theo thứ tự byte (big-endian lexicographic) |
| `epoch` | `uint64` | Số ngẫu nhiên, đổi khi owner mất/reset dữ liệu (§7.5) |
| `seq` | `uint64` | Tăng 1 cho **mỗi giao dịch** thay đổi zone của owner (§7.2) |
| `ts` (HLC) | `uint64` | 48 bit cao = mili-giây Unix, 16 bit thấp = bộ đếm logic |
| `net` | `bin(8)` | `SHA-256("ldns-net-v1:" ‖ network_name)[0:8]` |
| `v` | `[major, minor]` | Phiên bản giao thức; hiện là `[1, 0]` |

### 2.3 Tên và RR
- Tên **PHẢI** đã chuẩn hóa như SPEC §5.1 (chữ thường, bỏ dấu chấm cuối, punycode, nhãn ≤ 63, tổng ≤ 253). Bên nhận **KHÔNG** tự chuẩn hóa hộ: tên không chuẩn → op bị từ chối (§10).
- `type` là chuỗi: `"A"`, `"AAAA"`, `"CNAME"`, `"TXT"` (v1). Type lạ → op bị từ chối.
- Phần tử của `data[]` theo từng type:

| type | Phần tử `data[i]` | Ràng buộc |
|---|---|---|
| A | `bin(4)` | IPv4 |
| AAAA | `bin(16)` | IPv6 |
| CNAME | `str` — tên đã chuẩn hóa | Đúng **1** phần tử |
| TXT | `bin` — nội dung thô | ≤ 1024 byte/phần tử; bộ mã DNS tự cắt thành chuỗi ≤ 255 byte khi xuất ra gói DNS |

- `ttl`: `uint32` giây, ≤ 2 147 483 647.
- RRset: `1 ≤ len(data) ≤ 64`.

### 2.4 Dạng chuẩn của RRset (canonical form)
Dùng để so sánh "bản giống hệt" khi bỏ phiếu (SPEC §2, PLAN M6):
1. Sắp xếp `data[]` tăng dần theo so sánh byte (với CNAME: theo byte UTF-8 của tên).
2. Loại phần tử trùng.
3. **Bỏ qua `ttl`** khi so sánh.

Khóa nhóm bản = `(type, data đã chuẩn)`.

### 2.5 Op (đơn vị thay đổi)
```
put: { op:"put", name, type, ttl, data[], created, ts }
del: { op:"del", name, type, ts }
```
- `ts` = thời điểm thay đổi này (= `updated_at`).
- `created` = thời điểm claim `(name,type)` **được tạo lần đầu** (= `created_at`); giữ nguyên qua các lần `/set`, chỉ đổi khi xóa rồi tạo lại. Trường này cần để xếp hạng "claim cũ nhất" (D10) giống hệt nhau trên mọi node.
- Thứ tự áp dụng op **chỉ theo `seq`** (§7), không theo `ts`.

---

## 3. Discovery (UDP 5390)

### 3.1 Datagram
```
+--------+-------+-----------------------+-----------------+
| "LDB1" | flags | body (MessagePack map)| mac (16B, nếu   |
| 4 byte | 1 B   |                       |  flags & 0x01)  |
+--------+-------+-----------------------+-----------------+
```
- Tổng kích thước ≤ **1200 byte** (tránh phân mảnh IP). Datagram quá lớn/sai magic/sai MessagePack → bỏ im lặng.
- `flags` bit 0 = có `mac`. Các bit khác = 0.
- `mac` = `HMAC-SHA256(K_beacon, "LDB1" ‖ flags ‖ body)[0:16]`. Mạng có PSK mà datagram không có `mac` hoặc `mac` sai → **bỏ**. Mạng không PSK bỏ qua `mac` nếu có.

### 3.2 Body
| Trường | Kiểu | Ý nghĩa |
|---|---|---|
| `t` | `"beacon"` \| `"bye"` | `bye` gửi khi thoát êm |
| `v` | `[maj,min]` | Phiên bản giao thức |
| `node` | `bin(16)` | node_id |
| `name` | `str` | Tên hiển thị của node |
| `net` | `bin(8)` | Hash tên mạng |
| `sync_port` | `uint16` | Cổng TCP sync |
| `dns_port` | `uint16` | Cổng DNS |
| `ips` | `[bin(4)\|bin(16)]` | Các IP unicast của node trên interface mạng Ldns |
| `epoch`, `seq` | `uint64` | Trạng thái zone của chính node |
| `ts` | `uint64` | HLC lúc gửi (chỉ để thông tin) |

### 3.3 Gửi
- Gửi trên **từng interface hợp lệ** (bỏ loopback/ảo theo `network.interfaces`): multicast `239.76.68.83:5390` **và** broadcast subnet (dự phòng khi multicast bị chặn). TTL multicast = 1.
- Lịch: ngay khi khởi động → mỗi 2 s trong 10 s đầu → sau đó mỗi 10 s ± 20% jitter.
- Gửi thêm một beacon ngay khi `self` đổi IP hoặc `seq` đổi (nhưng không quá 1 beacon/giây).
- Thoát êm: gửi `bye` (3 lần cách 100 ms).

### 3.4 Nhận
1. Bỏ gói từ chính mình (`node` trùng) và gói khác `net`.
2. Khác `major` của `v` → bỏ (ghi log mức debug, không báo lỗi lặp).
3. Node lạ cùng `net` → tạo/cập nhật mục peer-candidate; nếu chưa có phiên → **áp dụng quy tắc dial (§9.1)**.
4. Beacon của peer đã có phiên: nếu `(epoch,seq)` mới hơn bản đã biết → gửi `SYNC_REQ` cho owner đó.
5. `bye`: đánh dấu peer-candidate "đã thoát"; **không** tự đóng phiên TCP (phiên đóng bằng `BYE` hoặc heartbeat). Phát lại beacon của node đó trong 3 s sau `bye` không được kích hoạt dial.
6. Địa chỉ dial: ưu tiên **địa chỉ nguồn** của datagram, sau đó các IP trong `ips`.
7. Replay beacon vô hại (chỉ gây thử dial; phiên TCP luôn tự xác thực). `ts` không dùng để từ chối.

---

## 4. Phiên TCP (cổng 5391)

### 4.1 Khung (frame)
```
+------------------+----------------------------------+
| length (u32, BE) | payload (length byte)            |
+------------------+----------------------------------+
```
- `length` ≤ **4 MiB** (4 194 304). Vượt → đóng kết nối, `ERROR MALFORMED`.
- Chưa mã hóa: `payload` = MessagePack map của thông điệp.
- Có PSK, sau khi dẫn xuất khóa (§5.3): `payload = ciphertext ‖ tag(16)` của XChaCha20-Poly1305-IETF; `length` tính cả tag.

### 4.2 Mã hóa phiên (khi có PSK)
- Mỗi chiều có khóa riêng (`K_i2r`, `K_r2i`, §5.3).
- Nonce 24 byte = `counter (u64, little-endian) ‖ 16 byte 0`. `counter` bắt đầu từ 0 mỗi chiều, tăng 1 sau mỗi frame, **không truyền trên dây**. Bên nhận dùng bộ đếm riêng; giải mã sai = đóng kết nối ngay (nên cũng chống replay/đảo thứ tự/mất frame).
- Associated data = 4 byte `length`.
- `counter` chạm 2⁴⁸ → đóng phiên và nối lại (khóa mới nhờ nonce mới).

### 4.3 Thông điệp chung
Mọi thông điệp có khóa `t` (chuỗi) = tên loại. Loại lạ sau `ESTABLISHED` → **bỏ qua** (ghi log debug). Loại lạ trước `ESTABLISHED` → đóng.

---

## 5. Bắt tay (handshake)

Bên **dial** (khởi tạo, *initiator*) luôn là node có `node_id` nhỏ hơn (§9.1). Bên còn lại là *responder*.

### 5.1 Trình tự

```
Initiator                                       Responder
   │──── TCP connect ───────────────────────────►│
   │──── HELLO(I) ──────────────────────────────►│   kiểm tra net, v, node, số peer
   │◄─── HELLO(R) ───────────────────────────────│
   │  (nếu cả hai psk=true: dẫn xuất khóa, §5.3) │
   │──── AUTH(I) [mã hóa] ──────────────────────►│   kiểm tra mac
   │◄─── AUTH(R) [mã hóa] ───────────────────────│
   │                  ESTABLISHED                │
   │──── SYNC_REQ + PEERS ──────────────────────►│
   │◄─── SYNC_REQ + PEERS ───────────────────────│   (đồng thời, hai chiều)
```
Toàn bộ bắt tay có hạn **5 s**; quá hạn → đóng.

### 5.2 `HELLO`
```
{ t:"HELLO", v:[1,0], node, name, net, epoch, seq,
  ips[], sync_port, dns_port, caps[], nonce:bin(32), psk:bool }
```
- `caps`: danh sách chuỗi tính năng tùy chọn; v1.0 gửi `["relay","snapshot-chunk"]`.
- `nonce`: 32 byte ngẫu nhiên (CSPRNG). Có thể gửi `bin(32)` toàn 0 khi `psk=false`.
- `psk`: node có đặt khóa mạng hay không.

Bên nhận kiểm tra theo thứ tự, lỗi nào gặp trước gửi `ERROR` rồi đóng:

| Điều kiện | `ERROR.code` |
|---|---|
| `v.major` khác | 1 `VERSION` |
| `net` khác | 2 `NETWORK` |
| `psk` hai bên khác nhau | 3 `AUTH` |
| `node` trùng node_id của chính mình | 7 `SELF` |
| Đã đủ 64 peer (và peer này chưa có) | 6 `TOO_MANY_PEERS` |
| Đã có phiên với `node` này và phiên này **không** thắng quy tắc §9.1 | 8 `DUPLICATE` |

### 5.3 Dẫn xuất khóa (chỉ khi `psk=true`)
- `PSK` = nội dung tệp `network.key_file` (≥ 16 byte; NÊN sinh bằng `ldns network key generate`).
- `K_root = HKDF-SHA256-Extract(salt="ldns-psk-v1", ikm=PSK)`.
- `K_beacon = HKDF-Expand(K_root, "beacon", 32)`.
- `TH = SHA-256(bytes payload HELLO(I) ‖ bytes payload HELLO(R))` — dùng **đúng byte trên dây**, không mã hóa lại để tránh lệch canonical.
- `K_i2r = HKDF-Expand(K_root, "i2r" ‖ TH, 32)`, `K_r2i = HKDF-Expand(K_root, "r2i" ‖ TH, 32)`.
- `K_auth = HKDF-Expand(K_root, "auth", 32)`.

### 5.4 `AUTH` (chỉ khi `psk=true`; frame đầu tiên của mỗi chiều, đã mã hóa)
```
{ t:"AUTH", mac: HMAC-SHA256(K_auth, role ‖ TH) }
```
`role` = byte `0x49` (`'I'`) khi do initiator gửi, `0x52` (`'R'`) khi do responder gửi. Giải mã AEAD sai hoặc `mac` sai → `ERROR AUTH` (khi có thể) rồi đóng; **không** tiết lộ vế nào sai.

### 5.5 Sau `ESTABLISHED`
Mỗi bên:
1. Đánh dấu owner tương ứng `online`.
2. Gửi `SYNC_REQ` liệt kê **mọi** owner mình biết (§7.3).
3. Gửi `PEERS` (§6.7).
4. Bắt đầu heartbeat (§9.3).

---

## 6. Thông điệp

### 6.1 `PING` / `PONG`
```
{ t:"PING", n:uint32, epoch, seq }
{ t:"PONG", n:uint32, epoch, seq }
```
- `n` do bên gửi PING chọn; PONG lặp lại `n` → đo RTT (dùng cho "RTT thấp trước" ở §8.3).
- `epoch`/`seq` là trạng thái **zone của người gửi**. Bên nhận thấy mới hơn bản đã biết → gửi `SYNC_REQ` cho owner đó. Đây là cơ chế **vá sót** rẻ tiền (chu kỳ 5 s).

### 6.2 `SYNC_REQ`
```
{ t:"SYNC_REQ", id:uint32,
  owners:[ { owner:node_id, epoch, since_seq }, ... ] }
```
- `since_seq` = `seq` cuối cùng mà bên gửi **đã áp dụng** cho owner đó.
- Cách trả lời — xem §7.3.

### 6.3 `SYNC_DATA`
```
{ t:"SYNC_DATA", id:uint32, owner, epoch,
  from_seq, to_seq,
  snapshot:bool, chunk:uint32, last:bool,
  ops:[ op, ... ] }
```
- **Delta** (`snapshot=false`): `ops` đã **nén** (với mỗi `(name,type)` chỉ giữ thao tác cuối trong khoảng `(from_seq, to_seq]`, kể cả `del`). Áp dụng xong → `last_seq = to_seq`. Luôn `chunk=0, last=true`.
- **Snapshot** (`snapshot=true`): `from_seq=0`, toàn bộ zone tại `to_seq`, chỉ gồm `put`. Chia thành các chunk **liên tiếp** `chunk=0,1,2…`, mỗi chunk ≤ 1000 op và ≤ 4 MiB; chunk cuối có `last=true`.
- Mọi `SYNC_DATA` thuộc cùng một `(id, owner)` do cùng một phiên gửi tuần tự.

### 6.4 `SYNC_END`
```
{ t:"SYNC_END", id:uint32 }
```
Bên trả lời gửi sau khi đã trả xong mọi owner cho một `SYNC_REQ` có `id` tương ứng (kể cả khi không có gì để gửi). Cho phép UI báo "đã đồng bộ xong" và đo mục tiêu < 2 s.

### 6.5 `UPDATE`
```
{ t:"UPDATE", owner, epoch, seq, ops:[ op, ... ] }
```
- Một `UPDATE` = **một** giao dịch của owner = đúng **một** bước `seq` (§7.2). `1 ≤ len(ops) ≤ 1000`.
- Được đẩy tức thì tới mọi peer `ESTABLISHED`, và **relay** theo §7.4.

### 6.6 `RESOLVE` / `RESOLVE_R`
```
{ t:"RESOLVE",   id:uint32, name, type }
{ t:"RESOLVE_R", id:uint32, rcode:uint8, epoch, seq,
  answers:[ { name, type, ttl, data[] }, ... ] }
```
- Ngữ nghĩa chi tiết ở §8.
- `rcode`: `0` NOERROR · `2` SERVFAIL (lỗi nội bộ/quá tải) · `3` NXDOMAIN · `5` REFUSED (ACL/rate limit).
- `epoch`/`seq` = trạng thái zone của peer lúc trả lời; bên hỏi dùng để biết registry của mình có lạc hậu không.

### 6.7 `PEERS`
```
{ t:"PEERS", peers:[ { node:node_id, addrs:["192.168.1.5:5391", ...] }, ... ] }
```
- ≤ 64 phần tử. Gửi sau `ESTABLISHED` và khi học được peer mới (gộp, không quá 1 lần/5 s).
- Bên nhận chỉ chấp nhận địa chỉ private/loopback/link-local (cùng ACL SPEC §8), bỏ node_id của chính mình hoặc đã có phiên; còn lại → hàng đợi dial (§9.1).
- Địa chỉ trong `PEERS` là **gợi ý chưa xác thực**: chỉ dial thử, tin cậy hay không do bắt tay quyết định.

### 6.8 `BYE`
```
{ t:"BYE" }
```
Thoát êm. Bên nhận đóng phiên, đánh dấu peer `offline` ngay, **không** thử nối lại trong 3 s.

### 6.9 `ERROR`
```
{ t:"ERROR", code:uint8, msg:str }
```
Luôn là frame cuối cùng trước khi đóng. `msg` chỉ để người đọc (≤ 200 ký tự, KHÔNG chứa thông tin khóa).

| code | Tên | Ý nghĩa |
|---|---|---|
| 1 | `VERSION` | Khác major |
| 2 | `NETWORK` | Khác `net` |
| 3 | `AUTH` | PSK sai/thiếu/không khớp |
| 4 | `RATE` | Vượt giới hạn tốc độ lặp lại |
| 5 | `MALFORMED` | Frame/op sai định dạng lặp lại |
| 6 | `TOO_MANY_PEERS` | Đã đủ 64 peer |
| 7 | `SELF` | Tự kết nối chính mình |
| 8 | `DUPLICATE` | Trùng phiên, giữ phiên kia |

---

## 7. Đồng bộ Registry

### 7.1 Trạng thái mỗi bên giữ cho từng owner
`(epoch, last_seq, records, status, last_seen)` (SPEC §5.3). Owner của chính mình có thêm **op log** (§7.2).

### 7.2 Phía owner
1. Thay đổi cục bộ (`add/set/rm/import`, đổi IP của `self`) chạy trong **một giao dịch SQLite**: ghi `my_records` + tăng `seq` + ghi op log. Commit xong mới phát.
2. **Một giao dịch = một `seq`.** Nếu giao dịch có ≤ 1000 op → gửi `UPDATE`. Nếu > 1000 op (ví dụ `/import` 100 000 dòng) → xem "Thay đổi lớn" bên dưới.
3. Op log giữ **tối thiểu** 24 giờ hoặc 10 000 `seq` gần nhất (cái nào dài hơn), có thể nén theo `(name,type)`. Peer hỏi `since_seq` cũ hơn mốc còn giữ → trả **snapshot**.
4. `ts`/`created` lấy từ HLC cục bộ; không bao giờ lùi.

**Thay đổi lớn (> 1000 op trong một giao dịch):** owner tăng `seq` một bước rồi đẩy **snapshot chủ động** (chuỗi `SYNC_DATA` với `id=0`, `snapshot=true`, `to_seq` = `seq` mới) tới mọi peer thay cho `UPDATE`. Cách này tránh giới hạn tốc độ `UPDATE` (§10) và cho phép áp dụng nguyên tử (AC-3.1).

### 7.3 Phía nhận `SYNC_REQ`
Với mỗi owner `X` mà bên trả lời **biết** (kể cả owner qua relay và chính bên trả lời):

| Tình huống | Trả lời |
|---|---|
| `X` không có trong `owners` của request | Snapshot (coi như `since_seq=0`) |
| `epoch` khác bản mình có | Snapshot |
| `since_seq < last_seq` và op log còn đủ | Delta `(since_seq, last_seq]` (nén); nếu sau nén > 1000 op → snapshot |
| `since_seq < last_seq` nhưng op log không đủ | Snapshot |
| `since_seq == last_seq` | Không gửi gì |
| `since_seq > last_seq` cùng epoch | Không gửi gì (bên hỏi đi trước mình; đợi `SYNC_REQ` ngược lại) — **trừ khi** `X` là chính bên trả lời, xem §7.5 |

Cuối cùng gửi `SYNC_END{id}`. Bên trả lời **PHẢI** gửi `SYNC_DATA` cho mọi owner thuộc diện cần gửi trước `SYNC_END`; có thể xen kẽ với `UPDATE`/`PING`.

Trong lúc nhận snapshot nhiều chunk cho owner `X`, các `UPDATE` của `X` đến sau được **đệm**; áp dụng theo thứ tự `seq` ngay sau khi chunk `last=true` được áp dụng.

### 7.4 Áp dụng và relay `UPDATE`
Khi nhận `UPDATE{owner, epoch, seq, ops}` từ phiên bất kỳ (kể cả relay):

1. `owner` = node_id của chính mình → xem §7.5 (không áp dụng).
2. Chưa biết `owner` hoặc `epoch` khác bản đã biết → gửi `SYNC_REQ` cho owner đó (nhận snapshot); **không** áp dụng `UPDATE` này.
3. `seq <= last_seq` → trùng/cũ, **bỏ** (dedupe theo `(owner, epoch, seq)`), không relay.
4. `seq == last_seq + 1` → hợp lệ: kiểm tra op (§10), áp dụng **nguyên tử** cả gói, đặt `last_seq = seq`, phát `RecordPut/Del{owner}`, xóa cache liên quan (SPEC §6.3.5), lưu `peer_records`, rồi **relay** nguyên văn `UPDATE` cho mọi peer `ESTABLISHED` **trừ nơi gửi** và trừ chính `owner`.
5. `seq > last_seq + 1` → có lỗ hổng: gửi `SYNC_REQ{owner, epoch, since_seq=last_seq}` thay vì áp dụng bừa; có thể đệm `UPDATE` này tối đa 30 s hoặc 100 gói để áp dụng sau khi lấp lỗ.

Nhận delta/snapshot cho `X` từ ai cũng được; áp dụng xong thì relay theo cùng nguyên tắc: gửi lại (nguyên văn) cho các peer có `last_seq` đã biết của `X` thấp hơn `to_seq`; nếu không biết thì **không** relay (họ sẽ tự `SYNC_REQ`).

> Chỉ relay sau khi đã áp dụng thành công. Op sai định dạng **không bao giờ** được relay.

### 7.5 Epoch và tự vệ của owner
- Owner mất DB hoặc bị reset → sinh `epoch` mới ngẫu nhiên, `seq = 0`, và ngay sau đó gọi snapshot chủ động tới mọi peer.
- Peer thấy `epoch` mới của `X` → **bỏ toàn bộ** record cũ của `X` rồi nhận snapshot.
- **Owner mất dữ liệu cục bộ nhưng peer còn nhớ:** khi bên trả lời nhận `SYNC_REQ`/`UPDATE`/`PING` nói về *chính nó* với `epoch` bằng epoch hiện tại và `seq` **lớn hơn** `seq` của nó, hoặc `epoch` khác — coi là dấu hiệu nó đã mất dữ liệu (hoặc có node giả mạo). Nó **KHÔNG ĐƯỢC** áp dụng; ghi log mức warn + sự kiện `Conflict`, và NÊN sinh `epoch` mới nếu DB cục bộ thực sự đã đặt lại.

### 7.6 Peer offline và hội tụ
- Phiên đóng/heartbeat chết → `status=offline`; dữ liệu giữ đến `peers.offline_retention` (mặc định 24 h) hoặc `/peer forget` (SPEC §5.3, §7.5).
- Nối lại cùng `epoch` → `SYNC_REQ` delta; khác → snapshot.
- Hội tụ sau chia mạng (AC-2.7) dựa vào: (a) `SYNC_REQ` đầy đủ mọi owner ngay khi `ESTABLISHED` (§5.5), (b) `seq` trong `PING`/beacon (§6.1, §3.4), (c) phát hiện lỗ hổng khi có `UPDATE` kế tiếp (§7.4).
- Trong **mạng lưới đầy đủ** (full mesh, D7) owner nối trực tiếp với mọi node, nên relay chủ yếu để chịu được mạng chia đoạn/nối chưa xong.

---

## 8. `RESOLVE` — hỏi thẳng LocalZone của peer

Bước 2d của SPEC §6.2.

### 8.1 Người hỏi
- Chỉ gửi qua phiên `ESTABLISHED` trực tiếp.
- Chọn `id` duy nhất trong phiên; có thể nhiều `RESOLVE` chạy song song (hedging, SPEC §6.3.2).
- "Hủy" ứng viên thua cuộc = **không chờ nữa và bỏ phản hồi đến muộn** (không có thông điệp hủy trên dây).
- Thời gian chờ mỗi ứng viên do pipeline quyết định (`hedge_ms`, `budget_ms`); không có timeout riêng trong giao thức.

### 8.2 Người trả lời
1. **Chỉ tra LocalZone của chính mình.** KHÔNG ĐƯỢC tra registry, cache hay upstream (chống vòng lặp, SPEC §6.3.3).
2. Tra khớp chính xác trước, wildcard sau (SPEC §6.2.2a).
3. Nếu có RRset đúng `type` → `rcode=0`, `answers=[RRset]`.
4. Nếu tên tồn tại chỉ dưới dạng `CNAME` → trả `CNAME` đó (người hỏi tự theo chuỗi, mỗi mắt xích đi lại cả pipeline, SPEC §6.3.4).
5. Tên có nhưng khác type (và không có CNAME) → `rcode=0`, `answers=[]` (NODATA).
6. Tên không có → `rcode=3`.
7. Quá tải/lỗi → `rcode=2`; vượt giới hạn tốc độ → `rcode=5`.

### 8.3 Cách người hỏi hiểu kết quả
| Kết quả | Hành động |
|---|---|
| `rcode=0` và `answers` khác rỗng | **Thành công**: trả client (AA=0, `ttl = min(ttl, 30s)`), dừng bước 2 |
| `rcode=0`, `answers=[]` (NODATA) hoặc `rcode=3` | Thất bại của ứng viên → peer kế trong nhóm → nhóm kế. Nếu `seq`/`epoch` trong phản hồi mới hơn registry → gửi `SYNC_REQ` (registry đang cũ) |
| `rcode=2`/`5`, đóng kết nối, hết thời gian | Thất bại của ứng viên → như trên |
| Không còn ứng viên online | Bước 2e (`serve_stale`) rồi bước 3 (upstream) |

Thứ tự duyệt: nhóm theo xếp hạng §8.4; trong nhóm theo **RTT** thấp trước (RTT từ `PING/PONG`, §6.1).

### 8.4 Xếp hạng xác định (để mọi node cho cùng kết quả)
Với một `(qname, qtype)`:
1. Mỗi owner **online** (có phiên `ESTABLISHED` trực tiếp) cho ra tối đa một RRset (§8.2 áp dụng trên bản registry của owner đó); owner không có câu trả lời thì không tham gia.
2. Gom theo **khóa nhóm** (§2.4). `phiếu` = số owner trong nhóm.
3. Sắp xếp nhóm theo: `phiếu` giảm dần → `claim cũ nhất` tăng dần (nhỏ nhất của `created` trong nhóm) → `node_id` nhỏ nhất trong nhóm tăng dần (D10).
4. Nếu `qname` thuộc LocalZone của chính node → trả luôn, **không** dùng bước này (D11).

Đây phải là một **hàm thuần**: cùng registry ⇒ cùng thứ tự trên mọi node (PLAN §4: test thuộc tính).

---

## 9. Vòng đời kết nối

### 9.1 Ai dial, tránh kết nối đôi
- Nhận beacon/`PEERS`/`manual_peers` của node `X` chưa có phiên:
  - `node_id(mình) < node_id(X)` → **mình dial** `X`.
  - Ngược lại → **chờ** `X` dial mình. Nếu sau 5 s (hoặc 3 beacon) vẫn không thấy kết nối đến → NÊN dial chủ động (phòng khi `X` không nghe được beacon của mình; AP isolation một chiều).
- Nếu vẫn xuất hiện 2 phiên với cùng `node`: **giữ phiên do node_id nhỏ hơn dial**, đóng phiên kia bằng `ERROR DUPLICATE` (§5.2).
- Chỉ dial tới địa chỉ thuộc subnet mà mình có interface hoặc đã được cấu hình tay/`PEERS`.

### 9.2 Nối lại và backoff
- Dial lỗi → thử lại theo **exponential backoff 1 s → 2 → 4 → … → 60 s** (± 20% jitter), tách riêng theo `node_id` (hoặc theo địa chỉ khi chưa biết id).
- Peer trong `manual_peers` được thử lại **vô hạn**, lưu bền; peer học qua beacon/`PEERS` được thử đến khi quá `offline_retention` mà không thấy.
- Nhận beacon mới từ node đang chờ backoff → **đặt lại backoff** và thử ngay.

### 9.3 Heartbeat
- Mỗi **5 s** gửi `PING` trên mọi phiên (không phụ thuộc phiên rảnh hay bận).
- **Bất kỳ frame nào** nhận được đều làm mới đồng hồ phiên. 3 chu kỳ (**15 s**) không nhận được gì → đóng phiên, `offline`, vào backoff (AC-2.4).
- Đóng TCP đột ngột (RST/FIN) → `offline` ngay, không chờ 15 s.
- Mỗi `PING` nhận được **PHẢI** được trả `PONG` ngay.

### 9.4 Đổi IP
Theo dõi sự kiện đổi địa chỉ (SPEC §7.4.6). Khi tập IP của node đổi:
1. Tính lại record `self` → một giao dịch → `UPDATE` (§7.2).
2. Gửi beacon mới ngay (§3.3).
3. Phiên TCP bị đứt do đổi IP sẽ được dial lại bình thường; các peer thấy `ips` mới từ beacon.
Mục tiêu: lan truyền ≤ 10 s (AC-2.5).

### 9.5 Thoát êm
Gửi `BYE` trên mọi phiên → gửi beacon `bye` → đóng. Thoát đột ngột chỉ dựa vào §9.3.

---

## 10. Giới hạn và xử lý vi phạm

| Mục | Giới hạn mặc định | Khi vượt |
|---|---|---|
| Kích thước frame | ≤ 4 MiB | `ERROR MALFORMED`, đóng |
| Op / `UPDATE` hoặc chunk | ≤ 1000 | Bỏ gói, tính 1 vi phạm |
| Giá trị / RRset | ≤ 64 | Op bị từ chối |
| Số peer đồng thời | ≤ 64 | `ERROR TOO_MANY_PEERS` |
| Tốc độ `UPDATE` / peer | 200 op/s (token bucket, burst 2000 op) | Bỏ phần vượt, tính vi phạm; phía nhận NÊN `SYNC_REQ` bù sau khi dịu |
| `RESOLVE` / peer | NÊN ≤ 1000 qps | `RESOLVE_R rcode=5` |
| Thời gian bắt tay | 5 s | Đóng |
| Beacon từ một nguồn | ≤ 20 gói/s | Bỏ phần vượt |

- **Snapshot** (`SYNC_DATA` snapshot, kể cả chủ động) **không** tính vào giới hạn tốc độ `UPDATE`, nhưng vẫn chịu giới hạn frame/chunk và NÊN giới hạn tổng ≤ 1 snapshot đầy đủ/owner/phút từ mỗi peer.
- **Kiểm tra op trước khi áp dụng:** tên đã chuẩn hóa và hợp lệ (SPEC §5.1); `type` hợp lệ; `data` đúng kiểu/độ dài theo §2.3; `ttl` trong khoảng; wildcard chỉ ở nhãn trái cùng; `del` không cần `data`. Gói có **một** op sai → từ chối **cả gói** (nguyên tử), không relay.
- **Vi phạm lặp lại:** ≥ 3 vi phạm trong 60 s → `ERROR RATE` hoặc `MALFORMED`, đóng phiên và đưa peer vào backoff dài (60 s) (SPEC §14).

---

## 11. Bảo mật

| Mối đe dọa | Có PSK | Không PSK |
|---|---|---|
| Người ngoài mạng Ldns nghe/ghi chèn | Chặn: AEAD toàn phiên, HMAC beacon | **Không chặn** — `/status` luôn cảnh báo (D16) |
| Replay/đảo thứ tự frame | Chặn (nonce theo bộ đếm) | Không |
| Thành viên mạng khai gian zone của mình | **Không chặn** (bỏ phiếu ≠ bảo mật) | Không |
| Thành viên mạng giả mạo owner khác | **Không chặn** — v1 chỉ tin cậy mức mạng; chữ ký khóa công khai ở v1.1 | Không |
| Tấn công Sybil vào bỏ phiếu | **Không chặn** | Không |
| Từ chối dịch vụ (flood) | Giảm nhẹ: §10 | Giảm nhẹ: §10 |

Ghi chú vận hành:
- PSK **không bao giờ** xuất hiện trong log, `ERROR.msg`, hay `/status`.
- Sai khóa không được phân biệt với "node lạ" ngoài `ERROR AUTH` chung chung.
- Chế độ hỗn hợp (một node có khóa, một node không) bị từ chối ở HELLO (`psk` khác nhau).
- So sánh `mac` dùng so sánh hằng thời gian.

---

## 12. Giá trị thời gian mặc định (tóm tắt, SPEC Phụ lục C)

| Mục | Giá trị |
|---|---|
| Beacon | mỗi 2 s trong 10 s đầu → mỗi 10 s ± 20% |
| Heartbeat | 5 s; chết sau 15 s (3 lần) |
| Hạn bắt tay | 5 s |
| Backoff dial | 1 s → 60 s (×2, ± 20%) |
| Hedge | 250 ms |
| Ngân sách bước 2 | 1500 ms |
| Trần TTL từ peer | 30 s |
| `serve_stale` TTL | 10 s |
| Giữ owner offline | 24 h |
| Op log owner | ≥ 24 h hoặc 10 000 `seq` |
| Đệm `UPDATE` có lỗ hổng | 30 s / 100 gói |

---

## 13. Kịch bản (trình tự)

### 13.1 Hai máy mở Ldns, không cấu hình (AC-2.1, 2.2)
```
A (id nhỏ)                                   B (id lớn)
 │── beacon (multicast+broadcast) ────────────►│
 │◄──────────────────────────── beacon ────────│  B thấy A: id(B) > id(A) → chờ A dial
 │── TCP connect ─────────────────────────────►│
 │── HELLO ───────────────────────────────────►│
 │◄─────────────────────────────── HELLO ───────│
 │── SYNC_REQ{owners:[]} + PEERS ─────────────►│
 │◄────────────── SYNC_REQ{owners:[]} + PEERS ──│
 │◄── SYNC_DATA(snapshot B) ... SYNC_END ───────│
 │── SYNC_DATA(snapshot A) ... SYNC_END ───────►│        ≤ 5 s kể từ lúc mở
 ⋮
 │  user: /add app.lan   (A, 1 giao dịch, seq 7→8)
 │── UPDATE{owner:A, epoch, seq:8, ops:[put app.lan A]} ─►│   B áp dụng, xóa cache, hiện sự kiện
                                                              < 500 ms
```

### 13.2 Máy C (không sở hữu) hỏi `app.lan`
```
C: client hỏi app.lan
   1. LocalZone: không có
   2. Registry: nhóm {A} (1 phiếu)
      C ── RESOLVE{id:41, name:app.lan, type:A} ──► A
      C ◄─ RESOLVE_R{id:41, rcode:0, answers:[...]} ── A     → trả client, AA=0, TTL ≤ 30 s
```

### 13.3 Ứng viên đầu tắt đột ngột (AC-1.4)
```
C ── RESOLVE{41} ──► A (vừa chết, chưa phát hiện)
   ... 250 ms (hedge) ...
C ── RESOLVE{42} ──► E (cùng nhóm, phiếu bằng)      ← chạy song song
C ◄─ RESOLVE_R{42, rcode:0, ...} ── E                → thắng; bỏ phản hồi muộn của A
```

### 13.4 Chia mạng rồi nối lại (AC-2.7)
```
Đứt:   hai phía vẫn phục vụ từ registry của mình; owner phía kia → offline sau ≤ 15 s
Nối:   dial lại (backoff) → HELLO → mỗi bên gửi SYNC_REQ đầy đủ mọi owner
       → delta (hoặc snapshot nếu epoch khác/log không đủ) → hội tụ, không mất domain của owner nào
```

---

## 14. Ví dụ gói tin (ghi theo JSON; `bin` hiển thị hex)

**Beacon** (body của datagram `LDB1`):
```json
{
  "t": "beacon", "v": [1,0],
  "node": "0xb3a1c9d2e4f50716283940a5b6c7d8e9",
  "name": "may-an", "net": "0x5f2e8a1c3b4d6e70",
  "sync_port": 5391, "dns_port": 53,
  "ips": ["0xc0a8010a"],
  "epoch": 7345921807, "seq": 8, "ts": 117253981225984
}
```

**HELLO:**
```json
{
  "t": "HELLO", "v": [1,0],
  "node": "0xb3a1c9d2e4f50716283940a5b6c7d8e9", "name": "may-an",
  "net": "0x5f2e8a1c3b4d6e70", "epoch": 7345921807, "seq": 8,
  "ips": ["0xc0a8010a"], "sync_port": 5391, "dns_port": 53,
  "caps": ["relay","snapshot-chunk"],
  "nonce": "0x7f...(32 byte)", "psk": false
}
```

**UPDATE** (thêm `app.lan` trỏ `192.168.1.10`):
```json
{
  "t": "UPDATE",
  "owner": "0xb3a1c9d2e4f50716283940a5b6c7d8e9",
  "epoch": 7345921807, "seq": 8,
  "ops": [
    { "op": "put", "name": "app.lan", "type": "A", "ttl": 60,
      "data": ["0xc0a8010a"],
      "created": 117253981225984, "ts": 117253981225984 }
  ]
}
```

**RESOLVE / RESOLVE_R:**
```json
{ "t": "RESOLVE", "id": 41, "name": "app.lan", "type": "A" }
{ "t": "RESOLVE_R", "id": 41, "rcode": 0, "epoch": 7345921807, "seq": 8,
  "answers": [ { "name": "app.lan", "type": "A", "ttl": 60, "data": ["0xc0a8010a"] } ] }
```

> Khi triển khai, thêm bộ **golden test**: các thông điệp trên được mã hóa MessagePack cố định (file hex trong `tests/unit/protocol/`), cả hai HĐH phải cho đúng byte.

---

## 15. Phiên bản và khả năng mở rộng

- `v = [major, minor]`. Khác **major** → từ chối (`ERROR VERSION`). Khác **minor** → vẫn kết nối; mỗi bên chỉ dùng tính năng có trong `caps` của cả hai.
- Thay đổi tương thích ngược (thêm khóa tùy chọn, thêm loại thông điệp mới, thêm `type` DNS mới) → tăng **minor**.
- Bên nhận **bỏ qua khóa lạ** và **bỏ qua loại thông điệp lạ** sau `ESTABLISHED`.
- Type DNS mới (MX, SRV, PTR — v1.1): op có type lạ vẫn bị từ chối bởi node cũ; node mới chỉ gửi type mới khi đối tác công bố `caps` tương ứng (ví dụ `"rr-mx"`), nếu không thì lọc ra.
- `caps` v1.0: `relay`, `snapshot-chunk`.

---

## 16. Các điểm bổ sung so với SPEC v0.1 (cần đưa ngược vào SPEC)

| # | Điểm | Lý do |
|---|---|---|
| P1 | Op `put` có thêm `created` | D10 cần "claim cũ nhất" xác định giống nhau trên mọi node; SPEC chỉ liệt kê `ts` |
| P2 | Thông điệp `ERROR` và mã lỗi 1–8 | Báo lý do từ chối (SPEC chỉ nói "từ chối") |
| P3 | Thông điệp `SYNC_END` và trường `id` trong `SYNC_REQ`/`SYNC_DATA` | Biết khi nào đồng bộ xong (đo < 2 s, hiển thị UI) |
| P4 | `PING`/`PONG` mang `epoch`+`seq` của người gửi | Vá sót rẻ tiền; cho đo RTT bằng `n` |
| P5 | `SYNC_REQ.owners` là **mảng** `{owner, epoch, since_seq}` thay vì map | `nlohmann::json` không hỗ trợ khóa nhị phân trong map MessagePack |
| P6 | Khung beacon `LDB1` + `flags` + `mac` 16 byte | SPEC chỉ nói "có HMAC nếu bật PSK" |
| P7 | Chi tiết KDF/AEAD: HKDF-SHA256, nonce = bộ đếm ngầm, transcript = byte HELLO | SPEC chỉ nêu thuật toán |
| P8 | Một giao dịch = một `seq`; giao dịch > 1000 op gửi bằng **snapshot chủ động** | **Mâu thuẫn trong SPEC:** §14 giới hạn 200 op/s/peer, trong khi AC-3.1 yêu cầu import 100 000 domain đồng bộ thành công (200 op/s sẽ mất ~500 s). Đề xuất: snapshot không bị tính vào giới hạn đó |
| P9 | Giới hạn 64 giá trị/RRset, TXT ≤ 1024 byte/phần tử | SPEC chưa chặn |
| P10 | Op log owner giữ ≥ 24 h hoặc 10 000 `seq` | SPEC nhắc "không còn log" nhưng chưa định lượng |
| P11 | "Online" ⇔ có phiên `ESTABLISHED` trực tiếp (bỏ phiếu và `RESOLVE`) | Khớp AC-2.4 (≤ 15 s); owner chỉ biết qua relay vẫn xuất hiện trong registry nhưng không bỏ phiếu |
| P12 | Quy tắc tự vệ owner khi peer nói `seq` lớn hơn của chính mình (§7.5) | SPEC §7.4.4 chỉ mô tả epoch lúc reset |

---

## 17. Danh mục kiểm tra tuân thủ (liên kết AC trong SPEC §17.2)

| Hạng mục | Kiểm thử | AC |
|---|---|---|
| Beacon gửi đúng lịch, `bye`, bỏ gói sai magic/net/MAC | Unit + fuzz parser beacon | 2.1 |
| Handshake: sai `v`/`net`/PSK → đúng `ERROR`; thành công → `ESTABLISHED` | Unit state machine | 2.1, 2.6 |
| Quy tắc dial `node_id` nhỏ + khử kết nối đôi | Integration nhiều tiến trình `127.0.0.x` | 2.1 |
| `UPDATE` áp dụng + relay + dedupe + phát hiện lỗ hổng | Sim N node, mất gói | 2.2, 2.3, 2.7 |
| Delta, snapshot nhiều chunk, đệm `UPDATE` khi đang nhận snapshot | Sim | 2.7, 3.1 |
| Epoch mới → bỏ dữ liệu cũ; owner mất DB | Sim | 2.4, 2.7 |
| Heartbeat 5 s / 15 s; `RST` → offline ngay | Integration (`tc netem`) | 2.4 |
| Đổi IP → `UPDATE` + beacon ≤ 10 s | Integration | 2.5 |
| `/peer add` + `PEERS` khi multicast chặn | Integration (netns) | 2.6 |
| `RESOLVE` chỉ tra LocalZone; duyệt theo xếp hạng; hedging | Integration | 1.2–1.4, 1.7 |
| Xếp hạng là hàm thuần (cùng registry ⇒ cùng thứ tự) | Property test | 1.3 |
| Parser frame/op không crash với dữ liệu hỏng | Fuzz (libFuzzer/AFL++) | — |
| PSK: AEAD, replay/đảo thứ tự bị đóng; log không lộ khóa | Unit + soát log | M9 |
| Import 100 000 domain qua snapshot chủ động | Integration + đo RAM | 3.1 |
