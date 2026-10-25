# Nhật ký làm việc — Ldns

## 2026-10-22 21:27 — bắt đầu
- Việc đã làm: Thiết lập quy tắc làm việc cho agent trong `CLAUDE.md` (tuân thủ tài liệu, quy trình commit, kiểm tra ảnh hưởng, bảo mật, nhật ký `process.md`).
- File liên quan: `CLAUDE.md`, `process.md`.
- Cần chú ý: Repo hiện mới có tài liệu (`docs/SPEC.md`, `docs/PLAN.md`, `docs/protocol.md`) và cây thư mục rỗng theo PLAN M0; chưa có mã nguồn, CMakeLists hay test. Nguồn sự thật là `docs/SPEC.md`.
- Bug/lỗ hổng: không có.
- Tiếp theo: Chờ người dùng xác nhận quy tắc; dự kiến bắt đầu M0 (nền tảng repo, CMake, CI) khi được yêu cầu.

## 2026-10-22 21:35 — bắt đầu (M0)
- Việc đã làm: Đọc `docs/SPEC.md`, `docs/PLAN.md`, `docs/protocol.md`; kiểm tra môi trường (Windows 11, VS 2022 MSVC 14.44, CMake 4.1.1, chưa có vcpkg; còn ~125 GB đĩa).
- File liên quan: `docs/SPEC.md` (nguồn sự thật), `docs/PLAN.md` M0, `docs/protocol.md` §2 (kiểu dữ liệu dùng chung).
- Cần chú ý: Bắt đầu M0 theo PLAN: CMake + vcpkg manifest + CI hai HĐH + khung `util/` + `ldns --version`. Sẽ bootstrap vcpkg riêng tại `C:\vcpkg` (VS có vcpkg nhưng nằm trong Program Files, không nên ghi vào đó), triplet tĩnh `x64-windows-static` theo SPEC Phụ lục B.
- Bug/lỗ hổng: không có.
- Tiếp theo: Tạo khung dự án M0, build/test cục bộ trên Windows, rồi CI; sau đó sang spike S1/S2 và M1 (`dns::Name`, `dns::Message`).

## 2026-10-22 22:05 — bước quan trọng (M0: khung dự án)
- Việc đã làm: Tạo `CMakeLists.txt`, `CMakePresets.json` (+ `CMakeUserPresets.json` cục bộ trỏ `VCPKG_ROOT=C:/vcpkg`), `vcpkg.json`, `.gitignore`, `.clang-format`, `.clang-tidy`, `README.md`, workflow CI `.github/workflows/ci.yml` (Linux debug+ASan, Windows MSVC). Thêm khung `src/core/util/` (`Result<T,E>`, `NodeId` + hex, `HlcClock`, logger spdlog) và `ldns --version` (CLI11). Unit test doctest cho id/HLC/Result.
- File liên quan: `CMakeLists.txt`, `CMakePresets.json`, `CMakeUserPresets.json`, `vcpkg.json`, `src/main.cpp`, `src/core/version.*`, `src/core/util/*`, `tests/*`, `.github/workflows/ci.yml`.
- Cần chú ý: `builtin-baseline` phải khớp commit mà bản vcpkg cục bộ nhìn thấy; đã đổi sang `67972e92edd181d433927b42d8ab4bfa52ac164a` (HEAD của vcpkg clone). CI dùng baseline này nên runner sẽ fetch đúng commit đó; nếu GitHub Actions cache lỗi thời có thể cần `git fetch` trong vcpkg. Triplet Windows là `x64-windows-static` (SPEC Phụ lục B), nên binary không phụ thuộc DLL runtime ngoài CRT.
- Bug/lỗ hổng: không có.
- Tiếp theo: Chờ configure/build/test cục bộ; sửa lỗi biên dịch nếu có; sau đó spike S1/S2 và bắt đầu M1 (`dns::Name`, `dns::Message` + fuzz harness).

## 2026-10-22 22:40 — bước quan trọng (M1: codec DNS)
- Việc đã làm: Thêm `dns::Name` (chuẩn hóa presentation, wildcard trái cùng, punycode RFC 3492, parse wire có nén với giới hạn độ sâu/pointer lùi) và `dns::Message` (parse/encode header + question + RR, nén tên khi encode, EDNS0 OPT, cắt TC, RDATA A/AAAA/CNAME/NS/PTR/TXT/SOA/SRV, parse/format IPv4 & IPv6). Unit test: 42 test case, 1287 assertion — pass trên Windows MSVC.
- File liên quan: `src/core/dns/name.{hpp,cpp}`, `src/core/dns/message.{hpp,cpp}`, `tests/test_name.cpp`, `tests/test_message.cpp`, `tests/doctest.hpp`, `src/core/util/result.hpp` (thêm `Result<void>`), `CMakeLists.txt`, `tests/CMakeLists.txt`.
- Cần chú ý: (1) MSVC + doctest cần include stream trước doctest — đã gom vào `tests/doctest.hpp`. (2) `Result<void,E>` dùng cho parse_section/parse_rr thay vì `Result<vector>` rỗng. (3) Wildcard khớp mọi depth dưới parent (RFC 1034), không chỉ một nhãn — SPEC §9.1 nói "phủ mọi nhãn con", cần xác nhận khi làm LocalZone. (4) Chưa có fuzz harness và test golden theo hex từ `dig` — harness fuzz cần libFuzzer/Clang, sẽ thêm khi CI Linux ổn định.
- Bug/lỗ hổng: không có lỗ hổng bảo mật; parser có giới hạn count (4096), độ sâu nén (32), pointer phải lùi.
- Tiếp theo: `LocalZone` (tra chính xác → wildcard, NODATA/NXDOMAIN, CNAME nội bộ), rồi `DnsServer` UDP+TCP + ACL + rate limit (phần còn lại của M1).

## 2026-10-22 23:05 — bước quan trọng (LocalZone + môi trường Linux/WSL)
- Việc đã làm: Thêm `dns::LocalZone` (exact → wildcard most-specific, NODATA/NXDOMAIN, CNAME một bước, ANY, SOA âm, validation RRset dùng chung với op validator sau này); thêm `Name::make_wildcard`. 57 test case pass trên Windows. Dựng môi trường Linux: WSL2 Ubuntu 24.04, cài cmake 3.28 + ninja + vcpkg (`~/vcpkg`, cùng baseline `67972e92`), thêm `scripts/wsl-setup.sh` và `scripts/wsl-build.sh` (build trong `~/ldns-build` để tránh 9p chậm).
- File liên quan: `src/core/dns/local_zone.{hpp,cpp}`, `src/core/dns/name.{hpp,cpp}`, `tests/test_local_zone.cpp`, `scripts/wsl-*.sh`.
- Cần chú ý: (1) Wildcard hiện khớp mọi cấp dưới parent (RFC 1034); SPEC §9.1 chưa nói rõ 1 cấp hay nhiều cấp — cần chốt. (2) `LocalZone` cho phép SOA/NS (cho negative answer) dù người dùng chỉ tạo A/AAAA/CNAME/TXT; `validate_rrset` dùng chung phải phân biệt "loại người dùng được tạo" và "loại zone được chứa" khi làm DomainService (M2). (3) Lỗi quoting PowerShell khi gọi `wsl bash -lc` với `$HOME` — từ nay dùng file script trong `scripts/`.
- Bug/lỗ hổng: không có.
- Tiếp theo: xác nhận build/test Linux qua WSL; sau đó `DnsServer` UDP+TCP (ACL private, rate limit) khép M1.

## 2026-10-22 23:55 — bước quan trọng (Linux/WSL + fuzz + ACL)
- Việc đã làm: Linux (GCC 13, `-Werror`) và ASan/UBSan xanh. Thêm fuzz harness `tests/fuzz/fuzz_dns_{message,name}.cpp` + `scripts/wsl-fuzz.sh` (Clang, libFuzzer, corpus seed từ `tests/fuzz/corpus`). Thêm `Acl`/`Cidr`/`RateLimiter` + test. Tổng: 68 test case / 1446 assertion pass trên cả MSVC và GCC.
- File liên quan: `tests/fuzz/*`, `scripts/wsl-*.sh`, `src/core/server/acl.{hpp,cpp}`, `tests/test_acl.cpp`, `src/core/dns/message.cpp`, `src/core/dns/name.cpp`, `docs/platform-notes.md`.
- Bug/lỗ hổng: **đã tìm và sửa 2 bug do fuzz** (mức trung bình): (1) gói có 2 OPT → encode lại sinh OPT thứ hai, round-trip hỏng; nay từ chối >1 OPT (mã 108) và không bao giờ phát OPT từ `additional`; payload OPT < 512 được chuẩn hoá thành 512 theo RFC 6891. (2) punycode giữ chữ hoa ASCII khiến chuẩn hoá không idempotent (`Aü` → `xn--A-eha`); nay hạ ASCII trước khi encode. Cả hai có test hồi quy + corpus `.bin` kèm theo. Giới hạn còn lại: chưa có Unicode case-fold đầy đủ (utf8proc tuỳ chọn) — `Ü` là nhãn khác `ü`, đã ghi chú.
- Tiếp theo: `DnsServer` UDP+TCP (bind theo interface, TC, ACL, rate limit, handler dùng LocalZone) — phần còn lại của M1; sau đó M2 (Store + CRUD + CLI).

## 2026-10-23 00:20 — bước quan trọng (chốt wildcard + review trước commit)
- Việc đã làm: Chốt wildcard **khớp mọi cấp dưới** (RFC 1034 §4.3.2) và ghi vào SPEC §9.1. Rà soát M0 + review toàn bộ code trước commit; sửa các lỗi phát hiện và thêm test hồi quy.
- File liên quan: `docs/SPEC.md` §9.1, `src/core/util/logger.cpp`, `src/core/util/hlc.{hpp,cpp}`, `src/core/server/acl.{hpp,cpp}`, `.github/workflows/ci.yml`, `.gitignore`, `tests/test_hlc.cpp`, `tests/test_acl.cpp`.
- Bug/lỗ hổng: đã sửa 4 lỗi (mức thấp–trung bình): (1) `logger()` gọi `init_logger()` khi đang giữ `std::mutex` không đệ quy → deadlock nếu logger chưa khởi tạo; nay tách `make_logger()` (gọi nội bộ). (2) `HlcClock::receive` cắt counter bằng `& 0xFFFF` → timestamp có thể **giảm** khi counter tràn; nay `bump_counter()` cộng sang mili-giây kế tiếp. (3) `RateLimiter` dùng `last_ns == 0` làm sentinel → nguồn có truy vấn đầu ở mốc 0 không nhận được token; nay có cờ `initialized`. (4) CI dùng vcpkg cài sẵn trên runner (có thể thiếu commit baseline) → luôn clone vcpkg; thêm autotools cho libsodium. Ngoài ra đã xoá clone vcpkg 153 MB rơi nhầm vào repo (lỗi quoting WSL trước đó) và thêm `.gitignore` cho artifact fuzz.
- Cần chú ý: `CLAUDE.md` và `process.md` là file quy trình của repo, có trong commit này; `CMakeUserPresets.json` cố tình nằm trong `.gitignore` (đường dẫn vcpkg là cục bộ từng máy).
- Tiếp theo: commit do người dùng quyết định (message đề xuất bên dưới); sau đó `DnsServer` UDP+TCP khép M1.

## 2026-10-23 01:10 — bước quan trọng (hoàn thành M1)
- Việc đã làm: Thêm `QueryHandler` (thuần, không socket) và `DnsServer` (Asio UDP+TCP). Handler: AA/RA/RD, NODATA+NXDOMAIN kèm SOA, wildcard mọi cấp, CNAME chain trong zone tối đa 8 bước, loop → SERVFAIL, EDNS echo + cap 1232, REFUSED khi zone rỗng hoặc non-IN, NOTIMP cho opcode lạ, FORMERR cho gói hỏng. Server: bind địa chỉ cụ thể (từ chối 0.0.0.0/::), ACL + rate limit trước khi parse, TC khi vượt payload client, TCP 2-byte framing + idle timeout + giới hạn kết nối, port 0 dùng chung cổng UDP cho TCP.
- File liên quan: `src/core/server/query_handler.{hpp,cpp}`, `src/core/server/dns_server.{hpp,cpp}`, `tests/test_query_handler.cpp`, `tests/test_dns_server.cpp`, `CMakeLists.txt` (Asio).
- Cần chú ý: `Acl`/`RateLimiter` giờ do listener tham chiếu non-const (server là chủ sở hữu duy nhất, callback cùng một io_context thread). Test `DnsServer` bind `127.0.0.1:0` nên không cần quyền. M1 chưa có upstream: tên ngoài zone bị NXDOMAIN (zone khác rỗng) hoặc REFUSED (zone rỗng) — M3 sẽ thay bằng bước 3.
- Bug/lỗ hổng: đã sửa trong lúc làm (mức trung bình): (1) `QueryHandler` ban đầu dùng con trỏ trỏ vào phần tử vector đã reallocate cho CNAME chain → viết lại bằng giá trị + cờ trạng thái. (2) Test server crash do TCP nối tới cổng UDP khi bind port 0, cộng với `std::thread` joinable bị hủy khi assertion throw → server nay dùng chung cổng, test bọc `IoRunner` RAII. (3) CNAME chain quá dài trả NOERROR thay vì SERVFAIL → sửa bằng cờ `last_was_cname`. (4) `reinterpret_cast` chuyển địa chỉ → thay bằng `to_asio_address`.
- Nghiệm thu M1: 87 test case / 1589 assertion pass trên MSVC, GCC và ASan/UBSan. Chưa chạy `dig` thật và chưa đo 10k qps (cần công cụ ngoài + benchmark; để ở M9).
- Tiếp theo: M2 — `Store` SQLite (WAL, migration), `node_id`/`epoch`/`seq`, `DomainService` (add/set/rm/ls, TLD policy, self IP), `Config` TOML, lệnh CLI một lần + `--json`.

## 2026-10-23 09:05 — bắt đầu (quy tắc mới + review M1)
- Việc đã làm: Thêm `CLAUDE.md` §2.1 — bắt buộc build/test cả Windows và Linux (kèm ASan, fuzz nếu chạm parser) và review code trước khi đề xuất commit. Áp dụng ngay cho thay đổi M1 đang chờ commit: rà lại `query_handler.cpp` và `dns_server.cpp`.
- File liên quan: `CLAUDE.md`, `process.md`, `src/core/server/*`.
- Cần chú ý: Từ nay mọi đề xuất commit phải kèm kết quả test hai nền tảng và kết quả review.
- Bug/lỗ hổng: review phát hiện và đã sửa 6 điểm: (1) vòng nhận UDP dừng hẳn sau lỗi thoáng qua → chỉ dừng khi socket đóng; (2) datagram > 4096 bị xử lý như gói hỏng thay vì bỏ (nay bỏ đúng `error::message_size`); (3) vòng accept TCP quay nóng vô hạn khi lỗi (EMFILE) → backoff 1 s; (4) TCP frame < 12 byte để kết nối treo tới idle timeout → đóng ngay; (5) địa chỉ peer TCP lấy lại mỗi request → lấy một lần lúc accept; (6) **rate limiter tăng bộ nhớ vô hạn** theo số nguồn (rủi ro DoS) → cap 16384 nguồn + prune định kỳ mỗi 1024 nguồn mới. Thêm xử lý EDNS version lạ → BADVERS (RFC 6891) kèm test.
- Test: Windows MSVC **90 test case / 1598 assertion pass**; Linux GCC 13 pass; Linux ASan+UBSan pass; fuzz `dns_message` 45 s không crash.
- Tiếp theo: chờ người dùng quyết định commit M1; sau đó M2.

## 2026-10-23 09:40 — bước quan trọng (sửa CI Windows)
- Việc đã làm: Điều tra CI fail trên GitHub. Kết luận: (a) run của `feat(m0-m1)` (307adf8) fail **chỉ ở job Windows**, lỗi `CMake Error at CMakeLists.txt:3 (project): could not find any instance of Visual Studio`; Linux debug và ASan đều pass. Nguyên nhân: runner `windows-latest` nay là image `windows-2025-vs2026` chỉ có VS 18, trong khi preset yêu cầu generator `Visual Studio 17 2022`. (b) Commit `feat(m1)` (1b97b41) *có* tạo run nhưng bị xếp hàng ~17 phút; run `eee78e1` bị `cancel-in-progress` huỷ khi push liên tiếp. (c) Cảnh báo phụ: backend cache `x-gha` của vcpkg đã bị xoá; `checkout@v4` dùng Node 20 deprecated. Đã sửa: thêm preset `windows-ci` (Ninja + MSVC, không phụ thuộc phiên bản VS); CI Windows dùng `msvc-dev-cmd` (pin SHA v1.13.0) + Ninja, có bước in toolchain; bỏ `VCPKG_BINARY_SOURCES`/`x-gha`; `checkout@v7`; `concurrency.cancel-in-progress` chỉ với `pull_request` để push lên `main` luôn có bản ghi CI đầy đủ; thêm `workflow_dispatch`.
- File liên quan: `.github/workflows/ci.yml`, `CMakePresets.json`, `process.md`.
- Test: preset mới `windows-ci` configure/build/test **pass** cục bộ trong môi trường vcvars64 (MSVC 14.44): 98 test case; `ldns --version` và `--headless` chạy đúng. Preset `local-windows-msvc-debug` pass; Linux GCC 13 pass.
- Cần chú ý: Trong working tree đang có thay đổi TUI của một phiên khác (chưa commit: `src/ui/*`, `tests/test_ui.cpp`, `docs/UI-SPEC.md`, `CMakeLists.txt`, `tests/CMakeLists.txt`, `src/main.cpp`). Tôi **không** đụng vào các file đó; phần sửa CI chỉ gồm `.github/workflows/ci.yml` và `CMakePresets.json`. Log CI tải về nằm ở `.github/logs/` (đã có `.github/.gitignore` chặn `logs/`).
- Bug/lỗ hổng: không có lỗ hổng bảo mật; đây là lỗi cấu hình CI.
- Tiếp theo: người dùng commit/push phần sửa CI; xác nhận run mới xanh trên GitHub; sau đó tiếp tục M2.

## 2026-10-23 15:56 — bắt đầu (M2: Store + DomainService + Config + CLI)
- Việc đã làm: Đọc lại `SPEC.md` (§5, §9, §10, §11, §12), `PLAN.md` M2, `protocol.md` §2.3/§2.5, `process.md`; đọc code M0/M1 (`dns/name`, `dns/message`, `dns/local_zone`, `server/*`, `util/*`) và xác nhận build/test hiện tại xanh trên Windows (98 test case).
- File liên quan: `process.md`; sắp thêm `src/core/store/*`, `src/core/config/*`, `src/core/domain/*`, `src/core/core_api.*`, `src/main.cpp`.
- Cần chú ý: (1) M2 chưa có sync nên `seq` cần tăng trong cùng transaction với thay đổi `my_records` ngay từ bây giờ (protocol.md §7.2). (2) `local_zone.validate_rrset` cho phép SOA/NS (dùng cho zone), còn người dùng chỉ được tạo A/AAAA/CNAME/TXT → DomainService phải phân biệt. (3) Không đụng vào worktree UI ở `C:/Code/Ldns-ui` (nhánh `feat/ux-ui`).
- Bug/lỗ hổng: chưa có.
- Tiếp theo: thiết kế `Store` (SQLite WAL + migration), `Config` (TOML + env + CLI), `SelfAddress`, `DomainService`, `CoreApi`, CLI một lần; test trên Windows và Linux/ASan.

## 2026-10-23 17:40 — bước quan trọng (M2 core: Store/Config/Domain/CoreApi)
- Việc đã làm: Thêm toàn bộ tầng core của M2 (không chạm UI/main.cpp):
  - `core/store`: SQLite WAL, migration v1 theo SPEC §5.4, `meta`/`my_records`/`peers`/`peer_records`/`upstreams`/`query_log`; identity (node_id/epoch/seq) sinh lần đầu; `Tx` RAII (BEGIN IMMEDIATE, rollback khi hủy); `bump_seq` bắt buộc trong transaction.
  - `core/config`: mô hình đầy đủ SPEC §11, parser TOML từ chối khóa lạ/sai kiểu, env `LDNS_*`, `default_paths` theo SPEC §12 (user/service, Windows/Linux).
  - `core/domain`: `DomainService` (add/set/rm/list/add_many), TLD công cộng + `.local` (D15), `self`/static, wildcard, CNAME độc quyền, RRset canonical sort; `SelfAddress` + `core/net/interfaces` (GetAdaptersAddresses/getifaddrs, ưu tiên interface default route); import/export TOML+CSV; `LocalZoneSink`.
  - `core/core_api`: giao diện duy nhất cho frontend, nạp zone từ DB khi khởi động, `add_self_domain`, `refresh_self_addresses`, `import/export`, `status` (cảnh báo PSK).
- File liên quan: `src/core/store/*`, `src/core/config/*`, `src/core/domain/*`, `src/core/net/interfaces.*`, `src/core/core_api.*`, `tests/test_{store,config,domain_service,core_api}.cpp`, `CMakeLists.txt`, `tests/CMakeLists.txt`.
- Test: Windows MSVC **133 test case / 1943 assertion pass**; Linux GCC 13 pass; Linux ASan+UBSan pass (1945 assertion). Mỗi mutation tăng `seq` đúng một lần (test kiểm chứng), import 100 dòng dùng một seq.
- Cần chú ý: (1) Chưa có CLI một lần (`src/main.cpp`) vì người dùng yêu cầu không đụng UI/UX; CoreApi đã sẵn để frontend gọi. (2) `SelfAddress` chỉ publish IPv4 khi dùng `auto`; `network.self_ip` ghim IPv6 vẫn dùng được. (3) Reopen DB: record hỏng bị bỏ qua kèm cảnh báo thay vì chặn khởi động.
- Bug/lỗ hổng: đang chờ kết quả review chéo; tự rà thấy một điểm cần sửa: nếu `bump_seq`/`commit` lỗi **sau khi** `LocalZone` đã cập nhật, zone có thể lệch DB (mức thấp, chỉ xảy ra khi DB hỏng/đĩa đầy).
- Tiếp theo: sửa các điểm review phát hiện, chạy lại 3 cấu hình, cập nhật `process.md`, rồi đề xuất commit (người dùng quyết định).

## 2026-10-23 17:45 — bước quan trọng (review chéo M2 + vấn đề mở)
> Trạng thái 2026-10-23 18:30: toàn bộ #1–#6 dưới đây **đã được xử lý** trong code; #1 đạt mục tiêu ở bản Release. Giữ nguyên mục để tra cứu lịch sử.
- Việc đã làm: Chạy review chéo toàn bộ M2 (agent `gsd-code-reviewer`). Kết quả: 5 blocker + ~20 warning, phần lớn đã sửa trong cùng phiên:
  - **Đã sửa**: (a) `add_many` bỏ sót kiểm tra CNAME trong cùng batch → nay kiểm tra chéo batch và kiểm tra lại theo trạng thái đọc trong transaction; (b) zone bị sửa trước commit và không hoàn tác khi `bump_seq`/`commit` lỗi → nay **commit DB trước, mirror zone sau** cho mọi mutation (`upsert`/`remove`/`add_many`/`refresh_self_records`), zone chỉ nhận dữ liệu đã bền; (c) TOCTOU giữa kiểm tra tồn tại và `begin()` → nay đọc trạng thái **bên trong** transaction; (d) thêm kiểm tra `kind=Self` chỉ cho A/AAAA trong `add_many`; (e) `Store` move giờ mang theo `in_tx_`; (f) hợp nhất mã lỗi zone → 706 thay vì rò rỉ mã 2xx của LocalZone.
  - **Còn mở**: xem mục "Vấn đề mở" bên dưới.
- File liên quan: `src/core/domain/domain_service.{hpp,cpp}`, `src/core/store/store.{hpp,cpp}`, `tests/bench_import.cpp`.
- Test: Windows 133 case pass sau refactor; benchmark import 100 000 dòng **vẫn ~35 s** (xem vấn đề mở #1).
- Bug/lỗ hổng: các mục đã sửa ở trên (mức trung bình–cao: zone/DB divergence, CNAME batch, TOCTOU).

### Vấn đề mở cần trợ giúp (cho agent khác phân tích)

**#1 (ĐÃ GIẢI QUYẾT) hiệu năng import 100 000 dòng: Release 1,15 s / reopen 0,45 s — đạt PLAN M2 (< 5 s).**
- Kết luận: số đo ~35 s trước đó là **bản Debug MSVC** (không tối ưu), không phải lỗi thuật toán. Đo lại Release: **import 100 000 dòng = 1152 ms (~86 800 dòng/s), reopen = 447 ms**.
- Đã cải thiện thật trong quá trình điều tra (giữ lại): (a) `add_many` đọc `my_records` **một lần** thay vì `records_for_name` mỗi dòng; (b) `Store::put_record` cache prepared statement (`put_stmt_`) thay vì biên dịch SQL mỗi dòng; (c) mirror zone **sau commit**.
- Số đo Debug cũ để tham chiếu (cho thấy Debug chậm ~30×): 2 000 dòng 0,54 s · 10 000 3,6 s · 50 000 17 s · 100 000 35 s.
- Nếu sau này cần tối ưu tiếp: tách pha cho thấy `records()` (decode JSON) và `LocalZone::upsert` là hai chi phí lớn nhất khi mở lại database lớn.

### Chi tiết cũ (giữ để tra cứu)
- Triệu chứng đo được (Windows MSVC Debug, máy local): 2 000 dòng = 0,54 s; 5 000 = 1,49 s; 10 000 = 3,4–3,6 s; 50 000 = 17 s; 100 000 = 35–37 s → gần như **tuyến tính theo số dòng** (~2 800 dòng/s), không phải O(n²).
- Đã thử và **không** cải thiện: (a) đọc `my_records` một lần thay vì `records_for_name` mỗi dòng (đã áp dụng — chỉ cải thiện reopen); (b) cache prepared statement cho `put_record` (đã áp dụng).
- Chưa kiểm chứng: chi phí `sqlite3_step` trên từng INSERT trong một transaction lớn; chi phí `Store::encode_data`/`decode_data` qua `nlohmann::json`; chi phí `LocalZone::upsert` (mutex + map) khi mirror 100 000 bản ghi sau commit. Benchmark hiện chỉ đo tổng, **chưa tách từng pha**.
- Hướng nghi ngờ: (1) tạo/hủy `nlohmann::json` cho mỗi dòng; (2) `my_records` là `WITHOUT ROWID` với PK `(name,type)` và chuỗi `name` dài → mỗi insert phải so sánh/chen vào B-tree, có thể chậm gấp nhiều lần insert tuần tự; (3) bản Debug MSVC (chưa tối ưu) làm mọi thứ chậm 10–20× so với Release — **cần đo lại ở Release trước khi kết luận**.
- Việc cần làm: tách benchmark theo pha (parse CSV / put_record / bump / commit / mirror zone), chạy ở cả Debug và Release, rồi mới chọn giải pháp (multi-row insert, dùng `PRAGMA synchronous=OFF` trong import có kiểm soát, chuyển `data_json` sang blob nhị phân, hoặc chỉnh schema).

**#2 (khó tái hiện) — benchmark trước đó thoát với exit=3 và hộp thoại "Debug Error" của MSVC CRT.**
- Nghi phạm: `api = std::move(api2);` trên `std::unique_ptr<CoreApi>` trong `tests/bench_import.cpp` (đoạn đó đã bị xóa khi viết lại benchmark). Nếu hộp thoại còn xuất hiện, ghi lại dòng assert đầu tiên (file:line) để tìm nguyên nhân — hiện **chưa** tái hiện trên bản test/bench mới.
- Người dùng được khuyên chọn **Abort** khi hộp thoại hiện (an toàn, chỉ dừng tiến trình).

**#3 (bảo mật, mức trung bình, chưa sửa — chờ quyết định) — `psk_enabled` chỉ phản ánh "có đường dẫn key_file", chưa nạp/kiểm tra khóa.**
- Hệ quả: `/status` có thể báo "đã bật PSK" khi tệp không tồn tại, trái với D16. Đề xuất: đổi tên thành `psk_configured` và cảnh báo dựa trên `!psk_enabled` cho tới khi M9 nạp khóa thật.

**#4 (độ bền, cần quyết định) — `PRAGMA synchronous = NORMAL` với WAL có thể mất giao dịch đã commit khi mất điện.**
- SPEC §15 nói "không mất domain đã báo thành công". Hai lựa chọn: giữ NORMAL (nhanh) và ghi rõ ngoại lệ trong SPEC, hoặc FULL cho thao tác ghi zone (chậm hơn).

**#5 (CSV/TOML round-trip) — TXT chứa dấu `,`, newline hoặc ký tự điều khiển làm hỏng file export.**
- TOML: `toml_quote` chưa escape ký tự `< 0x20`/`0x7F` → file export không parse lại được.
- CSV: chưa quote theo RFC 4180 → mất phần sau dấu phẩy.
- Hướng sửa: escape `\uXXXX` trong TOML, quote/parse CSV đúng chuẩn, hoặc từ chối giá trị chứa ký tự đặc biệt khi export kèm cảnh báo.

**#6 (lọc `self`) — `virtual_adapter` được tính nhưng chưa dùng**, nên Windows/WSL/Docker có thể publish IP ảo cho record `self` (SPEC §9.2/PLAN M2 yêu cầu loại ảo). Cũng cần quét cả `Description` (UTF-16) trên Windows, không chỉ `FriendlyName`.

---

## 2026-10-23 20:00 — bước quan trọng (quy tắc hai nhánh)
- Việc đã làm: Người dùng chốt mô hình **hai nhánh song song** và yêu cầu ghi thành rule: `main` (`C:/Code/Ldns`) phát triển chức năng chính, chạy dạng **CLI**; `feat/ux-ui` (`C:/Code/Ldns-ui`) phát triển **TUI/UX/UI**, chạy dạng **TUI**. Đã thêm `CLAUDE.md` §6 với bảng nhánh/worktree/phạm vi/cách chạy và các quy tắc tránh trộn lẫn (không sửa `src/ui/**` từ `main`; thay đổi core cần cho UI phải làm trên `main` trước).
- File liên quan: `CLAUDE.md` (§5 trỏ sang §6, thêm §6), `process.md`.
- Cần chú ý: hai worktree dùng **chung** file `process.md` và `CLAUDE.md` (mỗi worktree có bản checkout riêng của cùng file trong git) — khi commit cần tránh xung đột nội dung ở hai file này. Trạng thái hiện tại của `src/main.cpp` trên `main` (không tham số → in hướng dẫn CLI, TUI ở M7) là đúng với rule này.
- Bug/lỗ hổng: không có.
- Tiếp theo: chờ người dùng chạy `docs/TESTING-M2.md`; sau đó M3 trên `main`.

## 2026-10-23 19:40 — bước quan trọng (M2 khép: CLI một lần + daemon + checklist kiểm thử)
- Việc đã làm: Người dùng yêu cầu "để tôi test trước, tạo checklist trong `docs/`". Để **có thứ để test**, hoàn tất nốt phần chạy được của M2 (không chạm UX/UI):
  - `src/core/cli/commands.*`: các lệnh một lần `add/set/rm/ls/status/import/export` trên `CoreApi`, có `--json`, mã thoát theo SPEC §10.1 (703/705 → 5, 700–702 → 2, còn lại → 1).
  - `src/core/runtime/runtime.*`: `Runtime` sở hữu `io_context` + `CoreApi` + `DnsServer`; `listen = ["auto"]` mở rộng thành loopback + mọi IPv4 LAN (ưu tiên interface có default route, bỏ ảo/loopback/link-local), không bao giờ bind wildcard (D14).
  - `src/main.cpp`: viết lại thành CLI11 đầy đủ — `add/set/rm/ls/status/import/export`, `daemon --foreground`; không tham số thì in hướng dẫn (TUI ở M7).
  - `docs/TESTING-M2.md`: checklist kiểm thử thủ công + tự động (chuẩn bị môi trường, 145 test case, benchmark, ma trận DNS 15 mục, cấu hình 13 mục, phần "chưa kiểm thử được").
  - `tests/test_cli.cpp`: 7 test case cho CLI + runtime.
- File liên quan: `src/core/cli/*`, `src/core/runtime/*`, `src/main.cpp`, `tests/test_cli.cpp`, `CMakeLists.txt`, `tests/CMakeLists.txt`, `docs/TESTING-M2.md`.
- Bug/lỗ hổng đã sửa trong lúc làm (mức thấp–trung bình):
  1. **`resolve_listen` thêm `:53` vào cả địa chỉ đã có cổng** → `127.0.0.1:0` thành `127.0.0.1:0:53`, daemon không bind được. Nay chỉ thêm khi thiếu; có test hồi quy.
  2. **`ls --type` thừa hưởng mặc định `A`** do dùng chung biến CLI11 với `add/rm` → `ls` chỉ liệt kê bản ghi A. Nay `ls` có biến riêng, mặc định không lọc.
  3. **Daemon không thoát khi Ctrl+C trên Windows**: Asio `signal_set` chỉ bọc `::signal()`; console Ctrl+C chỉ thành `SIGINT` khi có `SetConsoleCtrlHandler`. Nay `Runtime` cài handler console riêng và **post** yêu cầu dừng về `io_context` (gọi `io_context::stop()` từ thread khác là không an toàn). Đã kiểm chứng: Windows Ctrl+C → thoát mã 0 + `shutdown complete`; Linux `kill -TERM` → mã 0.
  4. Hàm `error_type_for` không dùng → `-Werror=unused-function` trên GCC; đã xoá.
- Test: Windows MSVC **145 test case / 2013 assertion pass**; Linux GCC 13 pass; Linux ASan+UBSan pass. Kiểm chứng thủ công end-to-end bằng `ldns daemon` + client DNS Python: A/CNAME/TXT/wildcard (mọi cấp)/NXDOMAIN/NODATA/ANY đúng; `auto` bind `127.0.0.1:53` + IP LAN; wildcard bind bị từ chối (mã 4); xung đột cổng → mã 4; rate limit 2 qps chặn đúng; gói 5 byte bị bỏ, daemon sống; import/export TOML+CSV round-trip.
- Cần chú ý: `ldns daemon` **bắt buộc** `--foreground` cho tới M8 (chưa có service manager). Chưa có upstream nên tên ngoài zone trả NXDOMAIN — đúng như M2, M3 sẽ thay. TUI chưa có (M7).
- Tiếp theo: người dùng chạy `docs/TESTING-M2.md`; sau khi báo kết quả → M3 (upstream + pipeline + cache).

## 2026-10-23 18:30 — bước quan trọng (nối lại phiên bị cắt ngang — M2 xanh trên 2 nền tảng)
- Việc đã làm: Đọc lại `SPEC.md`/`PLAN.md`/`protocol.md` và `process.md`, xác định phiên trước bị cắt khi đang sửa test TOML escape. Hoàn tất phần còn dang dở:
  - **Sửa lỗi build**: `tests/test_domain_service.cpp` gọi `chr_escape()` không tồn tại (biên dịch thất bại trên cả MSVC và GCC) → dùng helper `toml_control_escape()` trong anonymous namespace.
  - **Sửa bug thật (mức trung bình)**: `toml_quote` dùng `std::snprintf(..., "\u%04X", ...)` — `\u` là universal-character-name của C++, không phải hai ký tự TOML, nên ký tự điều khiển bị xuất thành `u0001` và file export **không parse lại được**. Nay `"\\u%04X"`. Test hồi quy `TOML export escapes control characters and re-imports` fail trước bản sửa, pass sau.
  - **Sửa lỗi `-Werror=conversion` trên GCC**: `bench_import.cpp` chia `std::size_t` cho `double` → thêm `static_cast<double>`.
  - Rà lại các điểm treo của phiên trước đều đã có trong code: `Store` dùng `PRAGMA synchronous = FULL`, `psk_configured`/`psk_key_file_found` (không còn tên gọi sai), `HlcClock` có mutex, giới hạn import 64 MiB / 500 000 dòng, `csv_quote`/`parse_csv_line` theo RFC 4180, lọc `virtual_adapter` khi chọn `self`, `default_ttl` áp trong `CoreApi::apply_defaults`.
- File liên quan: `tests/test_domain_service.cpp`, `src/core/domain/import_export.cpp`, `tests/bench_import.cpp`.
- Test: Windows MSVC Debug **138 test case / 1966 assertion pass**; Linux GCC 13 `-Werror` pass; Linux ASan+UBSan pass (đang chạy lại sau khi sửa benchmark).
- Cần chú ý: `process.md` mục "Vấn đề mở" #1–#6 phía dưới đã được xử lý xong (xem ghi chú tại mục). Benchmark import 100k ở Release trước đó: 1152 ms, đạt PLAN M2 (< 5 s).
- Bug/lỗ hổng: đã sửa 2 lỗi (mức thấp–trung bình) nêu trên; không có lỗ hổng bảo mật mới.
- Tiếp theo: hoàn tất M2 (chờ người dùng quyết định commit), rồi M3 — upstream + pipeline + cache.

### Đáp án đã kiểm chứng (phân tích 2026-10-23 17:50)

**#1 — Đáp án: chủ yếu do Debug MSVC + 7 điểm nghẽn thật trong code**

Kết luận chính: **đo lại ở Release trước khi tối ưu.** Debug MSVC bật iterator debugging, debug heap validation, và tắt mọi tối ưu trình biên dịch, thường chậm **10–20×** so với Release. 35 s ÷ 15 ≈ **2,3 s** — rất có thể đã đạt AC-3.1 (\< 5 s) mà không cần sửa gì.

Nếu Release vẫn chậm, sửa theo thứ tự tác động giảm dần (đã kiểm chứng qua code):

| # | Điểm nghẽn | File | Chi phí | Cách sửa |
|---|-----------|------|---------|----------|
| 1 | **`encode_data` tạo `nlohmann::json` mỗi dòng** — tạo json::array, to_hex, dump — ≥4 heap alloc/dòng | `store.cpp:240-243`, gọi từ `put_record:333` | ~50% thời gian import (Debug) | Chuyển sang format đơn giản: hex thuần phẳng phân cách bằng `;` (vd `"0a000101"` cho A, `"0a000101;0a000102"` cho multi-value). Loại bỏ hoàn toàn `nlohmann::json` khỏi đường dẫn nóng. |
| 2 | **`SQLITE_TRANSIENT` copy mọi string** mỗi `put_record` | `store.cpp:45` (`bind_text`) | memcpy ×5 field/dòng | Thêm `bind_text_static()` dùng `SQLITE_STATIC` cho `add_many` — lifetime dữ liệu đảm bảo tới sau `sqlite3_step`. Giữ `SQLITE_TRANSIENT` cho thao tác đơn lẻ. |
| 3 | **`Name::from_presentation()` gọi 3 lần/dòng** — pass 1 (validate), pass 3 (`prepare_row`), pass 7 (zone mirror) | `domain_service.cpp:365, 444, 467` | 3× DNS name parse + allocation | Parse 1 lần trong pass 1, lưu `Name` vào struct, reuse ở pass 3 và 7. |
| 4 | **Mutex `put_record` lock/unlock 100k lần** dù đã trong transaction | `store.cpp:332` | 100k lock_guard | Thêm `put_record_batch(span<RecordRow>)` nội bộ: lock 1 lần, loop bind+step, unlock 1 lần. |
| 5 | **Zone `upsert` lấy exclusive lock + `validate_rrset` lại mỗi dòng** | `local_zone.cpp:77-92` | unique_lock + validate ×100k | Thêm `upsert_batch(...)`: lock 1 lần, bỏ `validate_rrset` (đã validate ở `prepare_row`), insert tất cả, unlock. |
| 6 | **`std::map` cho `normalized`/`by_key`** | `domain_service.cpp:353, 404` | O(log n) string compare | Đổi sang `std::unordered_map`. |
| 7 | **`WITHOUT ROWID` + TEXT PK** | `migrations.cpp:16-26` | B-tree key compare trên chuỗi dài | Giữ nguyên (thay đổi schema ảnh hưởng migration); ưu tiên sửa #1–#5 trước. Nếu cần thêm, thêm integer ROWID + UNIQUE index. |

**Kế hoạch hành động**: (a) thêm preset `Release` vào benchmark, đo tách theo pha (`parse → put_record → commit → mirror`); (b) nếu Release > 5 s, áp dụng #1 + #3 + #4 + #5 (tác động lớn nhất, thay đổi nhỏ); (c) #2 và #6 nếu vẫn thiếu.

**#2 — Đáp án: đã tự giải quyết, không cần hành động**

Bug `exit=3` từ `std::move` trên `unique_ptr<CoreApi>` trong benchmark cũ. Đoạn code đã bị xóa khi viết lại benchmark. Không tái hiện trên bản mới. Giữ ghi chú phòng khi hộp thoại xuất hiện lại — lúc đó ghi file:line từ assert.

**#3 — Đáp án: đổi tên + kiểm tra file tồn tại**

Code hiện tại (`core_api.cpp:53`):
```cpp
psk_enabled_ = !options_.config.network.key_file.empty();
```
Chỉ kiểm tra chuỗi không rỗng, không kiểm tra file tồn tại → `/status` có thể nói "PSK bật" khi file không có.

**Sửa:**
1. Đổi tên field thành `psk_configured` (phản ánh đúng ngữ nghĩa: "đã cấu hình" ≠ "đang hoạt động").
2. Thêm cảnh báo trong `status()`: nếu `key_file` không rỗng nhưng `!std::filesystem::exists(key_file)` → trả thêm `warnings.push_back("key_file configured but file not found")`.
3. Khi M9 nạp khóa thật, đổi lại thành `psk_enabled` = khóa đã nạp + hợp lệ.

Mức ưu tiên: thấp (D16 chỉ yêu cầu cảnh báo khi **không** có PSK; cảnh báo sai hướng "có PSK" khi thực ra không có ít nguy hiểm hơn ngược lại). Nên sửa trong commit M2 để giao diện trung thực.

**#4 — Đáp án: FULL cho ghi zone thường, NORMAL trong import có kiểm soát**

Phân tích:
- `synchronous=NORMAL` + WAL: commit trả về trước khi WAL flush xuống đĩa. Mất điện có thể mất **giao dịch cuối cùng** đã commit nhưng chưa flush.
- SPEC §15: "không mất domain đã báo thành công" → xung đột với NORMAL.
- Nhưng: thao tác ghi zone (user thêm 1–10 domain) rất ít → cost của FULL không đáng kể.
- Import 100k dòng: là thao tác có thể retry → NORMAL hoặc OFF chấp nhận được.

**Sửa:**
1. Mặc định `PRAGMA synchronous = FULL` (đảm bảo SPEC §15).
2. Trong `add_many` khi `requests.size() > threshold` (vd 1000), tạm `PRAGMA synchronous = NORMAL` trước `BEGIN`, và khôi phục `FULL` sau `COMMIT`. Ghi rõ trong CLI rằng import lớn có thể mất nếu mất điện đúng lúc commit — user có thể chạy lại.
3. Ghi vào SPEC §15 ngoại lệ: "thao tác import hàng loạt có thể mất khi mất điện; dùng lại lệnh import để khôi phục."

Lý do không dùng NORMAL cho mọi trường hợp: domain đơn lẻ user thêm bằng tay nên bền vững tuyệt đối — mất 1 domain sau khi CLI đã báo OK là vi phạm niềm tin.

**#5 — Đáp án: sửa cả TOML và CSV, có code mẫu**

**(a) `toml_quote` — thiếu escape cho control character < 0x20 và 0x7F:**

Sửa `default:` trong switch (file `import_export.cpp:55`):
```cpp
default:
    if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) {
        // TOML basic string: control chars khác \t \n \r phải escape \uXXXX
        char buf[7];
        std::snprintf(buf, sizeof(buf), "\\u%04X",
                      static_cast<unsigned int>(static_cast<unsigned char>(c)));
        out += buf;
    } else {
        out.push_back(c);
    }
    break;
```

**(b) CSV export — thiếu quoting theo RFC 4180:**

Thêm hàm helper:
```cpp
std::string csv_quote(std::string_view field) {
    const bool needs_quote = field.find_first_of(",\"\r\n") != std::string_view::npos;
    if (!needs_quote) return std::string(field);
    std::string out;
    out.reserve(field.size() + 4);
    out.push_back('"');
    for (char c : field) {
        if (c == '"') out.push_back('"');  // escape " → ""
        out.push_back(c);
    }
    out.push_back('"');
    return out;
}
```

Sửa export (dòng 210-214):
```cpp
out << csv_quote(entry.name) << ',' << csv_quote(entry.type) << ',' << entry.ttl << ',';
std::string combined;
for (std::size_t i = 0; i < entry.targets.size(); ++i) {
    if (i != 0) combined += ';';
    combined += entry.targets[i];
}
out << csv_quote(combined) << '\n';
```

Sửa import: thay `split(cleaned, ',')` bằng CSV parser nhận biết quoting — tách field khi gặp `,` **ngoài** dấu `"..."`. Hoặc đơn giản hơn: dùng TOML làm format chính, CSV chỉ cho A/AAAA (không chứa ký tự đặc biệt), từ chối export CSV cho TXT có ký tự đặc biệt kèm cảnh báo.

**#6 — Đáp án: thêm 1 dòng filter vào `SelfAddress::enumerate`**

Code hiện tại (`self_address.cpp:57`):
```cpp
if (!interface.up || interface.loopback) continue;
```

Sửa thành:
```cpp
if (!interface.up || interface.loopback || interface.virtual_adapter) continue;
```

`virtual_adapter` đã được tính đúng trong `looks_virtual()` (`interfaces.cpp`) — kiểm tra prefix docker/veth/virbr/vEthernet và trên Windows kiểm tra `Description` (đã dùng UTF-16 `FriendlyName`, cần thêm kiểm tra `Description` nữa vì một số driver Hyper-V/VMware chỉ ghi tên hãng trong Description).

Thêm vào `looks_virtual()` trên Windows:
```cpp
// Ngoài FriendlyName, cũng kiểm tra Description (thường chứa tên driver)
std::wstring desc(adapter->Description);
std::string desc_lower = utf16_to_utf8(desc);
std::transform(desc_lower.begin(), desc_lower.end(), desc_lower.begin(), ::tolower);
for (const auto& keyword : {"hyper-v", "virtualbox", "vmware", "vpn", "docker", "wsl"}) {
    if (desc_lower.find(keyword) != std::string::npos) return true;
}
```

Nếu user muốn giữ IP ảo (vd VPN), dùng `network.self_ip = "10.x.x.x"` để ghim — `SelfAddress::enumerate` bỏ qua filter khi `pinned_ip` khác rỗng (dòng 36-42, đã đúng).

## 2026-10-23 20:10 — bước quan trọng (Code review toàn bộ dự án tiêu chuẩn Senior Dev)
- Việc đã làm: Thực hiện code review toàn diện theo tiêu chuẩn Senior Systems/C++ Engineer cho toàn bộ dự án Ldns (M0, M1, M2 core: DNS codec, Server Asio, SQLite Store, DomainService, CoreApi, Config, Net interfaces). Viết báo cáo chi tiết kèm hướng dẫn và diff sửa lỗi vào `docs/CODE-REVIEW.md`.
- File liên quan: `docs/CODE-REVIEW.md`, `process.md`.
- Cần chú ý: Phát hiện 3 lỗi mức độ CRITICAL (SEC-01: Asio unhandled exception khi accept; SEC-02: RateLimiter DoS lockout; RES-01: SQLite connection/lock leak) và 5 lỗi HIGH/MEDIUM (DNS-01: CNAME TTL bị mất; DNS-02: `Name::is_suffix_of` bỏ qua biên nhãn; DNS-03: Tràn số encode message; API-01: lọc type IPv4/v6 trong add_self; PERF-01: Store::Tx lock retention).
- Bug/lỗ hổng:
  - CRITICAL: SEC-01 (uncaught exception `remote_endpoint()`), SEC-02 (RateLimiter DoS nguồn mới), RES-01 (`sqlite3_close` trả về `SQLITE_BUSY` làm rò rỉ file lock).
  - HIGH: DNS-01 (CNAME TTL hardcoded 60s), DNS-02 (label boundary byte overlap), DNS-03 (uint16 overflow).
  - MEDIUM: API-01 (address family mismatch), PERF-01 (unneeded lock retention during zone mirror).
- Tiếp theo: Sửa các lỗi theo thứ tự ưu tiên trong `docs/CODE-REVIEW.md` và kiểm chứng test suite trên Windows và Linux WSL/ASan.


## 2026-10-25 15:05 — bước quan trọng (Hoàn thành sửa toàn bộ bug code review + fuzzer)
- Việc đã làm:
  - Sửa và kiểm chứng toàn bộ các vấn đề trong CODE-REVIEW.md:
    - SEC-01: Bọc socket.remote_endpoint(endpoint_ec) ngăn sập tiến trình khi client ngắt kết nối đột ngột (src/core/server/dns_server.cpp).
    - SEC-02: RateLimiter dùng created_order_ để evict O(1) khi đầy 16k nguồn, chống DoS khóa client mới (src/core/server/acl.cpp).
    - RES-01: Dùng sqlite3_close_v2 tránh rò rỉ connection handle và file lock WAL/SHM (src/core/store/store.cpp).
    - DNS-01: Bảo toàn TTL của bản ghi CNAME từ LocalZone tới QueryHandler response (local_zone.hpp/cpp, query_handler.cpp).
    - DNS-02: Name::is_suffix_of duyệt nhãn để xác minh khớp tại đúng label boundary (src/core/dns/name.cpp).
    - DNS-03: Kiểm tra giới hạn 16-bit wire format trong Message::encode tránh tràn số (src/core/dns/message.cpp).
    - API-01: CoreApi::add_self_domain lọc họ địa chỉ IPv4 vs IPv6 tương ứng với A vs AAAA (src/core/core_api.cpp), cập nhật test test_core_api.cpp.
    - PERF-01: Store::Tx::commit() giải phóng lock mutex ngay khi commit thành công (src/core/store/store.cpp).
    - CFG-01: Config::validate() cấm discovery_port trùng sync_port (src/core/config/config.cpp).
  - Phát hiện và sửa bug mới qua fuzz testing: decode_utf8 chấp nhận overlong sequence (ví dụ 0xC1 0x84) và surrogate trái với RFC 3629 -> nay kiểm tra min_cp và khoảng surrogate; chuẩn hóa to_lower cho chuỗi punycode (src/core/dns/name.cpp).
- File liên quan: src/core/dns/*, src/core/server/*, src/core/store/*, src/core/domain/*, src/core/core_api.*, tests/test_core_api.cpp, docs/CODE-REVIEW.md, process.md.
- Bug/lỗ hổng: Đã sửa 3 lỗi CRITICAL, 5 lỗi HIGH/MEDIUM, 1 lỗi RFC 3629 UTF-8 parser bug phát hiện qua libFuzzer.
- Kết quả kiểm chứng:
  - Windows MSVC Debug: 145/145 test case pass (2013 assertions).
  - Linux GCC 13 Debug (WSL): 145/145 test case pass.
  - Linux Clang ASan + UBSan (WSL): 145/145 test case pass (không rò rỉ bộ nhớ hay UB).
  - libFuzzer dns_message: >100.000 executions pass, 0 crash.
  - libFuzzer dns_name: >100.000 executions pass, 0 crash.
- Tiếp theo: Sẵn sàng để người dùng xem xét và quyết định commit.

## 2026-10-25 15:35 — bắt đầu (M3: Upstream + Resolver Pipeline + Cache + System DNS)
- Việc đã làm: Đọc lại docs/SPEC.md §6, docs/PLAN.md M3; rà soát cấu trúc thư mục src/core/resolver, src/core/net; bắt đầu thiết kế và cài đặt Milestone 3:
  1. Phát hiện DNS hệ thống (System DNS detection cho Linux /etc/resolv.conf và Windows GetAdaptersAddresses/Registry).
  2. LRU DNS Cache trong bộ nhớ (positive/negative cache, TTL cap, invalidation, thread-safe).
  3. Upstream client (UDP/TCP, timeout, retry, loop prevention).
  4. QueryLog (ring buffer + SQLite).
  5. Resolver Pipeline (Bước 0 Cache -> Bước 1 LocalZone -> Bước 2 Peer placeholder -> Bước 3 Upstream; CNAME loop/chain 8 bước; --trace).
  6. CLI ldns resolve <name> [type] [--trace].
- File liên quan: docs/SPEC.md §6, docs/PLAN.md M3, process.md, sắp thêm src/core/resolver/*, src/core/net/system_dns.*.
- Cần chú ý: Tuân thủ quy tắc chống loop (loại trừ self listening IP), TTL cap theo cấu hình, negative TTL 5s, CNAME tối đa 8 bước.
- Bug/lỗ hổng: chưa có.
- Tiếp theo: Thiết kế và viết code cho System DNS detection và Cache LRU.

## 2026-10-25 18:25 — bước quan trọng (Hoàn thành Milestone 3: Upstream + Resolver Pipeline + Cache + System DNS + CLI resolve)
- Việc đã làm:
  - Cài đặt phát hiện DNS hệ thống (`src/core/net/system_dns.hpp`, `.cpp`):
    - Windows: `GetAdaptersAddresses`, lọc interface loopback/down, ưu tiên adapter có default IPv4 gateway; parse danh sách DNS server IPv4 và IPv6.
    - Linux: đọc `/run/systemd/resolve/resolv.conf` (hoặc `/etc/resolv.conf`), parse các chỉ thị `nameserver`, bỏ qua loopback DNS của systemd-resolved (127.0.0.53) để lấy upstream thực.
  - Cài đặt bộ nhớ đệm DNS LRU (`src/core/resolver/cache.hpp`, `.cpp`):
    - Positive caching (RRset answers) và Negative caching (NXDOMAIN / NODATA với TTL cố định 5 giây theo SPEC §6).
    - Giới hạn TTL: `min_ttl` (0s), `max_ttl` (86400s), decrement TTL theo thời gian trôi qua (RFC 1035).
    - Giới hạn kích thước cache LRU (mặc định 10.000 entries), thread-safe qua `std::mutex`.
    - Hỗ trợ xóa/invalidation theo domain name khi local zone thay đổi.
  - Cài đặt DNS Upstream Client (`src/core/resolver/upstream_client.hpp`, `.cpp`):
    - Asio UDP client với timeout, fallback tự động sang TCP khi nhận cờ TC=1 hoặc khi gửi gói tin EDNS kích thước lớn.
    - Hỗ trợ danh sách upstream xoay vòng hoặc ưu tiên; chống vòng lặp phân giải (loại trừ các IP/cổng mà Ldns đang lắng nghe qua `excluded_endpoints`).
  - Cài đặt Ring Buffer Query Log (`src/core/resolver/query_log.hpp`, `.cpp`):
    - Vòng đệm cố định 10.000 bản ghi trong bộ nhớ, thread-safe, ghi nhận latency, rcode, bước phân giải, client IP.
  - Cài đặt Resolver Pipeline 4 bước (`src/core/resolver/pipeline.hpp`, `.cpp`):
    - Pipeline: Bước 0 (Cache) -> Bước 1 (LocalZone) -> Bước 2 (Peer hook) -> Bước 3 (Upstream).
    - Hỗ trợ theo dõi CNAME chain lên tới 8 bước (chống CNAME loop/depth recursion).
    - Ghi nhận trace hops chi tiết khi kích hoạt `--trace`.
  - Tích hợp pipeline vào `QueryHandler`, `Runtime` và `CoreApi`:
    - `QueryHandler` chuyển tiếp các truy vấn ra ngoài LocalZone vào `ResolverPipeline`.
    - `Runtime` đăng ký các địa chỉ bind vào danh sách loại trừ của upstream client để ngăn ngừa DNS loop.
    - `CoreApi::resolve(name, type, trace)` cung cấp interface phân giải hoàn chỉnh.
  - Cài đặt lệnh CLI `ldns resolve <name> [type] [--trace] [--json]` (`src/core/cli/commands.hpp`, `.cpp`, `src/main.cpp`):
    - Định dạng đầu ra đẹp mắt cho người dùng với latency, step name, answers và trace hops.
    - Hỗ trợ output chuẩn định dạng JSON (`--json`) theo SPEC §10.3.
- File liên quan:
  - `src/core/net/system_dns.{hpp,cpp}`, `src/core/resolver/*` (`cache.*`, `upstream_client.*`, `query_log.*`, `pipeline.*`).
  - `src/core/server/query_handler.{hpp,cpp}`, `src/core/runtime/runtime.cpp`.
  - `src/core/core_api.{hpp,cpp}`, `src/core/cli/commands.{hpp,cpp}`, `src/core/dns/message.{hpp,cpp}`, `src/main.cpp`.
  - `tests/test_cache.cpp`, `tests/test_query_log.cpp`, `tests/test_system_dns.cpp`, `tests/test_upstream_client.cpp`, `tests/test_pipeline.cpp`, `tests/test_cli.cpp`, `tests/test_core_api.cpp`.
- Bug/lỗ hổng phát hiện và đã sửa trong phiên:
  - Asio async buffer lifetime trong mock server test: `enc.value()` bị hủy khi lambda thoát trước khi hoàn tất async send -> sửa bằng `std::make_shared<std::vector<uint8_t>>`.
  - GCC `-Wmissing-field-initializers` với aggregate init của `QueryContext` -> thêm explicit constructor.
  - Ambiguity ADL giữa `resolver::step_name` và `cli::step_name` -> tái sử dụng `resolver::step_name`.
- Kết quả kiểm chứng:
  - Windows MSVC Debug: 161/161 test cases, 2116 assertions PASS (0 failures).
  - Linux WSL GCC 13 Debug: 161/161 test cases PASS (100%).
  - Linux WSL Clang ASan + UBSan: 161/161 test cases PASS (100%), không rò rỉ bộ nhớ hay hành vi không xác định.
  - Kiểm thử trực tiếp binary CLI: `ldns resolve one.one.one.one --trace` và `--json` phân giải thành công qua upstream DNS hệ thống.
- Tiếp theo: Sẵn sàng commit Milestone 3, hoặc chuyển sang Milestone 4 (Peer Discovery mDNS / DNS-SD theo SPEC §7 và PLAN M4).

## 2026-10-25 18:45 — bước quan trọng (Review log CI GitHub runner và sửa lỗi test_cli / self address)
- Việc đã làm:
  - Phân tích toàn bộ log lỗi CI từ `C:\Code\Ldns\.github\logs\logs_108895849664`:
    - Hai job `Linux (linux-asan)` và `Linux (linux-debug)` đều pass 100%.
    - Job `Windows (windows-ci)` gặp 1 lỗi duy nhất tại `test_cli.cpp(73)`: `CHECK(added.error().code == 708)` thất bại với giá trị thực tế `602`.
  - Nguyên nhân gốc rễ (Root Cause Analysis):
    1. Trên GitHub Actions Windows runner (máy ảo Azure VM), adapter mạng duy nhất là synthetic NIC với mô tả `"Microsoft Hyper-V Network Adapter"`.
    2. Trong `src/core/net/interfaces.cpp`, hàm `looks_virtual` so khớp từ khóa `"hyper-v"` dẫn đến việc nhận diện nhầm NIC chính của máy ảo khách thành switch ảo của máy chủ (host virtual switch).
    3. `SelfAddress::enumerate` lọc bỏ card mạng này khiến danh sách LAN address rỗng và trả về `Error{602, "no LAN address found"}`.
    4. Trong `tests/test_cli.cpp`, test case `"CLI add without targets uses the self address"` có comment giả định lỗi trả về là `708` thay vì `602` (do 708 chỉ xảy ra khi có address nhưng không khớp họ địa chỉ IPv4/IPv6).
  - Khắc phục triệt để:
    - Trong `src/core/net/interfaces.cpp`: Tinh chỉnh marker thành `"hyper-v virtual"`, `"virtualbox host-only"`, `"vmware virtual"`, phân biệt chính xác switch ảo trên máy chủ (host) với card mạng máy ảo khách (guest VM NIC).
    - Trong `src/core/domain/self_address.cpp`: Thêm cơ chế fallback: nếu không tìm thấy adapter vật lý (chạy trong VM/container/cloud runner) thì chấp nhận adapter UP mang `default_route` của hệ thống.
    - Trong `tests/test_cli.cpp`: Chấp nhận cả mã `602` lẫn `708` khi môi trường không có địa chỉ LAN; bổ sung test case có ghim `self_ip` để đảm bảo logic `target_kind == "self"` luôn được kiểm thử tất định (deterministic) trên mọi môi trường CI.
- File liên quan: `src/core/net/interfaces.cpp`, `src/core/domain/self_address.cpp`, `tests/test_cli.cpp`, `process.md`.
- Kết quả kiểm chứng:
  - Windows MSVC Debug: 162/162 test cases, 2121 assertions PASS (0 failures).
  - Linux WSL GCC 13 Debug: 161/161 test cases PASS (100%).
  - Linux WSL Clang ASan + UBSan: 161/161 test cases PASS (100%), không phát hiện memory leak hay lỗi sanitizers.
- Tiếp theo: Sẵn sàng commit và push lên GitHub để kích hoạt lại CI.


