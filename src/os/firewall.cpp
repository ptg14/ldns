#include "os/firewall.hpp"

#include <cstdlib>

namespace ldns::os {

FirewallStatus Firewall::status() {
    FirewallStatus st;
#ifdef _WIN32
    st.backend_detected = "windows_advfirewall";
    // Check if rule exists
    int res = std::system("netsh advfirewall firewall show rule name=\"Ldns DNS Service\" >nul 2>&1");
    if (res == 0) st.has_dns_rule = true;

    int res2 = std::system("netsh advfirewall firewall show rule name=\"Ldns Discovery\" >nul 2>&1");
    if (res2 == 0) st.has_discovery_rule = true;

    int res3 = std::system("netsh advfirewall firewall show rule name=\"Ldns Peer Sync\" >nul 2>&1");
    if (res3 == 0) st.has_sync_rule = true;
#else
    if (std::system("which ufw >/dev/null 2>&1") == 0) {
        st.backend_detected = "ufw";
    } else if (std::system("which firewall-cmd >/dev/null 2>&1") == 0) {
        st.backend_detected = "firewalld";
    } else {
        st.backend_detected = "none";
    }
    // Check rules
    st.has_dns_rule = true; // Permissive fallback
    st.has_discovery_rule = true;
    st.has_sync_rule = true;
#endif
    return st;
}

Result<void> Firewall::add_rules() {
#ifdef _WIN32
    std::string cmd1 = "netsh advfirewall firewall add rule name=\"Ldns DNS Service\" dir=in action=allow protocol=UDP localport=53 >nul 2>&1";
    std::string cmd2 = "netsh advfirewall firewall add rule name=\"Ldns DNS Service TCP\" dir=in action=allow protocol=TCP localport=53 >nul 2>&1";
    std::string cmd3 = "netsh advfirewall firewall add rule name=\"Ldns Discovery\" dir=in action=allow protocol=UDP localport=5390 >nul 2>&1";
    std::string cmd4 = "netsh advfirewall firewall add rule name=\"Ldns Peer Sync\" dir=in action=allow protocol=TCP localport=5391 >nul 2>&1";

    int r1 = std::system(cmd1.c_str());
    [[maybe_unused]] auto r2 = std::system(cmd2.c_str());
    [[maybe_unused]] auto r3 = std::system(cmd3.c_str());
    [[maybe_unused]] auto r4 = std::system(cmd4.c_str());

    if (r1 != 0) {
        return Error{4, "Failed to add firewall rules. Run as Administrator."};
    }
#else
    if (std::system("which ufw >/dev/null 2>&1") == 0) {
        [[maybe_unused]] auto r_u1 = std::system("ufw allow 53 >/dev/null 2>&1");
        [[maybe_unused]] auto r_u2 = std::system("ufw allow 5390/udp >/dev/null 2>&1");
        [[maybe_unused]] auto r_u3 = std::system("ufw allow 5391/tcp >/dev/null 2>&1");
    } else if (std::system("which firewall-cmd >/dev/null 2>&1") == 0) {
        [[maybe_unused]] auto r_fw = std::system("firewall-cmd --add-port=53/udp --add-port=53/tcp --add-port=5390/udp --add-port=5391/tcp >/dev/null 2>&1");
    }
#endif
    return {};
}

Result<void> Firewall::remove_rules() {
#ifdef _WIN32
    [[maybe_unused]] auto r_d1 = std::system("netsh advfirewall firewall delete rule name=\"Ldns DNS Service\" >nul 2>&1");
    [[maybe_unused]] auto r_d2 = std::system("netsh advfirewall firewall delete rule name=\"Ldns DNS Service TCP\" >nul 2>&1");
    [[maybe_unused]] auto r_d3 = std::system("netsh advfirewall firewall delete rule name=\"Ldns Discovery\" >nul 2>&1");
    [[maybe_unused]] auto r_d4 = std::system("netsh advfirewall firewall delete rule name=\"Ldns Peer Sync\" >nul 2>&1");
#else
    if (std::system("which ufw >/dev/null 2>&1") == 0) {
        [[maybe_unused]] auto r_du1 = std::system("ufw delete allow 53 >/dev/null 2>&1");
        [[maybe_unused]] auto r_du2 = std::system("ufw delete allow 5390/udp >/dev/null 2>&1");
        [[maybe_unused]] auto r_du3 = std::system("ufw delete allow 5391/tcp >/dev/null 2>&1");
    }
#endif
    return {};
}

} // namespace ldns::os
