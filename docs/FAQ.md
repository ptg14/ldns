# Ldns — Các câu hỏi thường gặp (FAQ)

> Hướng dẫn xử lý các vấn đề mạng thực tế, xung đột cổng, tường lửa và khôi phục cấu hình hệ thống khi vận hành Ldns trên Windows và Linux.

---

## 1. Xung đột cổng 53 (Port 53 Conflicts)

### 1.1 Linux: `systemd-resolved` đang chiếm cổng 53
Trên Ubuntu 20.04+, Debian và Fedora, `systemd-resolved` thường lắng nghe sẵn trên `127.0.0.53:53`, khiến Ldns không thể bind vào cổng 53 (`0.0.0.0:53` hoặc `127.0.0.1:53`).

**Cách 1: Tắt DNSStubListener của systemd-resolved (Khuyến nghị khi chạy Ldns làm daemon chính)**
1. Mở `/etc/systemd/resolved.conf`:
   ```ini
   [Resolve]
   DNSStubListener=no
   ```
2. Tạo liên kết mềm cho `/etc/resolv.conf`:
   ```bash
   sudo ln -sf /run/systemd/resolve/resolv.conf /etc/resolv.conf
   sudo systemctl restart systemd-resolved
   ```
3. Khởi động lại dịch vụ Ldns:
   ```bash
   sudo systemctl restart ldns
   ```

**Cách 2: Cấu hình Ldns chỉ bind trên interface LAN và IP cụ thể**
Nếu muốn giữ `systemd-resolved`, chỉnh `config.toml` của Ldns:
```toml
[server]
listen = ["192.168.1.100"] # IP mạng LAN của máy bạn
```

---

### 1.2 Linux: Quyền bind cổng thấp (< 1024) không cần quyền root
Cổng 53 là privileged port. Thay vì chạy Ldns dưới quyền `root`, hãy cấp Linux Capability `CAP_NET_BIND_SERVICE`:
```bash
sudo setcap 'cap_net_bind_service=+ep' /usr/bin/ldns
```
*(Nếu cài đặt qua gói `.deb`, script `postinst` đã tự động thực hiện thao tác này)*.

---

### 1.3 Windows: Dịch vụ nào chiếm cổng 53?
Trên Windows, cổng 53 có thể bị chiếm bởi:
1. **Internet Connection Sharing (ICS)** hoặc SharedAccess.
2. **Hyper-V / WSL2 Host Network Service (HNS)**: dải cổng loại trừ ngẫu nhiên (`excludedportrange`).
3. **DNS Server Role** (trên Windows Server).

**Kiểm tra dịch vụ nào đang chiếm cổng:**
```powershell
Get-Process -Id (Get-NetUDPEndpoint -LocalPort 53).OwningProcess
```

**Khắc phục bằng lệnh tích hợp:**
Chạy công cụ chẩn đoán của Ldns:
```powershell
ldns doctor
```
Ldns sẽ phát hiện tiến trình xung đột và hướng dẫn chi tiết cách giải phóng hoặc chọn cổng thay thế.

---

## 2. Phát hiện node & Multicast (Discovery & Networking)

### 2.1 Wi-Fi Client Isolation (AP Isolation)
Trên một số mạng Wi-Fi công cộng, quán café hoặc router doanh nghiệp, tính năng **AP Isolation** (hoặc Client Isolation) được bật. Router sẽ ngăn các thiết bị Wi-Fi gửi gói multicast, broadcast hoặc kết nối trực tiếp với nhau.

**Dấu hiệu nhận biết:**
- Chạy `ldns peers` nhưng không thấy node nào khác dù cả hai máy đều đang bật Ldns.
- `ldns doctor` báo `multicast: blocked or no reflection`.

**Cách khắc phục:**
1. **Thêm peer thủ công bằng unicast:**
   ```bash
   ldns peers add 192.168.1.50:5391
   ```
   Hoặc cấu hình cố định trong `config.toml`:
   ```toml
   [network]
   manual_peers = ["192.168.1.50:5391"]
   ```
2. **Peer Exchange (PEX):**
   Chỉ cần một máy trong mạng kết nối được với peer thủ công, giao thức trao đổi peer sẽ tự động lan truyền danh sách các node còn lại cho nhau.

---

### 2.2 Đa card mạng (VPN, Docker, VirtualBox, WSL)
Nếu máy bạn có nhiều card mạng ảo (Docker bridge, Tailscale, WireGuard, vEthernet), multicast có thể bị gửi nhầm ra interface ảo thay vì LAN thật.

**Khắc phục:**
Chỉ định rõ interface LAN vật lý trong `config.toml`:
```toml
[network]
interfaces = "eth0"         # trên Linux (hoặc "enp3s0")
# interfaces = "Wi-Fi"      # trên Windows (hoặc "Ethernet")
```

---

## 3. Cấu hình tường lửa (Firewall)

Ldns sử dụng 3 cổng mạng LAN:
- **UDP / TCP 53**: Phục vụ truy vấn DNS chuẩn từ máy cục bộ và các thiết bị LAN.
- **UDP 5390**: Phát hiện node tự động (Multicast `239.76.68.83:5390` + Subnet Broadcast).
- **TCP 5391**: Kết nối phiên bảo mật và đồng bộ bản ghi giữa các node Ldns.

### 3.1 Linux (UFW)
```bash
sudo ufw allow in proto udp to any port 53 comment 'Ldns DNS UDP'
sudo ufw allow in proto tcp to any port 53 comment 'Ldns DNS TCP'
sudo ufw allow in proto udp to any port 5390 comment 'Ldns Discovery'
sudo ufw allow in proto tcp to any port 5391 comment 'Ldns Peer Sync'
```

### 3.2 Linux (firewalld)
```bash
sudo firewall-cmd --permanent --add-port=53/udp --add-port=53/tcp
sudo firewall-cmd --permanent --add-port=5390/udp --add-port=5391/tcp
sudo firewall-cmd --reload
```

### 3.3 Windows Defender Firewall (PowerShell với quyền Admin)
```powershell
New-NetFirewallRule -DisplayName "Ldns DNS Service" -Direction Inbound -Protocol UDP -LocalPort 53 -Action Allow
New-NetFirewallRule -DisplayName "Ldns DNS TCP" -Direction Inbound -Protocol TCP -LocalPort 53 -Action Allow
New-NetFirewallRule -DisplayName "Ldns Peer Discovery" -Direction Inbound -Protocol UDP -LocalPort 5390 -Action Allow
New-NetFirewallRule -DisplayName "Ldns Peer Sync" -Direction Inbound -Protocol TCP -LocalPort 5391 -Action Allow
```

---

## 4. An toàn cấu hình hệ thống & Khôi phục (Recovery)

### 4.1 "Tôi lỡ bật cấu hình DNS hệ thống nhưng Ldns dừng đột ngột, giờ máy mất mạng?"
Khi sử dụng tính năng điều hướng DNS của hệ điều hành (`ldns system setup`), Ldns tự động lưu bản sao lưu DNS gốc vào cơ sở dữ liệu bền vững.

Nếu Ldns bị crash hoặc dừng bất thường mà chưa kịp khôi phục:
```bash
ldns system restore
```
Lệnh này sẽ đọc bản sao lưu và trả DNS của card mạng về trạng thái ban đầu (DHCP hoặc DNS tĩnh cũ) ngay lập tức.

---

### 4.2 Vị trí tệp dữ liệu và sao lưu
Cơ sở dữ liệu Ldns lưu trữ toàn bộ bản ghi cục bộ, cache và trạng thái trong SQLite file ở chế độ WAL:
- **Linux (Service):** `/var/lib/ldns/ldns.db`
- **Linux (User):** `~/.local/share/ldns/ldns.db`
- **Windows (Service):** `C:\ProgramData\ldns\ldns.db`
- **Windows (User):** `%APPDATA%\ldns\ldns.db`

Để xuất toàn bộ bản ghi domain ra file backup đọc được:
```bash
ldns export backup-domains.toml
# hoặc xuất CSV:
ldns export backup-domains.csv
```
Khôi phục lại:
```bash
ldns import backup-domains.toml
```

---

## 5. Khóa mạng & Bảo mật (PSK)

### 5.1 Khi nào cần tạo khóa mạng?
Trong môi trường mạng mở (quán café, văn phòng chia sẻ, hội nghị), bất kỳ ai cùng mạng LAN đều có thể nhận beacon và kết nối vào cụm Ldns nếu không có khóa.
Bật Pre-Shared Key (PSK) sẽ kích hoạt:
1. Xác thực beacon bằng HMAC-SHA256 (bỏ qua mọi beacon lạ).
2. Mã hóa toàn bộ dữ liệu trao đổi bằng AEAD (XChaCha20-Poly1305).

**Cách bật:**
```bash
# 1. Tạo khóa
ldns network key generate cluster.key

# 2. Áp dụng
ldns network key set cluster.key

# 3. Sao chép cluster.key sang các máy bạn muốn kết nối và chạy:
ldns network key set cluster.key
```
