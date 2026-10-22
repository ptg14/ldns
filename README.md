# Ldns

Local DNS ngang hàng (peer-to-peer) cho mạng LAN — CLI/TUI, C++20, Windows + Linux.

- **R1** — Phân giải theo thứ tự: DNS của chính ứng dụng → DNS của peer trong mạng (bỏ phiếu, xếp hạng) → upstream.
- **R2** — Các node tự phát hiện nhau và đồng bộ domain theo thời gian thực.
- **R3** — Mỗi người dùng tạo không giới hạn domain trỏ về máy mình hoặc địa chỉ tự cấu hình.

Tài liệu nguồn sự thật: [`docs/SPEC.md`](docs/SPEC.md) · kế hoạch: [`docs/PLAN.md`](docs/PLAN.md) · giao thức peer: [`docs/protocol.md`](docs/protocol.md).

## Trạng thái

Đang ở **M0 — nền tảng repo & CI** (xem `docs/PLAN.md` §3). Mã nguồn hiện có khung `util/` và binary `ldns --version`.

## Build

Yêu cầu: CMake ≥ 3.25, compiler C++20 (MSVC 2022 / GCC 12+ / Clang 15+), vcpkg.

```bash
git clone https://github.com/microsoft/vcpkg.git C:/vcpkg   # Windows
C:/vcpkg/bootstrap-vcpkg.bat -disableMetrics

cmake --preset local-windows-msvc-debug    # Windows (CMakeUserPresets.json)
cmake --build --preset local-windows-msvc-debug
ctest --preset local-windows-msvc-debug
```

Linux: `export VCPKG_ROOT=/path/to/vcpkg`, rồi `cmake --preset linux-debug` / `linux-release` / `linux-asan`.

## Test

`ctest --preset <preset>`. Unit test dùng doctest; fuzz/sim/integration sẽ thêm theo milestone.
