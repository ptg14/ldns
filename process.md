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
