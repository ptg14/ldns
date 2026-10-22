# Ghi chú nền tảng (PLAN S2)

Kết quả kiểm chứng hành vi trên từng HĐH. Cập nhật mỗi khi spike/milestone chạm vào phần nền tảng.

## Môi trường phát triển

| | Windows | Linux |
|---|---|---|
| OS | Windows 11 Pro 10.0.26300 | Ubuntu 24.04.5 LTS (WSL2, kernel 6.6) |
| Compiler | MSVC 14.44 (VS 2022 17.14) | GCC 13 / Clang 18 |
| CMake | 4.1.1 | 3.28.3 |
| Generator | Visual Studio 17 2022 | Ninja 1.11.1 |
| vcpkg | `C:\vcpkg` | `~/vcpkg` (cùng baseline `67972e92`) |
| Triplet | `x64-windows-static` (CRT tĩnh `/MT`) | `x64-linux` |

### Ghi chú build

- Windows: `vcpkg.json` + preset `windows-msvc-*`; bắt buộc `CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded` để khớp triplet tĩnh.
- Linux/WSL: `scripts/wsl-build.sh` build trong `~/ldns-build/<preset>` (source ở `/mnt/c`, 9p chậm nên tách build dir).
- libsodium (vcpkg) cần `autoconf autoconf-archive automake libtool` trên Ubuntu mới.
- doctest + MSVC: include stream headers trước doctest (đã gói trong `tests/doctest.hpp`).

## Will be filled by spikes

- S2: bind `127.0.0.1:53` và IP LAN (systemd-resolved trên Ubuntu, ICS/Hyper-V/WSL trên Windows).
- S2: multicast `239.76.68.83:5390` theo interface (Wi-Fi/Ethernet cả hai HĐH).
- S2: nhận sự kiện đổi IP (netlink / `NotifyIpInterfaceChange`).
- S1: raw mode, VT input, renderer inline trên Windows Terminal, gnome-terminal, tmux, ssh.
