# Quy tắc làm việc cho agent — dự án Ldns

Áp dụng cho mọi phiên làm việc trong repo này.

## 1. Tuân thủ tài liệu

- Đọc tài liệu liên quan **trước khi** thiết kế hay sửa code: `docs/SPEC.md`, `docs/PLAN.md`, `docs/protocol.md`.
- `SPEC.md` là **nguồn sự thật**; `protocol.md` chỉ bổ sung cho SPEC. Khi tài liệu và code lệch nhau, dừng lại, báo người dùng và sửa tài liệu trước rồi mới sửa code.
- Không tự ý thay đổi hành vi đã được đặc tả. Mọi sai lệch so với SPEC/PLAN phải được nêu rõ và được người dùng đồng ý.

## 2. Commit

- **Không tự ý commit hay push.** Không bao giờ chạy `git commit`, `git push`, `git reset --hard`… khi chưa được yêu cầu.
- Khi công việc đã sẵn sàng commit: liệt kê file thay đổi + **viết sẵn commit message** đề xuất, rồi để **người dùng quyết định** có commit hay không.
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

## 5. Nhật ký làm việc — `process.md`

- Cập nhật `process.md` khi: **bắt đầu** phiên/công việc, **kết thúc**, và tại **các bước quan trọng** (hoàn thành milestone, thay đổi thiết kế, phát hiện bug/lỗ hổng).
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
