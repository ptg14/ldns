# Ldns — Kế hoạch triển khai (PLAN) v0.1

Tham chiếu: `SPEC.md`. Ước lượng cho **1 lập trình viên**, làm việc đều; có 2 người thì M7 (TUI) chạy song song với M4–M6.

---

## 0. Neo mục đích (làm lại sau MỖI milestone)

Trước khi đóng milestone, chạy tay kịch bản này và đánh dấu. Nếu có mục nào lệch, sửa trước khi sang milestone kế.

- [ ] **R1** Hỏi một tên: local → peer (bản nhiều phiếu nhất, thất bại thì theo xếp hạng) → upstream. Mọi thứ đã làm có đang phục vụ luồng này không?
- [ ] **R2** Hai máy mở Ldns, không cấu hình, tự thấy nhau và thay đổi domain hiện sang máy kia gần như tức thì.
- [ ] **R3** Tạo domain không giới hạn, trỏ về `self` hoặc địa chỉ tự cấu hình.
- [ ] Vẫn là CLI C++ chạy được trên **Windows và Linux**, UX hướng tới Claude Code.

Nguyên tắc làm việc:
1. **Lát cắt dọc trước:** sớm có bản chạy end-to-end (2 node, 1 domain) rồi mới đào sâu.
2. Mỗi milestone kết thúc bằng thứ **chạy được và kiểm thử được** trên cả hai HĐH (CI).
3. Core không phụ thuộc UI; UI chỉ gọi `CoreApi`.
4. Rủi ro lớn nhất (TUI inline, port 53, mạng/multicast Windows) được **spike sớm**.

---

## 1. Tổng quan lộ trình

| M | Tên | Kết quả chính | Ước lượng |
|---|---|---|---|
| M0 | Nền tảng repo & CI | Build/test xanh trên Win+Linux | 3 ngày |
| M1 | DNS codec + server + LocalZone | Server trả lời domain cục bộ (bước 1) | 1,5 tuần |
| M2 | Store + domain CRUD + CLI một lần | `ldns add/ls/rm/resolve` hoạt động | 1 tuần |
| M3 | Upstream + pipeline + cache (bước 1+3) | Dùng được như DNS cục bộ thay thế | 1 tuần |
| M4 | Discovery + kết nối peer | 2 máy tự thấy nhau, heartbeat | 1,5 tuần |
| M5 | Đồng bộ Registry real-time | Domain lan giữa các máy < 500 ms | 2 tuần |
| M6 | PeerResolver (bỏ phiếu + xếp hạng + hedge) | **R1 đầy đủ** | 1,5 tuần |
| M7 | TUI kiểu Claude Code | Giao diện hoàn chỉnh | 3 tuần (song song được) |
| M8 | Daemon, IPC, tích hợp HĐH | Service, `/system`, `/doctor`, firewall | 2 tuần |
| M9 | Bảo mật & độ cứng | PSK, ACL, rate limit, fuzz, sim | 1,5 tuần |
| M10 | Đóng gói, tài liệu, phát hành | v1.0 | 1 tuần |

Tổng: ~15–17 tuần (1 người); ~11–12 tuần (2 người).

Spike song song ngay từ tuần 1: **S1** inline-renderer terminal (1–2 ngày), **S2** bind port 53 + multicast trên Windows và Linux (1 ngày).

```
M0 ─► M1 ─► M2 ─► M3 ─► M4 ─► M5 ─► M6 ─┐
 │                                      ├─► M8 ─► M9 ─► M10
 └─► S1 ─► M7 (cần M2 để có lệnh thật) ──┘
```

---

## 2. Spike (rủi ro cao, làm trước)

### S1 — Renderer inline kiểu Claude Code (1–2 ngày)
Mục tiêu: chứng minh vẽ được "vùng động" cố định ở đáy + in dòng bất biến lên scrollback, trên Windows Terminal và gnome-terminal.
- [ ] Bật raw mode Linux (termios) và Windows (`SetConsoleMode` + `ENABLE_VIRTUAL_TERMINAL_*`, UTF-8).
- [ ] Đọc phím (VT input) thành sự kiện thống nhất: ký tự, mũi tên, Home/End, Ctrl/Alt, Shift+Enter, bracketed paste, resize.
- [ ] Vẽ vùng động N dòng: di chuyển con trỏ lên, xóa, vẽ lại (diff theo dòng); in dòng mới phía trên vùng động.
- [ ] Đo độ rộng ký tự (tiếng Việt có dấu, CJK, emoji).
- **Tiêu chí:** gõ vào ô nhập có viền bo, spinner chạy, mỗi 1 s in một dòng sự kiện phía trên mà không chớp/vỡ bố cục; đổi kích thước cửa sổ không hỏng.
- **Phương án dự phòng nếu thất bại:** dùng FTXUI (chế độ non-fullscreen) và chấp nhận khác biệt nhỏ về scrollback.

### S2 — Mạng nền (1 ngày)
- [ ] Bind `127.0.0.1:53` + IP LAN trên Ubuntu có `systemd-resolved`, và Windows 11 (kiểm tra ICS/Hyper-V/WSL).
- [ ] Gửi/nhận multicast `239.76.68.83:5390` theo từng interface (Linux và Windows, Wi-Fi và Ethernet).
- [ ] Nhận sự kiện đổi IP (netlink / `NotifyIpInterfaceChange`).
- **Tiêu chí:** ghi lại kết quả vào `docs/platform-notes.md`; điều chỉnh SPEC §13 nếu phát hiện khác biệt.

---

## 3. Milestone chi tiết

### M0 — Nền tảng repo & CI (3 ngày)
**Task**
- [ ] Khởi tạo repo theo cấu trúc SPEC Phụ lục A; `CMakeLists.txt`, `CMakePresets.json` (linux-debug/release, windows-msvc, asan/ubsan).
- [ ] `vcpkg.json` (asio, nlohmann-json, sqlite3, tomlplusplus, libsodium, spdlog, cli11, doctest).
- [ ] Chuẩn C++20; cảnh báo = lỗi (`-Wall -Wextra -Werror`, `/W4 /WX`); `.clang-format`, `.clang-tidy`.
- [ ] CI GitHub Actions: ubuntu-latest + windows-latest; build, test, sanitizer (Linux), cache vcpkg.
- [ ] Khung `util/`: logger, `Result<T,E>` (hoặc `std::expected` nếu có), id, time (HLC), buffer view.
- [ ] Binary mẫu `ldns --version`.
**Nghiệm thu:** CI xanh cả hai HĐH; `ldns --version` chạy được.

### M1 — DNS codec + server + LocalZone (1,5 tuần)
**Task**
- [ ] `dns::Name`: parse/encode, chuẩn hóa, nén con trỏ (đọc có giới hạn độ sâu, ghi có nén), IDN → punycode.
- [ ] `dns::Message`: header, question, RR (A, AAAA, CNAME, TXT, NS, SOA, opaque), EDNS0 OPT.
- [ ] Unit test (golden packets từ `dig`/RFC) + harness fuzz parser ngay từ đây.
- [ ] `LocalZone` trong bộ nhớ: tra chính xác → wildcard; NODATA vs NXDOMAIN; CNAME nội bộ (tối đa 8).
- [ ] `DnsServer`: UDP + TCP (framing 2 byte), cắt TC khi vượt kích thước, per-interface socket, ACL private, rate limit token-bucket.
- [ ] Công cụ test `tests/tools/dnsq` (hoặc dùng `dig`/`nslookup` trong integration).
**Nghiệm thu:** nạp zone giả lập, `dig @127.0.0.1 -p 5300 app.lan` trả đúng; gói hỏng 1 triệu mẫu fuzz không crash; ≥ 10 000 qps cục bộ.

### M2 — Store + domain CRUD + CLI một lần (1 tuần)
**Task**
- [ ] `Store` SQLite (WAL), hệ thống migration, bảng `meta`, `my_records`, `upstreams`.
- [ ] Sinh/lưu bền `node_id`, `epoch`, `seq_self`.
- [ ] `DomainService`: add/set/rm/list, kiểm tra tên hợp lệ, chính sách TLD công cộng (danh sách nhúng sẵn), RRset nhiều giá trị, wildcard, `self` vs static.
- [ ] `SelfAddress`: chọn IP LAN (interface có route mặc định, loại ảo/loopback/link-local), ghim bằng cấu hình.
- [ ] `Config` TOML + biến môi trường + tham số CLI.
- [ ] Khai báo lệnh dùng chung (`ui/commands`): `add`, `set`, `rm`, `ls`, `resolve`, `config`, `version` — chạy chế độ nhúng (core trong tiến trình).
- [ ] Hỗ trợ `--json`, mã thoát chuẩn (SPEC §10.1).
- [ ] `import/export` (TOML/CSV).
**Nghiệm thu:** `ldns add app.lan` rồi `ldns daemon --foreground` + `dig` trả IP LAN; khởi động lại vẫn giữ domain; import 100 000 dòng < 5 s.

### M3 — Upstream + pipeline + cache (bước 1 + 3) (1 tuần)
**Task**
- [ ] `Upstream`: UDP trước, TCP khi TC; timeout, retry, chọn tuần tự; loại trừ địa chỉ của chính mình (chống vòng lặp).
- [ ] Phát hiện DNS hệ thống hiện tại làm mặc định `auto` (đọc `/etc/resolv.conf`/`resolvectl`/registry-API Windows), lưu bản sao để `restore`.
- [ ] `Resolver` pipeline có điểm cắm cho bước 2 (rỗng tạm thời), `--trace`.
- [ ] `Cache` LRU, TTL cap, negative cache, invalid hóa theo sự kiện.
- [ ] `QueryLog` (bảng vòng) và sự kiện.
**Nghiệm thu:** đặt DNS máy = 127.0.0.1 thủ công, duyệt web bình thường + domain cục bộ hoạt động; `--trace` hiển thị bước L/U; AC-1.1, AC-1.5.

### M4 — Discovery + kết nối peer (1,5 tuần)
**Task**
- [ ] `Discovery`: beacon multicast theo từng interface hợp lệ + broadcast dự phòng; lịch gửi (2s → 10s ±20% jitter); `bye`.
- [ ] `PeerManager`: bảng peer, quy tắc dial (node_id nhỏ dial), khử kết nối đôi, backoff, `manual_peers`.
- [ ] `Session`: framing (tiền tố độ dài), MessagePack, handshake `HELLO`, kiểm tra `net`/phiên bản.
- [ ] Heartbeat PING/PONG, phát hiện offline (15 s), tự nối lại.
- [ ] Peer exchange (`PEERS`).
- [ ] Trình theo dõi đổi IP (netlink / `NotifyIpInterfaceChange`) + dự phòng thăm dò.
- [ ] Sự kiện `PeerUp/PeerDown`; lệnh `peers`, `peer add/rm/forget`.
- [ ] Test: nhiều tiến trình trên `127.0.0.x`; sim mất gói.
**Nghiệm thu:** AC-2.1 (thấy nhau ≤ 5 s không cấu hình), AC-2.4 (tắt đột ngột → offline ≤ 15 s), AC-2.6 (multicast bị chặn, `/peer add` vẫn nối).

### M5 — Đồng bộ Registry real-time (2 tuần)
**Task**
- [x] `Registry`: `OwnerState`, áp dụng op, cấu trúc đọc không khóa (snapshot bất biến/RCU) cho hot-path DNS.
- [x] Bảng `peers`, `peer_records` (lưu bền registry để khởi động nhanh).
- [x] Op log của owner (put/del), tăng `seq` trong cùng transaction với thay đổi `my_records`.
- [x] `SYNC_REQ` / `SYNC_DATA` (delta + snapshot chia khúc), `UPDATE` đẩy tức thì.
- [x] Phát hiện lỗ hổng seq, xử lý `epoch`.
- [x] Relay gossip + dedupe `(owner,epoch,seq)`.
- [x] Cập nhật `self` khi đổi IP → phát UPDATE + beacon.
- [x] Giữ owner offline theo `offline_retention`; dọn dẹp.
- [x] Sự kiện `RecordPut/Del{owner}` → TUI/CLI `--follow`; invalid hóa cache.
- [x] Test mô phỏng: N node, mất gói, chia mạng, khởi động lại ngẫu nhiên → **hội tụ**.
**Nghiệm thu:** AC-2.2, 2.3, 2.5, 2.7; node mới đồng bộ 10 000 record < 2 s; benchmark độ trễ lan truyền < 500 ms.

### M6 — PeerResolver: bỏ phiếu + xếp hạng + hedging (1,5 tuần)
**Task**
- [ ] Tính claim của từng owner cho `(qname,qtype)` (chính xác trước, wildcard sau).
- [ ] Gom nhóm bản theo RRset chuẩn hóa (sắp xếp, so sánh TTL-bỏ-qua); chỉ tính owner online.
- [ ] Xếp hạng: phiếu ↓ → claim cũ nhất → node_id nhỏ nhất (D10) — **hàm thuần, test kỹ**.
- [ ] `RESOLVE`/`RESOLVE_R` qua `Session`; peer chỉ trả từ LocalZone (chống vòng lặp).
- [ ] Duyệt: trong nhóm theo RTT; hedging `hedge_ms`; tổng ngân sách `budget_ms`; hủy phần còn lại khi có kết quả.
- [ ] Bước 2e `serve_stale` cho owner offline.
- [ ] Ghi nhận xung đột (`Conflict`) + lệnh `/conflicts`, `/info` (nhóm, phiếu, thứ hạng).
- [ ] Tích hợp vào pipeline; `--trace` hiển thị peer đã thử và kết quả.
- [ ] Test kịch bản SPEC §6.4 và AC-1.2…1.7 (tự động, nhiều tiến trình).
**Nghiệm thu:** toàn bộ AC-1.x đạt. **Đây là mốc "bản chạy đúng mục đích R1+R2".** Demo 3 máy thật.

### M7 — TUI kiểu Claude Code (3 tuần, bắt đầu sau S1, tích hợp lệnh thật sau M2)
**Task**
- [ ] `term/`: raw mode, VT input parser, bracketed paste, resize, độ rộng ký tự (bảng + xử lý tổ hợp), color/NO_COLOR/ASCII fallback.
- [ ] `InlineRenderer`: quản lý vùng động + dòng bất biến vào scrollback; diff khung hình; chống chớp.
- [ ] Widgets: `Panel` (viền bo, welcome box), `InputBox` (nhiều dòng, con trỏ, undo, lịch sử bền, Ctrl+R), `SlashMenu` (lọc, mô tả, điều hướng), `Spinner` (động từ xoay vòng), `Dialog` (danh sách đánh số), `StatusLine`, bảng (peers/domains), khối `⏺` / `⎿`.
- [ ] Hệ lệnh dùng chung: đăng ký lệnh → TUI (slash) và CLI (một lần) cùng nguồn; gợi ý tham số; hoàn thành Tab theo ngữ cảnh (tên domain, tên peer).
- [ ] Luồng sự kiện thời gian thực chèn vào scrollback (peer vào/ra, domain thêm/xóa, xung đột) mà không phá ô nhập.
- [ ] `/logs --follow`, `/status`, `/resolve --trace` hiển thị đẹp (cột L/P/U).
- [ ] Phím tắt đầy đủ (SPEC §10.2); Ctrl+C hai lần; Esc ngắt tác vụ đang chạy.
- [ ] Chủ đề (dark/light/none), i18n vi/en (bảng chuỗi).
- [ ] Windows: bật VT + UTF-8, kiểm tra Windows Terminal và conhost (fallback ASCII).
- [ ] Test snapshot với terminal ảo; kiểm thử thủ công trên gnome-terminal, kitty, tmux, ssh, Windows Terminal.
**Nghiệm thu:** AC-4.1, 4.2, 4.3; quay màn hình demo so sánh cảm giác với Claude Code (bố cục, phím, hộp thoại, spinner).

### M8 — Daemon, IPC, tích hợp HĐH (2 tuần)
**Task**
- [ ] IPC JSON-RPC 2.0: Unix socket (0600) / Named pipe (ACL); phương thức = `CoreApi`; luồng subscribe sự kiện.
- [ ] `ldns daemon` (foreground/service); TUI/CLI tự phát hiện daemon và gắn vào; nếu không → nhúng core.
- [ ] Linux: unit systemd, user `ldns`, capability; `/service install|…`.
- [ ] Windows: dịch vụ SCM; cài/gỡ; khôi phục khi lỗi.
- [ ] `/system setup|restore` cho systemd-resolved, NetworkManager, resolv.conf, Windows (sao lưu/khôi phục chắc chắn, kể cả khi crash).
- [ ] `/doctor`: kiểm tra bind 53, `systemd-resolved`, quyền, firewall, multicast, đổi IP, upstream hoạt động.
- [ ] Firewall: nhận diện và (khi được phép) thêm luật cho UDP/TCP 53, UDP 5390, TCP 5391 theo subnet LAN.
- [ ] Hộp thoại xử lý lỗi bind/quyền với hướng dẫn cụ thể.
**Nghiệm thu:** cài service trên Ubuntu 22.04 và Windows 11 sạch; reboot → DNS tự chạy; `/system restore` trả DNS về như cũ; AC-4.4 (điện thoại dùng DNS là máy Ldns).

### M9 — Bảo mật & độ cứng (1,5 tuần)
**Task**
- [ ] PSK: dẫn xuất khóa, HMAC beacon, handshake xác thực, AEAD phiên (libsodium); `/network key set|clear`.
- [ ] Cảnh báo "chưa đặt khóa mạng" trong `/status`.
- [ ] Giới hạn: số peer, tốc độ UPDATE/peer, kích thước frame, số op/frame; ngắt peer vi phạm.
- [ ] Rà soát ACL DNS, chống khuếch đại, chống vòng lặp upstream.
- [ ] Fuzz liên tục parser DNS + parser frame (corpus trong CI nightly).
- [ ] Sim chaos: chia mạng, trễ, mất gói, khởi động lại ngẫu nhiên, đổi IP; kiểm tra hội tụ.
- [ ] Đo hiệu năng (qps, độ trễ lan truyền, RAM với 100 000 record, 100 peer giả lập).
- [ ] Rà soát bộ nhớ: ASan/UBSan/TSan sạch; kiểm tra rò rỉ chạy 24 h.
- [ ] Soát log để không lộ khóa; quyền tệp cấu hình/khóa.
**Nghiệm thu:** mục §15 SPEC đạt; không còn lỗi mức nghiêm trọng từ sanitizer/fuzz; AC-3.1.

### M10 — Đóng gói, tài liệu, phát hành (1 tuần)
**Task**
- [ ] Binary tĩnh Linux (x86_64, aarch64), Windows x64; `.tar.gz`, `.deb`, `.zip` (+ MSI/WiX nếu kịp).
- [ ] Release workflow (tag → build → ký checksum → GitHub Release).
- [ ] Tài liệu: README (bắt đầu nhanh 5 phút), `docs/protocol.md`, hướng dẫn cài service, FAQ (xung đột port 53, multicast, Wi-Fi isolation), mô hình đe dọa.
- [ ] Kịch bản nghiệm thu đầy đủ trên 3 máy thật (2 Linux + 1 Windows); chạy lại toàn bộ AC-1…4.
- [ ] Gắn tag `v1.0.0`.
**Nghiệm thu:** người mới cài trên 2 máy sạch và dùng được domain chung trong < 5 phút theo README.

---

## 4. Chiến lược kiểm thử (tóm tắt, chi tiết SPEC §17)

| Tầng | Công cụ | Khi nào chạy |
|---|---|---|
| Unit | doctest | mỗi commit |
| Fuzz | libFuzzer/AFL++ | PR (ngắn) + nightly (dài) |
| Mô phỏng hội tụ | harness trong `tests/sim` | mỗi PR liên quan net/registry |
| Integration | nhiều tiến trình + netns/netem (Linux), hai tiến trình (Windows) | mỗi PR |
| TUI snapshot | terminal ảo | mỗi PR liên quan ui |
| Hệ thống thật | 3 máy | cuối M6, M8, M10 |

Quy ước: mọi lỗi sửa xong kèm test tái hiện; hàm xếp hạng/bỏ phiếu phải có **test thuộc tính** (cùng đầu vào ⇒ cùng thứ tự trên mọi node).

---

## 5. Sổ rủi ro

| # | Rủi ro | Xác suất | Ảnh hưởng | Giảm thiểu |
|---|---|---|---|---|
| K1 | Renderer inline tự viết khó chạy ổn trên mọi terminal | Trung bình | Cao (UX là yêu cầu chính) | Spike S1 sớm; phương án dự phòng FTXUI; khóa phạm vi widget |
| K2 | Port 53 bị chiếm / thiếu quyền | Cao | Cao | Bind địa chỉ cụ thể; `/doctor`; hộp thoại hướng dẫn; capability/service |
| K3 | Multicast bị chặn (Wi-Fi, VLAN, firewall) | Cao | Trung bình | Broadcast dự phòng, `/peer add`, peer exchange |
| K4 | Firewall Windows chặn kết nối peer | Cao | Trung bình | `/doctor` + tự thêm luật khi được phép |
| K5 | Mạng mở: node xấu giả mạo domain | Trung bình | Trung bình | PSK, cảnh báo, ghi rõ trong tài liệu; chữ ký khóa công khai ở v1.1 |
| K6 | Bỏ phiếu bị lệch (nhiều node cùng nói dối) | Thấp | Thấp–TB | Đúng đặc tả người dùng; local luôn thắng (D11); hiển thị `/conflicts` |
| K7 | Đặt DNS hệ thống làm mất DNS khi Ldns chết | Trung bình | Cao | Sao lưu/khôi phục chắc chắn; chế độ an toàn `[127.0.0.1, gốc]`; service tự khởi động lại |
| K8 | Sai lệch đồng hồ ảnh hưởng tie-break | Thấp | Thấp | HLC; còn hòa thì node_id; ghi nhận trong tài liệu |
| K9 | Bộ phân tích DNS có lỗ hổng | Trung bình | Cao | Fuzz từ M1, sanitizer, giới hạn cứng độ sâu/độ dài |
| K10 | Khác biệt API mạng Windows/Linux (đa interface, IP đổi) | Trung bình | Trung bình | Spike S2; lớp `os/` tách riêng; CI cả hai HĐH |
| K11 | Phạm vi trượt (tính năng thêm) | Cao | Trung bình | Bám §0; mọi mục ngoài R1–R3/UX vào backlog v1.1 |

---

## 6. Quy ước kỹ thuật

- C++20, không ngoại lệ qua ranh giới module (dùng `Result`); RAII, không `new` thô; `string_view`/`span` ở giao diện.
- Đa luồng: một `io_context` cho mạng; thread pool nhỏ cho SQLite; truyền trạng thái bằng `shared_ptr<const Snapshot>`; cấm giữ mutex qua `await/handler`.
- Nhánh: `main` luôn xanh; PR nhỏ, một mối quan tâm; commit theo Conventional Commits.
- Mỗi milestone có tag `m<N>-done` và bản ghi demo ngắn.
- Tài liệu cập nhật cùng PR thay đổi hành vi (SPEC là nguồn sự thật; lệch phải sửa SPEC).

---

## 7. Định nghĩa "Hoàn thành" (Definition of Done)

**Một tính năng xong khi:** có test tự động · chạy trên Windows và Linux (CI) · không cảnh báo/sanitizer · có thông báo lỗi dễ hiểu · có lệnh CLI + slash tương ứng (nếu là hành động người dùng) · SPEC/README đã cập nhật.

**v1.0 xong khi:** toàn bộ AC-1.x, 2.x, 3.x, 4.x trong SPEC §17.2 đạt trên 3 máy thật · mục §15 đạt · chạy ổn định 24 h với tải giả lập · README đủ để người mới dùng trong < 5 phút.

---

## 8. Backlog sau v1.0

Tên `<node>.ldns` tự sinh · PTR/reverse · MX/SRV · DoH/DoT cho upstream · xác thực owner bằng chữ ký khóa công khai (chống giả mạo thật sự) · macOS · gói winget/apt/brew · giao diện web quản trị nhẹ · tính năng "nhóm/namespace" để tách domain theo phòng ban · xuất metrics Prometheus.

---

## 9. Bước làm ngay (tuần đầu)

1. M0: tạo repo, CMake + vcpkg + CI hai HĐH.
2. S1 và S2 (spike) — ghi kết quả vào `docs/platform-notes.md`.
3. Bắt đầu M1: `dns::Name` + `dns::Message` kèm golden test và fuzz harness.
4. Mỗi cuối tuần: chạy "Neo mục đích" (§0), cập nhật bảng tiến độ trong README.
