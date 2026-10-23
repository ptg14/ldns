# Quy tắc làm việc cho agent — dự án Ldns

Áp dụng cho mọi phiên làm việc trong repo này.

## 1. Tuân thủ tài liệu

- Đọc tài liệu liên quan **trước khi** thiết kế hay sửa code: `docs/SPEC.md`, `docs/PLAN.md`, `docs/protocol.md`.
- `SPEC.md` là **nguồn sự thật**; `protocol.md` chỉ bổ sung cho SPEC. Khi tài liệu và code lệch nhau, dừng lại, báo người dùng và sửa tài liệu trước rồi mới sửa code.
- Không tự ý thay đổi hành vi đã được đặc tả. Mọi sai lệch so với SPEC/PLAN phải được nêu rõ và được người dùng đồng ý.

## 2. Commit

- **Không tự ý commit hay push.** Không bao giờ chạy `git commit`, `git push`, `git reset --hard`… khi chưa được yêu cầu.
- **Trước khi đề xuất commit, bắt buộc phải test và review code** (xem §2.1). Không đề xuất commit dựa trên code chưa được kiểm chứng.
- Khi công việc đã sẵn sàng commit: liệt kê file thay đổi + **viết sẵn commit message** đề xuất, rồi để **người dùng quyết định** có commit hay không.

### 2.1 Test và review bắt buộc trước khi đề xuất commit

1. **Build + test thật trên cả hai nền tảng**, không chỉ một máy:
   - Windows: `cmake --preset local-windows-msvc-debug` → build → `ctest --preset local-windows-msvc-debug`.
   - Linux (WSL): `scripts/wsl-build.sh test linux-debug` và `scripts/wsl-build.sh test linux-asan`.
   - Nếu thay đổi chạm parser nhận dữ liệu từ mạng: chạy thêm `scripts/wsl-fuzz.sh` cho harness liên quan.
   - Build/test phải **xanh**; nếu đỏ thì sửa trước, không đề xuất commit kèm lỗi đã biết (trừ khi người dùng yêu cầu rõ).
2. **Review code** phần thay đổi (tự review hoặc dùng quy trình review của repo):
   - đúng SPEC/PLAN/protocol;
   - lỗi biên, tràn, vòng đời/ownership, đa luồng, tài nguyên;
   - bảo mật theo §4 (validate đầu vào mạng, không lộ bí mật, giới hạn kích thước);
   - test bao phủ hành vi mới và **mỗi bug sửa được kèm test tái hiện**.
3. Ghi kết quả test/review (nền tảng nào, pass/fail, bug đã sửa) vào `process.md` **trước** khi đưa ra commit message đề xuất.
4. Commit message đề xuất chỉ được đưa ra sau khi hoàn tất 1–3.
- Chỉ commit đúng phạm vi thay đổi của nhiệm vụ; không gộp thay đổi ngoài lề.

## 3. Kiểm tra ảnh hưởng khi viết code

- Trước khi sửa: tìm mọi nơi sử dụng (hàm, kiểu, hằng số, định dạng dữ liệu, giao thức, file cấu hình) sẽ bị ảnh hưởng — cả trong repo lẫn tài liệu.
- Kiểm tra ảnh hưởng tới: `src/core/*` (dùng chung), `src/ipc/`, wire format trong `docs/protocol.md`, lưu trữ trên đĩa, và tương thích Windows/Linux.
- Sau khi sửa: build được, chạy test liên quan (`tests/`), và ghi lại các ảnh hưởng đã kiểm tra vào `process.md`.
- Nếu thay đổi chạm vào giao thức hoặc định dạng lưu trữ, phải nêu rõ vấn đề tương thích ngược.

## 4. Bảo mật

- Luôn áp dụng yêu cầu bảo mật của dự án (SPEC §14, PLAN M9): xác thực peer, PSK, ACL, rate limit, giới hạn kích thước gói, kiểm tra dữ liệu từ mạng LAN là **không tin cậy**.
- Không hard-code bí mật, không log khoá/PSK/token; không đưa bí mật vào commit.
- Mọi đầu vào từ mạng (DNS packet, message peer, IPC) phải được validate biên và chống tràn/khử tuần tự độc hại.
- Khi phát hiện lỗ hổng hay nghi vấn bảo mật trong lúc làm: **ghi ngay vào `process.md`** và báo người dùng, không tự ý bỏ qua.

## 5. Nhật ký làm việc — `process.md` & `process-ui.md`

- Với công việc Core / Backend / Giao thức / CI: cập nhật vào `process.md`.
- **Với công việc UX / UI:** tuyệt đối **không ghi vào `process.md`** mà ghi riêng vào `process-ui.md` để tránh xung đột git giữa các luồng làm việc.
- Cập nhật nhật ký khi: **bắt đầu** phiên/công việc, **kết thúc**, và tại **các bước quan trọng** (thay đổi thiết kế, sửa bug, hoàn thành nhóm chức năng).
- Mỗi mục ghi: thời điểm, việc đã làm, file liên quan, điều cần chú ý, bug/lỗ hổng phát hiện (kèm mức độ), việc tiếp theo.
- Ghi ngắn gọn, có thể tra cứu; không thay thế commit message hay tài liệu thiết kế.

### Định dạng mục nhật ký

```markdown
## YYYY-MM-DD HH:MM — <loại: bắt đầu | kết thúc | bước quan trọng>
- Việc đã làm: ...
- File liên quan: ...
- Cần chú ý: ...
- Bug/lỗ hổng: ... (mức độ: thấp/trung bình/cao)
- Tiếp theo: ...
```

---

## 6. Quy tắc riêng cho phân hệ UX / UI

1. **Phạm vi chuyên biệt:** Chỉ tập trung vào thiết kế và triển khai UX/UI (inline TUI, widgets, tương tác terminal, hiển thị bảng màu, phím tắt). **Không can thiệp quá sâu vào logic vận hành core** (DNS server, SQLite store, resolver pipeline, giao thức đồng bộ peer).
2. **Nhánh làm việc riêng:** Mọi công việc về giao diện phải được thực hiện trên nhánh riêng về UI (ví dụ: `feat/ux-ui`), không làm trực tiếp trên nhánh `main`.
3. **Quy trình báo cáo kèm danh mục Test (User là Tester):** Mỗi khi báo cáo kết quả phải đi kèm **checklist kiểm thử UX/UI chi tiết** (duy trì tại `UI_CHECKLIST.md` và `docs/UI-SPEC.md`) để người dùng trực tiếp tự kiểm thử trong vai trò tester.
4. **Nhật ký phân hệ UI tách biệt:** Toàn bộ lịch sử tiến độ, phân tích lỗi giao diện và cập nhật khắc phục của phân hệ UI được ghi vào `process-ui.md`, không ghi vào `process.md`.
5. **Theo dõi lỗi giao diện:** Các lỗi phát hiện trong quá trình test UX/UI phải được ghi nhận và theo dõi trạng thái cụ thể trong bảng Bug Tracker của `UI_CHECKLIST.md`.
