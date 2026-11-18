#include <extension.cpp>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

// The USB ECM extension against fake services and a fake network: its reading of an adapter's
// descriptors (the RTL8153's real ones, and a different adapter's), DHCP, ARP, DNS, TCP and a
// chunked HTTP response, the JSON count, and the Record double-tap with its popup.
using namespace opxy;
namespace ecm = opxy::extensions::usb_ecm;
namespace {
#define CHECK(value) do { if (!(value)) return __LINE__; } while (false)

typedef std::vector<u8> Bytes;

Bytes unhex(const char* text) {
    Bytes out;
    for (; text[0] && text[1]; text += 2) out.push_back(static_cast<u8>(std::stoi(std::string(text, 2), nullptr, 16)));
    return out;
}

// What an adapter reports: its configuration descriptor (value 2), the string index and text of
// its MAC address, and its language.
struct Adapter {
    Bytes config;
    u32 mac_string;
    const char* mac;
    u32 language;
    u32 control_interface, data_interface, alternate, bulk_in, bulk_out, packet;
};

// The RTL8153's second configuration, as read from the device at high speed.
Adapter rtl8153() {
    return {unhex("09025000020200a05a090400000102060005052400100105"
                  "240600010d240f0300000000ea0500000007058303100008"
                  "09040100000a00000009040101020a000004070581020002"
                  "0007050202000200"),
            3, "A0CEC812D660", 0x0409, 0, 1, 1, 0x81, 0x02, 512};
}

// A full-speed ECM adapter laid out differently: other endpoint addresses, string index,
// packet size and language.
Adapter other() {
    Bytes c = unhex("09020000020200a0fa"                   // 2 interfaces, value 2 (length set below)
                    "090400000102060000"                   // control interface 0, class 02/06
                    "05240010010524060001"                 // CDC header and union
                    "0d240f0500000000ea05000000"           // Ethernet networking: MAC in string 5
                    "07058503100008"                       // interrupt IN 0x85
                    "09040100000a000000"                   // data interface 1, alternate 0, no endpoints
                    "09040101020a000000"                   // alternate 1, two endpoints
                    "07050102400000"                       // bulk OUT 0x01, 64 bytes
                    "07058202400000");                     // bulk IN 0x82, 64 bytes
    c[2] = static_cast<u8>(c.size());
    c[3] = static_cast<u8>(c.size() >> 8);
    return {c, 5, "001122334455", 0x0809, 0, 1, 1, 0x82, 0x01, 64};
}

Adapter vendor_only() {
    return {unhex("09022700010200a05a" "0904000003ffff0000" "07058102000200" "07050202000200" "07058303020008"),
            0, "", 0x0409, 0, 0, 0, 0, 0, 0};
}

Adapter* adapter;  // the device being faked

std::string log_text;
void log(const char* line) { log_text += line; log_text += '\n'; }
bool contains(const char* text) { return log_text.find(text) != std::string::npos; }

std::string calls;
u32 filter_value, filter_index;
bool bad_transfer;  // a frame or read the real stack would refuse

i32 control(const usb::Device&, u8 type, u8 request, u16 value, u16 index, void* data, u16 length) {
    u8* out = static_cast<u8*>(data);
    if (type == 0x80 && request == 6 && (value >> 8) == 2) {  // a configuration descriptor, by index
        Bytes d;
        if ((value & 0xff) == 0) d = unhex("09021900010100a05a");  // configuration 1, header only
        else if ((value & 0xff) == 1) d = adapter->config;
        else return -13;
        if (length > d.size()) length = static_cast<u16>(d.size());
        std::memcpy(out, d.data(), length);
        return length;
    }
    if (type == 0x80 && request == 6 && (value >> 8) == 3) {  // a string descriptor
        Bytes d;
        if ((value & 0xff) == 0) d = {4, 3, static_cast<u8>(adapter->language), static_cast<u8>(adapter->language >> 8)};
        else if ((value & 0xff) == adapter->mac_string && index == adapter->language) {
            d = {26, 3};
            for (u32 i = 0; i < 12; ++i) { d.push_back(static_cast<u8>(adapter->mac[i])); d.push_back(0); }
        } else return -13;
        if (length > d.size()) length = static_cast<u16>(d.size());
        std::memcpy(out, d.data(), length);
        return length;
    }
    if (type == 0x21 && request == 0x43) { filter_value = value; filter_index = index; calls += "F"; return 0; }
    return -13;
}
i32 set_interface(const usb::Device&, u8 interface, u8 alternate) {
    calls += "S" + std::to_string(interface) + std::to_string(alternate);
    return 0;
}

// --- The network: a router at 192.168.1.1 (DHCP, DNS, ARP) and a web server at 203.0.113.5 ---
const u8 router_mac[6] = {0x02, 0, 0, 0, 0, 1};
const u8 router_ip[4] = {192, 168, 1, 1}, client_ip[4] = {192, 168, 1, 21}, server_ip[4] = {203, 0, 113, 5};
const u8 broadcast_ip[4] = {255, 255, 255, 255};

void be16(Bytes& b, u32 v) { b.push_back(static_cast<u8>(v >> 8)); b.push_back(static_cast<u8>(v)); }
void be32(Bytes& b, u32 v) { be16(b, v >> 16); be16(b, v & 0xffff); }
void put(Bytes& b, const u8* p, u32 n) { b.insert(b.end(), p, p + n); }
u32 at16(const Bytes& b, u32 i) { return static_cast<u32>(b[i] << 8 | b[i + 1]); }
u32 at32(const Bytes& b, u32 i) { return at16(b, i) << 16 | at16(b, i + 2); }

Bytes ethernet(const u8* to, const u8* from, u32 type, const Bytes& payload) {
    Bytes f; put(f, to, 6); put(f, from, 6); be16(f, type); f.insert(f.end(), payload.begin(), payload.end());
    return f;
}
Bytes ipv4(u8 protocol, const u8* from, const u8* to, const Bytes& payload) {
    Bytes p = {0x45, 0};
    be16(p, 20 + static_cast<u32>(payload.size())); be16(p, 0); be16(p, 0);
    p.push_back(64); p.push_back(protocol); be16(p, 0);
    put(p, from, 4); put(p, to, 4);
    p.insert(p.end(), payload.begin(), payload.end());
    return p;
}
Bytes udp(u32 from, u32 to, const Bytes& payload) {
    Bytes p; be16(p, from); be16(p, to); be16(p, 8 + static_cast<u32>(payload.size())); be16(p, 0);
    p.insert(p.end(), payload.begin(), payload.end());
    return p;
}
Bytes tcp(u32 from, u32 to, u32 seq, u32 ack, u8 flags, const Bytes& payload) {
    Bytes p; be16(p, from); be16(p, to); be32(p, seq); be32(p, ack);
    p.push_back(0x50); p.push_back(flags); be16(p, 8192); be16(p, 0); be16(p, 0);
    p.insert(p.end(), payload.begin(), payload.end());
    return p;
}

// What the web server answers, and what to break.
enum Response { kUsers, kCatalog, kRedirect, kNoIds, kPlain };
Response response = kUsers;
bool silent_dhcp, silent_arp, silent_dns, silent_syn;
u32 response_bytes;       // the length of the last HTTP response, headers included
std::string last_request, last_dns_name;
std::deque<Bytes> inbound;
u8 client_mac[6];
u32 client_isn, server_next, client_next;
u32 out_frames, out_dhcp_types;

std::string body_for(Response kind) {
    switch (kind) {
        case kUsers: return "[{\"id\":1,\"name\":\"Leanne\",\"address\":{\"id\":9}},{\"id\":2,\"name\":\"Ervin\"},{\"id\":3,\"name\":\"Clementine\"}]";
        case kCatalog: return "{\"schema\":1,\"machines\":[{\"id\":\"fm2op\",\"kind\":\"synth\",\"keywords\":[\"fm\",\"bell\"],"
                              "\"cpu\":{\"id\":\"nested\",\"runs\":3}},{\"id\":\"macroosc\"},{\"id\":\"ringmod\"},{\"id\":\"pluck\"}],\"id\":\"top\"}";
        case kNoIds: return "[{\"name\":\"x\"},{\"name\":\"y\"}]";
        default: return "[{\"id\":7}]";
    }
}

std::string http_response(Response kind) {
    if (kind == kRedirect) return "HTTP/1.1 301 Moved Permanently\r\nLocation: https://example.test/users\r\nContent-Length: 0\r\n\r\n";
    const std::string body = body_for(kind);
    if (kind == kPlain) return "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
    // Chunked, in two chunks that split the JSON.
    const size_t cut = body.size() / 2;
    auto hex = [](size_t n) { char s[16]; std::snprintf(s, sizeof s, "%zx", n); return std::string(s); };
    return "HTTP/1.1 200 OK\r\nContent-Type: application/json; charset=utf-8\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n" +
           hex(cut) + "\r\n" + body.substr(0, cut) + "\r\n" + hex(body.size() - cut) + "\r\n" + body.substr(cut) + "\r\n0\r\n\r\n";
}

void dhcp_reply(const Bytes& f, u32 type) {
    const u32 d = 14 + 20 + 8;
    Bytes m = {2, 1, 6, 0}; m.insert(m.end(), f.begin() + d + 4, f.begin() + d + 8);  // xid
    be16(m, 0); be16(m, 0x8000);
    put(m, broadcast_ip, 0);  // ciaddr
    for (u32 i = 0; i < 4; ++i) m.push_back(0);
    put(m, client_ip, 4);     // yiaddr
    for (u32 i = 0; i < 8; ++i) m.push_back(0);
    m.insert(m.end(), f.begin() + d + 28, f.begin() + d + 34);  // chaddr
    while (m.size() < 236) m.push_back(0);
    be32(m, 0x63825363);
    m.push_back(53); m.push_back(1); m.push_back(static_cast<u8>(type));
    m.push_back(54); m.push_back(4); put(m, router_ip, 4);
    m.push_back(1); m.push_back(4); m.push_back(255); m.push_back(255); m.push_back(255); m.push_back(0);
    m.push_back(3); m.push_back(4); put(m, router_ip, 4);
    m.push_back(6); m.push_back(4); put(m, router_ip, 4);
    m.push_back(51); m.push_back(4); be32(m, 86400);
    m.push_back(255);
    inbound.push_back(ethernet(client_mac, router_mac, 0x0800, ipv4(17, router_ip, broadcast_ip, udp(67, 68, m))));
}

void dns_reply(const Bytes& f) {
    const u32 d = 14 + 20 + 8;
    u32 end = d + 12;
    std::string name;
    while (f[end]) { if (!name.empty()) name += '.'; name.append(f.begin() + end + 1, f.begin() + end + 1 + f[end]); end += 1 + f[end]; }
    last_dns_name = name;
    end += 1 + 4;
    Bytes m(f.begin() + d, f.begin() + d + 2);
    be16(m, 0x8180); be16(m, 1); be16(m, 1); be16(m, 0); be16(m, 0);
    m.insert(m.end(), f.begin() + d + 12, f.begin() + end);  // the question
    be16(m, 0xc00c); be16(m, 1); be16(m, 1); be32(m, 60); be16(m, 4); put(m, server_ip, 4);
    inbound.push_back(ethernet(client_mac, router_mac, 0x0800, ipv4(17, router_ip, client_ip, udp(53, at16(f, 14 + 20), m))));
}

void tcp_reply(u32 seq, u32 ack, u8 flags, const std::string& data = "") {
    inbound.push_back(ethernet(client_mac, router_mac, 0x0800,
                               ipv4(6, server_ip, client_ip, tcp(80, 49200, seq, ack, flags, Bytes(data.begin(), data.end())))));
}

void serve(const Bytes& f) {
    ++out_frames;
    std::memcpy(client_mac, f.data() + 6, 6);
    const u32 type = at16(f, 12);
    if (type == 0x0806 && at16(f, 20) == 1) {  // ARP who-has: the router owns every address
        if (silent_arp) return;
        Bytes a; be16(a, 1); be16(a, 0x0800); a.push_back(6); a.push_back(4); be16(a, 2);
        put(a, router_mac, 6); a.insert(a.end(), f.begin() + 38, f.begin() + 42);
        a.insert(a.end(), f.begin() + 22, f.begin() + 28); a.insert(a.end(), f.begin() + 28, f.begin() + 32);
        inbound.push_back(ethernet(client_mac, router_mac, 0x0806, a));
        return;
    }
    if (type != 0x0800) return;
    const u32 protocol = f[14 + 9];
    if (protocol == 17 && at16(f, 14 + 20 + 2) == 67) {  // DHCP
        const u32 message = f[14 + 20 + 8 + 240 + 2];
        out_dhcp_types = out_dhcp_types * 10 + message;
        if (!silent_dhcp) dhcp_reply(f, message == 1 ? 2 : 5);
    } else if (protocol == 17 && at16(f, 14 + 20 + 2) == 53) {
        if (!silent_dns) dns_reply(f);
    } else if (protocol == 6 && at16(f, 14 + 20 + 2) == 80) {
        const u32 t = 14 + 20, flags = f[t + 13], seq = at32(f, t + 4);
        const u32 size = (f[t + 12] >> 4) * 4, total = at16(f, 14 + 2), payload = total - 20 - size;
        if (flags & 0x02) {  // SYN
            if (silent_syn) return;
            client_isn = seq;
            server_next = 5000;
            tcp_reply(server_next, client_isn + 1, 0x12);
            server_next += 1;
        } else if (payload) {  // the request: answer in segments of 90 bytes, the last with FIN
            last_request.assign(f.begin() + t + size, f.begin() + t + size + payload);
            client_next = seq + payload;
            const std::string r = http_response(response);
            response_bytes = static_cast<u32>(r.size());
            for (size_t at = 0; at < r.size(); at += 90) {
                const std::string piece = r.substr(at, 90);
                const bool last = at + 90 >= r.size();
                tcp_reply(server_next, client_next, last ? 0x19 : 0x18, piece);
                server_next += static_cast<u32>(piece.size());
            }
        }
    }
}

i32 transfer(const usb::Device&, u8 endpoint, u8 type, void* data, u16 length, u32) {
    if (type != 2) return -22;
    if (endpoint == adapter->bulk_out) {
        if (length % adapter->packet == 0) bad_transfer = true;  // would need a zero-length packet
        Bytes f(static_cast<u8*>(data), static_cast<u8*>(data) + length);
        serve(f);
        return length;
    }
    if (endpoint != adapter->bulk_in || length % adapter->packet != 0) { bad_transfer = true; return -22; }
    if (inbound.empty()) return -14;
    const Bytes f = inbound.front();
    inbound.pop_front();
    std::memcpy(data, f.data(), f.size() < length ? f.size() : length);
    return static_cast<i32>(f.size());
}

// --- The driver, through the event and the stack's probe ---
const usb::Driver* added[2];
u32 added_count;
bool add(const usb::Driver& driver) { added[added_count++] = &driver; return true; }

std::vector<std::string> popups;
void notify(const char* text) { popups.push_back(text); }
extensions::Time clock_us;

// Record pressed and released twice within the double-tap windows.
void double_tap() {
    const extensions::Hooks& hooks = extensions::usb_ecm::hooks;
    const extensions::Actions actions = {nullptr, notify, nullptr};
    const extensions::Context context = {};
    clock_us += 10000000;
    const extensions::Time times[4] = {0, 100000, 200000, 300000};
    for (u32 i = 0; i < 4; ++i)
        hooks.queued_control(context, {extensions::keys::record, i % 2 == 0, true, true, clock_us + times[i]}, actions);
}

usb::Device device() {
    usb::Device d = {};
    d.speed = 3; d.address = 1; d.configuration_count = 2; d.configuration_value = 2; d.handle = 0x2000;
    return d;
}

void reset_network() {
    inbound.clear();
    silent_dhcp = silent_arp = silent_dns = silent_syn = false;
    response = kUsers;
    out_frames = out_dhcp_types = 0;
    log_text.clear();
    calls.clear();
    popups.clear();
}

// Brings an adapter up through the driver's probes and leaves it leased.
int bring_up(Adapter& which, const usb::Driver& driver, const usb::Services& services) {
    adapter = &which;
    reset_network();
    CHECK(driver.probe(services, device(), which.control_interface) == which.control_interface + 1);
    CHECK(calls.empty());  // the control interface is claimed and nothing is sent
    CHECK(driver.probe(services, device(), which.data_interface) == which.data_interface + 1);
    return 0;
}
}

int main() {
    const extensions::Hooks& hooks = extensions::usb_ecm::hooks;
    CHECK(hooks.usb_host_ready && hooks.queued_control && !hooks.context);
    const usb::Registry registry = {add};
    hooks.usb_host_ready(registry);
    CHECK(added_count == 1);
    const usb::Driver& driver = *added[0];
    CHECK(std::strcmp(driver.name, "usb-ecm") == 0 && driver.probe && driver.remove);

    // It matches by class, never by vendor or product: any ECM adapter.
    CHECK(driver.matches[0].flags == (usb::match::interface_class | usb::match::interface_subclass));
    CHECK(driver.matches[0].interface_class == 2 && driver.matches[0].interface_subclass == 6 && driver.matches[0].vendor_id == 0);
    CHECK(driver.matches[1].flags == usb::match::interface_class && driver.matches[1].interface_class == 0x0a);
    CHECK(driver.matches[2].flags == 0);

    const usb::Services services = {log, control, set_interface, transfer};

    // The RTL8153's real descriptors give its layout: interface 0 and 1, alternate 1, bulk
    // 0x81 and 0x02 of 512 bytes, the MAC in string 3.
    Adapter a = rtl8153();
    std::memcpy(ecm::config, a.config.data(), a.config.size());
    ecm::config_length = static_cast<u32>(a.config.size());
    ecm::parse_config();
    CHECK(ecm::link.complete() && ecm::link.control_interface == 0 && ecm::link.data_interface == 1 && ecm::link.alternate == 1);
    CHECK(ecm::link.bulk_in == 0x81 && ecm::link.bulk_out == 0x02 && ecm::link.packet == 512 && ecm::link.mac_string == 3);

    // Another adapter's differ, and the driver follows them.
    Adapter b = other();
    std::memcpy(ecm::config, b.config.data(), b.config.size());
    ecm::config_length = static_cast<u32>(b.config.size());
    ecm::parse_config();
    CHECK(ecm::link.complete() && ecm::link.bulk_in == 0x82 && ecm::link.bulk_out == 0x01 && ecm::link.packet == 64 && ecm::link.mac_string == 5);

    // A vendor-mode configuration is declined, so the stack goes on to the next one.
    Adapter v = vendor_only();
    adapter = &v;
    reset_network();
    CHECK(driver.probe(services, device(), 0) == 0);
    CHECK(contains("not a CDC-ECM adapter"));
    driver.remove(services, device(), 0);

    // The RTL8153 end to end: probes, SET_INTERFACE on its data interface and alternate, the
    // packet filter on its control interface, its MAC, DHCP.
    CHECK(bring_up(a, driver, services) == 0);
    CHECK(calls == "S11F" && filter_value == 0x0c && filter_index == 0);
    CHECK(contains("ecm link control=0 data=1 alt=1 in=0x81 out=0x02 packet=512 mac-string=3"));
    CHECK(contains("ecm mac a0cec812d660"));
    CHECK(contains("ecm dhcp bound ip=192.168.1.21 server=192.168.1.1 lease=86400"));
    CHECK(contains("ecm dhcp mask=255.255.255.0 router=192.168.1.1 dns=192.168.1.1"));
    CHECK(out_dhcp_types == 13 && !bad_transfer);

    // A double-tap on Record reads the page: ARP, DNS, TCP, a chunked response split across
    // segments, the JSON count and the popup with the byte count.
    popups.clear();
    log_text.clear();
    double_tap();
    CHECK(last_dns_name == "jsonplaceholder.typicode.com");
    CHECK(last_request.find("GET /users HTTP/1.1\r\n") == 0 && last_request.find("Host: jsonplaceholder.typicode.com\r\n") != std::string::npos);
    CHECK(contains("ecm dns jsonplaceholder.typicode.com=203.0.113.5") && contains("ecm tcp connected"));
    CHECK(contains("ecm http HTTP/1.1 200 OK") && contains("chunked complete"));
    CHECK(contains("ecm ids: 1 2 3") && contains("ecm json elements=3 ids found=3"));
    CHECK(popups.size() == 1 && popups[0] == "3 ids, " + std::to_string(response_bytes) + " bytes");
    CHECK(!bad_transfer);

    // The catalog's shape: the array under "machines", only each entry's own id (not a nested
    // one, nor the top-level one).
    response = kCatalog;
    inbound.clear();
    log_text.clear();
    CHECK(ecm::http_get(ecm::kTargets[0]) == ecm::kOk);
    CHECK(last_request.find("GET /machines.json HTTP/1.1\r\n") == 0 && last_request.find("Host: open-xy.com\r\n") != std::string::npos);
    CHECK(ecm::id_count == 4 && ecm::element_count == 4);
    CHECK(contains("ecm ids: fm2op macroosc ringmod pluck") && contains("ecm json elements=4 ids found=4"));

    // A plain (Content-Length) response works too.
    response = kPlain;
    inbound.clear();
    popups.clear();
    double_tap();
    CHECK(popups.size() == 1 && popups[0] == "1 ids, " + std::to_string(response_bytes) + " bytes");

    // What goes wrong is said in the popup.
    response = kRedirect;
    inbound.clear(); popups.clear(); log_text.clear();
    double_tap();
    CHECK(popups.size() == 1 && popups[0] == "HTTP status 301" && contains("ecm http location https://example.test/users"));
    response = kNoIds;
    inbound.clear(); popups.clear();
    double_tap();
    CHECK(popups.size() == 1 && popups[0] == "No ids in the response");
    response = kUsers;
    silent_syn = true;
    inbound.clear(); popups.clear();
    double_tap();
    CHECK(popups.size() == 1 && popups[0] == "Could not connect");
    silent_syn = false;
    silent_dns = true;
    inbound.clear(); popups.clear();
    double_tap();
    CHECK(popups.size() == 1 && popups[0] == "DNS lookup failed");
    silent_dns = false;
    // A tap that is not a double-tap does nothing.
    popups.clear();
    {
        const extensions::Actions actions = {nullptr, notify, nullptr};
        const extensions::Context context = {};
        clock_us += 10000000;
        hooks.queued_control(context, {extensions::keys::record, true, true, true, clock_us}, actions);
        hooks.queued_control(context, {extensions::keys::record, false, true, true, clock_us + 100000}, actions);
        hooks.queued_control(context, {7, true, true, true, clock_us + 150000}, actions);
        hooks.queued_control(context, {extensions::keys::record, true, true, true, clock_us + 200000}, actions);
        hooks.queued_control(context, {extensions::keys::record, false, true, true, clock_us + 300000}, actions);
    }
    CHECK(popups.empty());

    // No lease: the key tries DHCP again and says so when nothing answers.
    ecm::leased = false;
    silent_dhcp = true;
    inbound.clear(); popups.clear();
    double_tap();
    CHECK(popups.size() == 1 && popups[0] == "No DHCP lease");
    silent_dhcp = false;
    ecm::leased = true;

    // Nothing answers ARP: the router cannot be reached.
    silent_arp = true;
    for (u32 i = 0; i < 4; ++i) ecm::neighbors[i].valid = false;
    inbound.clear(); popups.clear();
    double_tap();
    CHECK(popups.size() == 1 && popups[0] == "DNS lookup failed");
    silent_arp = false;

    // The adapter is removed: the key says there is none.
    driver.remove(services, device(), 2);
    popups.clear();
    double_tap();
    CHECK(popups.size() == 1 && popups[0] == "No USB Ethernet adapter");

    // The other adapter, with nothing in common but the class: its endpoints, packet size,
    // string index and language drive everything, and every frame sent is accepted by its
    // 64-byte packet rule.
    CHECK(bring_up(b, driver, services) == 0);
    CHECK(calls == "S11F" && filter_index == 0);
    CHECK(contains("ecm link control=0 data=1 alt=1 in=0x82 out=0x01 packet=64 mac-string=5"));
    CHECK(contains("ecm dhcp bound ip=192.168.1.21"));
    popups.clear();
    double_tap();
    CHECK(popups.size() == 1 && popups[0] == "3 ids, " + std::to_string(response_bytes) + " bytes");
    CHECK(!bad_transfer);

    // A stack without the services leaves the driver quiet: it declines.
    driver.remove(services, device(), 2);
    const usb::Services bare = {log};
    CHECK(driver.probe(bare, device(), 1) == 0);
    std::puts("ok");
    return 0;
}
