#include <opxy/double_tap.hpp>
#include <opxy/extension.hpp>

// USB ECM: a CDC-ECM host driver for an Ethernet adapter (tried on an RTL8153 in its ECM
// configuration). At boot it reads the adapter's descriptors, claims the ECM control and data
// interfaces, selects the data interface's alternate setting, enables directed and broadcast
// frames, reads the MAC address and runs DHCP; a double-tap on Record then reads one JSON file
// over plain HTTP (ARP, DNS, TCP), counts the ids in an array of it and shows the result in a
// popup. Every step is logged to the device's log. Experimental.
namespace opxy { namespace extensions { namespace usb_ecm {
namespace {
// One log line, built without a C library.
struct Line {
    char text[120];
    u32 length;
    void add(const char* s) { while (*s && length < sizeof text - 1) text[length++] = *s++; }
    void hex(u32 value, u32 digits) {
        for (u32 i = digits; i > 0; --i) {
            const u32 nibble = (value >> (4 * (i - 1))) & 0xf;
            if (length < sizeof text - 1) text[length++] = static_cast<char>(nibble < 10 ? '0' + nibble : 'a' + nibble - 10);
        }
    }
    void number(i32 value) {
        if (value < 0) { add("-"); value = -value; }
        char digits[10];
        u32 count = 0;
        do { digits[count++] = static_cast<char>('0' + value % 10); value /= 10; } while (value);
        while (count && length < sizeof text - 1) text[length++] = digits[--count];
    }
    void ip(const u8* address) {
        for (u32 i = 0; i < 4; ++i) { if (i) add("."); number(address[i]); }
    }
    const char* done() { text[length] = 0; return text; }
};

void step(const usb::Services& services, const char* name, i32 status) {
    Line line = {{}, 0};
    line.add("ecm ");
    line.add(name);
    line.add(" ret=");
    line.number(status);
    services.log(line.done());
}

const u16 kFilterDirectedBroadcast = 0x000c;  // directed (bit 2) and broadcast (bit 3)
const u32 kXid = 0x4f505859;                  // "OPXY"
const u32 kCookie = 0x63825363;               // the DHCP magic cookie

// The host stack's task has a small stack: everything big is static.
alignas(32) u8 tx[512];
alignas(32) u8 rx[1536];  // a whole frame (at most 1514 bytes) in whole bulk packets of 64 or 512 bytes
alignas(32) u8 desc[64];
alignas(32) u8 config[512];
u8 mac[6];

// What the adapter's own descriptors say: no interface number, alternate setting, endpoint,
// packet size or string index is assumed.
struct Link {
    u8 control_interface, data_interface, alternate;  // the ECM control interface; the data interface's
                                                       // first alternate setting that has endpoints
    u8 bulk_in, bulk_out;                             // that setting's bulk endpoint addresses
    u8 mac_string;                                    // iMACAddress of the Ethernet networking descriptor
    u16 packet;                                       // the bulk wMaxPacketSize
    bool control_found, data_found, mac_found;
    bool complete() const { return control_found && data_found && bulk_in && bulk_out && packet && mac_found; }
};
Link link;
u32 config_length;
u32 rx_length = sizeof rx;  // a multiple of the bulk packet size

struct Lease {
    u8 ip[4], server[4], mask[4], router[4], dns[4];
    u32 seconds;
};
Lease lease;

void put16(u8* p, u32 v) { p[0] = static_cast<u8>(v >> 8); p[1] = static_cast<u8>(v); }
void put32(u8* p, u32 v) { put16(p, v >> 16); put16(p + 2, v & 0xffff); }
u32 get16(const u8* p) { return static_cast<u32>(p[0] << 8 | p[1]); }
u32 get32(const u8* p) { return get16(p) << 16 | get16(p + 2); }
void copy(u8* to, const u8* from, u32 n) { for (u32 i = 0; i < n; ++i) to[i] = from[i]; }
void fill(u8* to, u8 value, u32 n) { for (u32 i = 0; i < n; ++i) to[i] = value; }
bool same(const u8* a, const u8* b, u32 n) { for (u32 i = 0; i < n; ++i) if (a[i] != b[i]) return false; return true; }

u32 checksum(const u8* p, u32 n) {
    u32 sum = 0;
    for (u32 i = 0; i + 1 < n; i += 2) sum += get16(p + i);
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return ~sum & 0xffff;
}

u32 hex_digit(u8 c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : c >= 'a' && c <= 'f' ? c - 'a' + 10 : 16; }

// The descriptor of the configuration being tried, in `config`.
bool read_config(const usb::Services& services, const usb::Device& device) {
    for (u32 index = 0; index < device.configuration_count; ++index) {
        if (services.control(device, 0x80, 6, static_cast<u16>(0x0200 | index), 0, config, 9) < 0) return false;
        if (config[5] != device.configuration_value) continue;
        config_length = config[2] | config[3] << 8;
        if (config_length > sizeof config) config_length = sizeof config;
        return config_length >= 9 &&
               services.control(device, 0x80, 6, static_cast<u16>(0x0200 | index), 0, config, static_cast<u16>(config_length)) >= 0;
    }
    return false;
}

// Reads `link` from the descriptors in config[0..config_length): the ECM control interface
// (class 02, subclass 06), the first alternate setting of a data interface (class 0a) that has
// endpoints and its bulk endpoints, and the string index of the MAC address (the Ethernet
// networking functional descriptor).
void parse_config() {
    link = Link();
    bool in_data = false;
    for (u32 i = 0; i + 2 <= config_length && config[i] >= 2 && i + config[i] <= config_length; i += config[i]) {
        const u8* d = config + i;
        const u32 size = d[0];
        if (d[1] == 4 && size >= 9) {  // interface
            if (d[5] == 0x02 && d[6] == 0x06 && !link.control_found) { link.control_interface = d[2]; link.control_found = true; }
            in_data = d[5] == 0x0a && d[4] >= 2 && !link.data_found;
            if (in_data) { link.data_interface = d[2]; link.alternate = d[3]; link.data_found = true; }
        } else if (d[1] == 0x24 && size >= 5 && d[2] == 0x0f) {  // Ethernet networking
            link.mac_string = d[3];
            link.mac_found = d[3] != 0;
        } else if (d[1] == 5 && size >= 7 && in_data && (d[3] & 3) == 2) {  // a bulk endpoint of that setting
            if (d[2] & 0x80) link.bulk_in = d[2]; else link.bulk_out = d[2];
            link.packet = static_cast<u16>(d[4] | d[5] << 8);
        }
    }
}

// The adapter's MAC address: its string descriptor, 12 hex digits in UTF-16, in the first
// language the adapter lists.
bool read_mac(const usb::Services& services, const usb::Device& device) {
    u32 language = 0x0409;
    if (services.control(device, 0x80, 6, 0x0300, 0, desc, 4) >= 4 && desc[0] >= 4) language = desc[2] | desc[3] << 8;
    const u16 value = static_cast<u16>(0x0300 | link.mac_string);
    i32 status = services.control(device, 0x80, 6, value, static_cast<u16>(language), desc, 2);
    u32 length = status < 0 ? 0 : desc[0];
    if (length > sizeof desc) length = sizeof desc;
    if (length >= 26) status = services.control(device, 0x80, 6, value, static_cast<u16>(language), desc, static_cast<u16>(length));
    bool good = length >= 26 && status >= 0;
    for (u32 i = 0; good && i < 6; ++i) {
        const u32 high = hex_digit(desc[2 + 4 * i]), low = hex_digit(desc[4 + 4 * i]);
        good = high < 16 && low < 16;
        mac[i] = static_cast<u8>(high << 4 | low);
    }
    Line line = {{}, 0};
    line.add("ecm mac ");
    if (good) for (u32 i = 0; i < 6; ++i) line.hex(mac[i], 2); else line.add("unreadable");
    services.log(line.done());
    return good;
}

// A DHCP DISCOVER (type 1) or REQUEST (type 3, for the offered address) as a broadcast frame
// in `tx`; returns its length.
u32 build_dhcp(u8 type) {
    fill(tx, 0, sizeof tx);
    fill(tx, 0xff, 6);
    copy(tx + 6, mac, 6);
    put16(tx + 12, 0x0800);
    u8* ip = tx + 14;
    u8* udp = ip + 20;
    u8* d = udp + 8;
    d[0] = 1; d[1] = 1; d[2] = 6;  // BOOTREQUEST, Ethernet, 6-byte address
    put32(d + 4, kXid);
    put16(d + 10, 0x8000);         // ask for a broadcast reply: there is no address yet
    copy(d + 28, mac, 6);
    put32(d + 236, kCookie);
    u8* o = d + 240;
    *o++ = 53; *o++ = 1; *o++ = type;
    *o++ = 61; *o++ = 7; *o++ = 1; copy(o, mac, 6); o += 6;
    if (type == 3) {
        *o++ = 50; *o++ = 4; copy(o, lease.ip, 4); o += 4;
        *o++ = 54; *o++ = 4; copy(o, lease.server, 4); o += 4;
    }
    *o++ = 55; *o++ = 4; *o++ = 1; *o++ = 3; *o++ = 6; *o++ = 51;  // mask, router, DNS, lease time
    *o++ = 255;
    const u32 udp_length = 8 + static_cast<u32>(o - d);
    put16(udp, 68); put16(udp + 2, 67); put16(udp + 4, udp_length);  // UDP checksum 0: none
    ip[0] = 0x45; put16(ip + 2, 20 + udp_length); ip[8] = 64; ip[9] = 17;
    fill(ip + 16, 0xff, 4);        // from 0.0.0.0 to 255.255.255.255
    put16(ip + 10, checksum(ip, 20));
    return 14 + 20 + udp_length;
}

// The DHCP message type of a reply to our request in rx[0..n), or 0; fills `lease`.
u32 parse_dhcp(i32 n) {
    if (n < 14 + 20 + 8 + 240 || get16(rx + 12) != 0x0800 || (rx[14] >> 4) != 4) return 0;
    const u32 header = static_cast<u32>(rx[14] & 0xf) * 4;
    if (header < 20 || rx[14 + 9] != 17 || 14 + header + 8 + 240 > static_cast<u32>(n)) return 0;
    const u8* udp = rx + 14 + header;
    const u8* d = udp + 8;
    if (get16(udp) != 67 || get16(udp + 2) != 68 || d[0] != 2 || get32(d + 4) != kXid) return 0;
    if (!same(d + 28, mac, 6) || get32(d + 236) != kCookie) return 0;
    copy(lease.ip, d + 16, 4);
    u32 type = 0;
    const u8* end = rx + n;
    for (const u8* o = d + 240; o < end && *o != 255;) {
        if (*o == 0) { ++o; continue; }
        if (o + 2 > end || o + 2 + o[1] > end) break;
        const u8* v = o + 2;
        const u32 length = o[1];
        if (o[0] == 53 && length >= 1) type = v[0];
        else if (o[0] == 54 && length >= 4) copy(lease.server, v, 4);
        else if (o[0] == 1 && length >= 4) copy(lease.mask, v, 4);
        else if (o[0] == 3 && length >= 4) copy(lease.router, v, 4);
        else if (o[0] == 6 && length >= 4) copy(lease.dns, v, 4);
        else if (o[0] == 51 && length >= 4) lease.seconds = get32(v);
        o += 2 + length;
    }
    return type;
}

// Reads frames (other traffic is ignored) until a DHCP reply of type `want` arrives, a NAK
// does, or a read times out.
bool wait_for(const usb::Services& services, const usb::Device& device, u32 want) {
    for (u32 frames = 0; frames < 16; ++frames) {
        const i32 n = services.transfer(device, link.bulk_in, 2, rx, static_cast<u16>(rx_length), 2000);
        if (n < 0) return false;
        const u32 type = parse_dhcp(n);
        if (type == want) return true;
        if (type == 6) return false;
    }
    return false;
}

void log_lease(const usb::Services& services, const char* name) {
    {
        Line line = {{}, 0};
        line.add("ecm dhcp "); line.add(name); line.add(" ip="); line.ip(lease.ip);
        line.add(" server="); line.ip(lease.server);
        line.add(" lease="); line.number(static_cast<i32>(lease.seconds));
        services.log(line.done());
    }
    Line more = {{}, 0};
    more.add("ecm dhcp mask="); more.ip(lease.mask);
    more.add(" router="); more.ip(lease.router);
    more.add(" dns="); more.ip(lease.dns);
    services.log(more.done());
}

// DISCOVER, OFFER, REQUEST, ACK: three tries.
bool dhcp(const usb::Services& services, const usb::Device& device) {
    for (u32 attempt = 0; attempt < 3; ++attempt) {
        fill(reinterpret_cast<u8*>(&lease), 0, sizeof lease);
        step(services, "dhcp-discover", services.transfer(device, link.bulk_out, 2, tx, static_cast<u16>(build_dhcp(1)), 1000));
        if (!wait_for(services, device, 2)) { services.log("ecm dhcp no offer"); continue; }
        log_lease(services, "offer");
        step(services, "dhcp-request", services.transfer(device, link.bulk_out, 2, tx, static_cast<u16>(build_dhcp(3)), 1000));
        if (wait_for(services, device, 5)) { log_lease(services, "bound"); return true; }
        services.log("ecm dhcp no ack");
    }
    return false;
}

// --- The rest of the way to a web page: ARP, DNS and a one-shot TCP client. ---

const u8 kBroadcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
const u8 kNoAddress[6] = {0, 0, 0, 0, 0, 0};
const u16 kDnsPort = 49153, kLocalPort = 49200;
const u32 kDnsId = 0x4f50, kSeq = 0x4f500000;
const u8 kFin = 0x01, kSyn = 0x02, kRst = 0x04, kPsh = 0x08, kAck = 0x10;

const usb::Services* S;  // the stack's services and device, for the length of the probe
const usb::Device* D;
u8 request[200];
u32 ip_id;

struct Neighbor { u8 ip[4]; u8 mac[6]; bool valid; };
Neighbor neighbors[4];
u32 neighbor_next;
u8 wanted[4];

u32 sum16(const u8* p, u32 n, u32 sum) {
    for (u32 i = 0; i + 1 < n; i += 2) sum += get16(p + i);
    if (n & 1) sum += static_cast<u32>(p[n - 1]) << 8;
    return sum;
}
u32 fold(u32 sum) {
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return ~sum & 0xffff;
}

void send_failed(i32 status) {
    Line line = {{}, 0};
    line.add("ecm send failed ret=");
    line.number(status);
    S->log(line.done());
}

// Sends tx[0..length): padded to the Ethernet minimum, and never an exact multiple of the
// bulk packet size, which would need a zero-length packet to end it.
void send(u32 length) {
    if (length < 60) length = 60;
    if (length % link.packet == 0) ++length;
    const i32 status = S->transfer(*D, link.bulk_out, 2, tx, static_cast<u16>(length), 1000);
    if (status < 0) send_failed(status);
}

u8* start_ip(const u8* destination) {
    fill(tx, 0, sizeof tx);
    copy(tx, destination, 6);
    copy(tx + 6, mac, 6);
    put16(tx + 12, 0x0800);
    return tx + 14;
}

// The IPv4 header for `payload` bytes already placed at ip + 20; returns the frame length.
u32 finish_ip(u8* ip, const u8* to, u8 protocol, u32 payload) {
    ip[0] = 0x45; put16(ip + 2, 20 + payload); put16(ip + 4, ++ip_id); put16(ip + 6, 0x4000);
    ip[8] = 64; ip[9] = protocol;
    copy(ip + 12, lease.ip, 4);
    copy(ip + 16, to, 4);
    put16(ip + 10, checksum(ip, 20));
    return 14 + 20 + payload;
}

// The IPv4 header of the frame in rx[0..n) if it is a whole, unfragmented datagram of this
// protocol for our address.
const u8* ipv4_in(i32 n, u8 protocol) {
    if (n < 14 + 20 || get16(rx + 12) != 0x0800 || (rx[14] >> 4) != 4) return nullptr;
    const u8* ip = rx + 14;
    const u32 header = static_cast<u32>(ip[0] & 0xf) * 4;
    const u32 total = get16(ip + 2);
    if (header < 20 || total < header || 14 + total > static_cast<u32>(n) || ip[9] != protocol) return nullptr;
    if ((get16(ip + 6) & 0x3fff) != 0 || !same(ip + 16, lease.ip, 4)) return nullptr;
    return ip;
}

// --- ARP ---
const u8* hop(const u8* target) {
    for (u32 i = 0; i < 4; ++i) if ((target[i] ^ lease.ip[i]) & lease.mask[i]) return lease.router;
    return target;
}

bool lookup(const u8* ip, u8* address) {
    for (u32 i = 0; i < 4; ++i)
        if (neighbors[i].valid && same(neighbors[i].ip, ip, 4)) { copy(address, neighbors[i].mac, 6); return true; }
    return false;
}

void learn(const u8* ip, const u8* address) {
    if (!same(ip, lease.router, 4) && !same(ip, lease.dns, 4) && !same(ip, wanted, 4)) return;
    u32 slot = neighbor_next;
    for (u32 i = 0; i < 4; ++i) if (neighbors[i].valid && same(neighbors[i].ip, ip, 4)) slot = i;
    if (slot == neighbor_next) neighbor_next = (neighbor_next + 1) % 4;
    copy(neighbors[slot].ip, ip, 4);
    copy(neighbors[slot].mac, address, 6);
    neighbors[slot].valid = true;
}

void arp_packet(u32 op, const u8* to_mac, const u8* target_mac, const u8* target_ip) {
    fill(tx, 0, sizeof tx);
    copy(tx, to_mac, 6); copy(tx + 6, mac, 6); put16(tx + 12, 0x0806);
    put16(tx + 14, 1); put16(tx + 16, 0x0800); tx[18] = 6; tx[19] = 4; put16(tx + 20, op);
    copy(tx + 22, mac, 6); copy(tx + 28, lease.ip, 4);
    copy(tx + 32, target_mac, 6); copy(tx + 38, target_ip, 4);
    send(42);
}

// Learns the sender of an ARP packet we care about, and answers a request for our address.
void handle_arp(i32 n) {
    if (n < 42 || get16(rx + 12) != 0x0806 || get16(rx + 14) != 1 || get16(rx + 16) != 0x0800 || rx[18] != 6 || rx[19] != 4) return;
    const u32 op = get16(rx + 20);
    if (op != 1 && op != 2) return;
    learn(rx + 28, rx + 22);
    if (op == 1 && same(rx + 38, lease.ip, 4)) {
        u8 to[6], ip[4];
        copy(to, rx + 22, 6);
        copy(ip, rx + 28, 4);
        arp_packet(2, to, to, ip);
    }
}

i32 receive(u32 timeout) {
    const i32 n = S->transfer(*D, link.bulk_in, 2, rx, static_cast<u16>(rx_length), timeout);
    if (n > 0) handle_arp(n);
    return n;
}

// The hardware address to send to for `target`: the host itself on our subnet, else the router.
bool resolve(const u8* target, u8* address) {
    const u8* next = hop(target);
    copy(wanted, next, 4);
    for (u32 attempt = 0; attempt < 3; ++attempt) {
        if (lookup(next, address)) return true;
        arp_packet(1, kBroadcast, kNoAddress, next);
        for (u32 frames = 0; frames < 16 && !lookup(next, address); ++frames)
            if (receive(1000) < 0) break;
    }
    return lookup(next, address);
}

// --- DNS ---
u32 write_name(u8* out, const char* name) {
    u8* start = out;
    while (*name) {
        u8* length = out++;
        while (*name && *name != '.') *out++ = static_cast<u8>(*name++);
        *length = static_cast<u8>(out - length - 1);
        if (*name == '.') ++name;
    }
    *out++ = 0;
    return static_cast<u32>(out - start);
}

// The position after a (possibly compressed) name at `pos`, or 0 past the end.
u32 skip_name(const u8* d, u32 pos, u32 end) {
    while (pos < end) {
        const u32 b = d[pos];
        if (b == 0) return pos + 1;
        if ((b & 0xc0) == 0xc0) return pos + 2;
        pos += 1 + b;
    }
    return 0;
}

bool parse_dns(i32 n, u8* result) {
    const u8* ip = ipv4_in(n, 17);
    if (!ip) return false;
    const u8* udp = ip + (ip[0] & 0xf) * 4;
    const u32 length = get16(udp + 4);
    if (get16(udp) != 53 || get16(udp + 2) != kDnsPort || length < 8 + 12 || udp + length > rx + n) return false;
    const u8* d = udp + 8;
    const u32 end = length - 8;
    if (get16(d) != kDnsId || !(d[2] & 0x80) || (d[3] & 0xf) != 0) return false;
    u32 pos = 12;
    for (u32 q = get16(d + 4); q && pos; --q) { pos = skip_name(d, pos, end); if (pos) pos += 4; }
    for (u32 a = get16(d + 6); a && pos && pos + 10 <= end; --a) {
        pos = skip_name(d, pos, end);
        if (!pos || pos + 10 > end) return false;
        const u32 type = get16(d + pos), size = get16(d + pos + 8);
        if (type == 1 && size == 4 && pos + 14 <= end) { copy(result, d + pos + 10, 4); return true; }
        pos += 10 + size;
    }
    return false;
}

bool dns_lookup(const char* name, u8* result) {
    u8 server[6];
    if (!resolve(lease.dns, server)) { S->log("ecm dns: no hardware address for the server"); return false; }
    for (u32 attempt = 0; attempt < 3; ++attempt) {
        u8* ip = start_ip(server);
        u8* udp = ip + 20;
        u8* d = udp + 8;
        put16(d, kDnsId); put16(d + 2, 0x0100); put16(d + 4, 1);  // one question, recursion desired
        u32 q = 12 + write_name(d + 12, name);
        put16(d + q, 1); put16(d + q + 2, 1);                     // type A, class IN
        q += 4;
        put16(udp, kDnsPort); put16(udp + 2, 53); put16(udp + 4, 8 + q);
        send(finish_ip(ip, lease.dns, 17, 8 + q));
        for (u32 frames = 0; frames < 16; ++frames) {
            const i32 n = receive(2000);
            if (n < 0) break;
            if (parse_dns(n, result)) return true;
        }
    }
    return false;
}

// --- TCP, one connection at a time ---
struct Tcp { u8 mac[6]; u8 ip[4]; u32 seq, ack; };
Tcp tcp;

void tcp_send(u8 flags, const u8* data, u32 length) {
    u8* ip = start_ip(tcp.mac);
    u8* t = ip + 20;
    const u32 header = (flags & kSyn) ? 24 : 20;
    put16(t, kLocalPort); put16(t + 2, 80); put32(t + 4, tcp.seq); put32(t + 8, (flags & kAck) ? tcp.ack : 0);
    t[12] = static_cast<u8>(header / 4 << 4); t[13] = flags; put16(t + 14, 5840);
    if (flags & kSyn) { t[20] = 2; t[21] = 4; put16(t + 22, 1460); }  // maximum segment size
    copy(t + header, data, length);
    const u32 segment = header + length;
    u32 sum = sum16(lease.ip, 4, 0);
    sum = sum16(tcp.ip, 4, sum) + 6 + segment;
    put16(t + 16, fold(sum16(t, segment, sum)));
    send(finish_ip(ip, tcp.ip, 6, segment));
}

// The TCP header of the frame in rx[0..n) if it belongs to our connection; `payload` gets
// the number of data bytes after it.
const u8* tcp_in(i32 n, u32& payload) {
    const u8* ip = ipv4_in(n, 6);
    if (!ip) return nullptr;
    const u32 header = static_cast<u32>(ip[0] & 0xf) * 4;
    const u8* t = ip + header;
    const u32 size = static_cast<u32>(t[12] >> 4) * 4;
    if (!same(ip + 12, tcp.ip, 4) || get16(t) != 80 || get16(t + 2) != kLocalPort || header + size > get16(ip + 2)) return nullptr;
    payload = get16(ip + 2) - header - size;
    return t;
}

u32 append(u8* out, u32 at, const char* text) {
    while (*text) out[at++] = static_cast<u8>(*text++);
    return at;
}

// --- What to read, and what to make of it ---
struct Target {
    const char* host;
    const char* path;
    const char* array_key;  // the key holding the array to count, or null when the JSON is the array
};
const Target kTargets[] = {
    {"open-xy.com", "/machines.json", "machines"},
    {"jsonplaceholder.typicode.com", "/users", nullptr},
};
const u32 kTarget = 1;  // until open-xy.com serves /machines.json

// --- A streaming JSON scan: counts the objects of one array and collects their "id" ---
const u32 kMaxDepth = 8, kIdBytes = 16, kMaxIds = 256;
char ids[kMaxIds][kIdBytes];
u32 id_count;       // ids found; only the first kMaxIds are kept
u32 element_count;  // objects in the array

bool equal(const char* a, const char* b) {
    while (*a && *a == *b) { ++a; ++b; }
    return *a == *b;
}

struct Json {
    const char* array_key;
    u32 depth;                       // open containers
    u32 array_depth;                 // the depth inside the array, once found
    bool array_found;
    char key[kMaxDepth + 1][12];     // the last key seen at each depth
    bool in_string, escaped, in_scalar, have_value;
    char token[kIdBytes];            // the string or number being read, or just read
    u32 token_length;

    void add(char c) { if (token_length < kIdBytes - 1) token[token_length++] = c; }

    void value_done() {
        if (!have_value) return;
        have_value = false;
        token[token_length] = 0;
        if (array_depth && depth == array_depth + 1 && depth <= kMaxDepth && equal(key[depth], "id")) {
            if (id_count < kMaxIds) for (u32 i = 0; i <= token_length; ++i) ids[id_count][i] = token[i];
            ++id_count;
        }
    }

    void open(char c) {
        if (c == '[' && !array_found && (depth == 0 ? !array_key : (depth == 1 && array_key && equal(key[1], array_key)))) {
            array_found = true;
            array_depth = depth + 1;
        } else if (c == '{' && array_depth && depth == array_depth) {
            ++element_count;
        }
        if (++depth <= kMaxDepth) key[depth][0] = 0;
        have_value = false;
    }

    void close() {
        if (depth == array_depth) array_depth = 0;
        if (depth) --depth;
    }

    void feed(u8 c) {
        if (in_string) {
            if (escaped) { escaped = false; add(static_cast<char>(c)); }
            else if (c == '\\') escaped = true;
            else if (c == '"') { in_string = false; have_value = true; }
            else add(static_cast<char>(c));
            return;
        }
        if (in_scalar) {
            if (c != ',' && c != '}' && c != ']' && c != ' ' && c != '\r' && c != '\n' && c != '\t') { add(static_cast<char>(c)); return; }
            in_scalar = false;
            have_value = true;
        }
        switch (c) {
            case ' ': case '\t': case '\r': case '\n': return;
            case '"': token_length = 0; in_string = true; have_value = false; return;
            case ':':
                if (have_value && depth <= kMaxDepth) {
                    token[token_length] = 0;
                    u32 i = 0;
                    for (; i < sizeof key[0] - 1 && token[i]; ++i) key[depth][i] = token[i];
                    key[depth][i] = 0;
                }
                have_value = false;
                token_length = 0;
                return;
            case ',': value_done(); return;
            case '{': case '[': open(static_cast<char>(c)); return;
            case '}': case ']': value_done(); close(); return;
            default: token_length = 0; in_scalar = true; add(static_cast<char>(c)); return;
        }
    }
};
Json json;

// --- An HTTP response, read as it arrives: headers, then a chunked or plain body into the scan ---
enum { kHeaders, kChunkSize, kChunkData, kChunkEnd, kBody, kDone };
struct Http {
    u32 state;
    char head[768];
    u32 head_length, matched;        // header bytes kept; how much of CR LF CR LF has been seen
    u32 status;
    bool chunked;
    i32 length;                      // Content-Length, or -1
    u32 chunk_left, digits;
    u32 total, body;                 // bytes received; body bytes fed to the scan
};
Http http;

char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; }

// The value of header `name` (lower case, with its colon) in the kept headers, or null.
const char* header(const char* name) {
    for (u32 i = 0; i < http.head_length;) {
        u32 k = 0;
        while (name[k] && i + k < http.head_length && lower(http.head[i + k]) == name[k]) ++k;
        if (!name[k]) {
            i += k;
            while (i < http.head_length && http.head[i] == ' ') ++i;
            return http.head + i;
        }
        while (i < http.head_length && http.head[i] != '\n') ++i;
        ++i;
    }
    return nullptr;
}

u32 number_at(const char* p) {
    u32 n = 0;
    while (*p >= '0' && *p <= '9') n = n * 10 + static_cast<u32>(*p++ - '0');
    return n;
}

void log_line(const char* label, const char* text) {
    Line line = {{}, 0};
    line.add(label);
    for (u32 i = 0; text[i] && text[i] != '\r' && text[i] != '\n' && line.length < 110; ++i) {
        const char c[2] = {text[i] >= 32 && text[i] < 127 ? text[i] : '.', 0};
        line.add(c);
    }
    S->log(line.done());
}

void end_of_headers() {
    http.head[http.head_length] = 0;
    http.status = http.head_length > 9 ? number_at(http.head + 9) : 0;  // after "HTTP/1.1 "
    const char* encoding = header("transfer-encoding:");
    http.chunked = false;
    for (u32 i = 0; encoding && encoding[i] && encoding[i] != '\r'; ++i)
        if (lower(encoding[i]) == 'c' && lower(encoding[i + 1]) == 'h') http.chunked = true;
    const char* length = header("content-length:");
    http.length = length ? static_cast<i32>(number_at(length)) : -1;
    log_line("ecm http ", http.head);  // the status line
    const char* location = header("location:");
    if (location) log_line("ecm http location ", location);
    http.chunk_left = http.digits = 0;
    http.state = http.chunked ? kChunkSize : (http.length == 0 ? kDone : kBody);
}

u32 hex_value(u8 c) { return hex_digit(c); }

void http_feed(const u8* data, u32 n) {
    http.total += n;
    for (u32 i = 0; i < n;) {
        const u8 c = data[i];
        switch (http.state) {
            case kHeaders: {
                if (http.head_length < sizeof http.head - 1) http.head[http.head_length++] = static_cast<char>(c);
                const u8 expected = (http.matched & 1) ? '\n' : '\r';
                http.matched = c == expected ? http.matched + 1 : (c == '\r' ? 1 : 0);
                ++i;
                if (http.matched == 4) end_of_headers();
                break;
            }
            case kChunkSize:
                if (hex_value(c) < 16) { http.chunk_left = http.chunk_left * 16 + hex_value(c); ++http.digits; }
                else if (c == '\n' && http.digits) http.state = http.chunk_left ? kChunkData : kDone;
                ++i;
                break;
            case kChunkEnd:
                if (c == '\n') { http.state = kChunkSize; http.chunk_left = http.digits = 0; }
                ++i;
                break;
            case kChunkData:
            case kBody: {
                u32 take = n - i;
                if (http.state == kChunkData && take > http.chunk_left) take = http.chunk_left;
                if (http.state == kBody && http.length >= 0 && take > static_cast<u32>(http.length) - http.body) take = static_cast<u32>(http.length) - http.body;
                for (u32 k = 0; k < take; ++k) json.feed(data[i + k]);
                http.body += take;
                i += take;
                if (http.state == kChunkData) { http.chunk_left -= take; if (!http.chunk_left) http.state = kChunkEnd; }
                else if (http.length >= 0 && http.body >= static_cast<u32>(http.length)) http.state = kDone;
                break;
            }
            default:
                i = n;  // after the last chunk: trailers are ignored
        }
    }
}

// The ids in lines of about 100 characters, then the count.
void report() {
    Line line = {{}, 0};
    line.add("ecm ids:");
    for (u32 i = 0; i < id_count && i < kMaxIds; ++i) {
        u32 n = 0;
        while (ids[i][n]) ++n;
        if (line.length + 1 + n > 100) { S->log(line.done()); line.length = 0; line.add("ecm ids:"); }
        line.add(" ");
        line.add(ids[i]);
    }
    if (line.length > 8) S->log(line.done());
    Line total = {{}, 0};
    total.add("ecm json elements="); total.number(static_cast<i32>(element_count));
    total.add(" ids found="); total.number(static_cast<i32>(id_count));
    S->log(total.done());
}

enum Outcome { kOk, kNoDns, kNoArp, kNoConnect, kRefused, kStatus, kNoIds };

Outcome http_get(const Target& target) {
    u8 server[4];
    if (!dns_lookup(target.host, server)) { S->log("ecm dns: no answer"); return kNoDns; }
    {
        Line line = {{}, 0};
        line.add("ecm dns "); line.add(target.host); line.add("="); line.ip(server);
        S->log(line.done());
    }
    copy(tcp.ip, server, 4);
    if (!resolve(server, tcp.mac)) { S->log("ecm arp: no hardware address for the next hop"); return kNoArp; }

    tcp.seq = kSeq; tcp.ack = 0;
    bool open = false;
    for (u32 attempt = 0; attempt < 3 && !open; ++attempt) {
        tcp_send(kSyn, nullptr, 0);
        for (u32 frames = 0; frames < 16; ++frames) {
            const i32 n = receive(2000);
            if (n < 0) break;
            u32 payload = 0;
            const u8* t = tcp_in(n, payload);
            if (!t) continue;
            if (t[13] & kRst) { S->log("ecm tcp refused"); return kRefused; }
            if ((t[13] & (kSyn | kAck)) == (kSyn | kAck) && get32(t + 8) == tcp.seq + 1) {
                tcp.ack = get32(t + 4) + 1;
                tcp.seq += 1;
                open = true;
                break;
            }
        }
    }
    if (!open) { S->log("ecm tcp no answer to the SYN"); return kNoConnect; }
    S->log("ecm tcp connected");
    tcp_send(kAck, nullptr, 0);

    u32 size = append(request, 0, "GET ");
    size = append(request, size, target.path);
    size = append(request, size, " HTTP/1.1\r\nHost: ");
    size = append(request, size, target.host);
    size = append(request, size, "\r\nUser-Agent: OP-XY\r\nAccept: application/json\r\nConnection: close\r\n\r\n");
    tcp_send(kPsh | kAck, request, size);
    tcp.seq += size;

    fill(reinterpret_cast<u8*>(&http), 0, sizeof http);
    fill(reinterpret_cast<u8*>(&json), 0, sizeof json);
    json.array_key = target.array_key;
    id_count = element_count = 0;
    bool finished = false;
    u32 quiet = 0;
    for (u32 frames = 0; frames < 600 && !finished && quiet < 3; ++frames) {
        const i32 n = receive(3000);
        if (n < 0) { ++quiet; tcp_send(kAck, nullptr, 0); continue; }
        u32 payload = 0;
        const u8* t = tcp_in(n, payload);
        if (!t) continue;
        quiet = 0;
        if (t[13] & kRst) { S->log("ecm tcp reset"); break; }
        if (get32(t + 4) != tcp.ack) { tcp_send(kAck, nullptr, 0); continue; }  // a gap or a repeat: ask again
        if (payload) {
            http_feed(t + (t[12] >> 4) * 4, payload);
            tcp.ack += payload;
        }
        if (t[13] & kFin) {
            tcp.ack += 1;
            tcp_send(kFin | kAck, nullptr, 0);
            tcp.seq += 1;
            finished = true;
        } else if (http.state == kDone) {
            tcp_send(kFin | kAck, nullptr, 0);  // the body is complete: close our side
            tcp.seq += 1;
            finished = true;
        } else if (payload) {
            tcp_send(kAck, nullptr, 0);
        }
    }
    {
        Line line = {{}, 0};
        line.add("ecm http status="); line.number(static_cast<i32>(http.status));
        line.add(" bytes="); line.number(static_cast<i32>(http.total));
        line.add(" body="); line.number(static_cast<i32>(http.body));
        line.add(http.chunked ? " chunked" : " plain");
        line.add(http.state == kDone || finished ? " complete" : " incomplete");
        S->log(line.done());
    }
    report();
    if (http.status != 200) return kStatus;
    return id_count ? kOk : kNoIds;
}

// --- The adapter, brought up by the host stack's probe, used later from a key ---
usb::Device adapter;      // a copy: the probe's argument does not outlive it
bool adapter_ready;       // alternate setting 1 selected, frames enabled, MAC read
bool leased;

bool lease_address() {
    if (leased) return true;
    leased = dhcp(*S, *D);
    return leased;
}

void log_link(const usb::Services& services) {
    Line line = {{}, 0};
    line.add("ecm link control="); line.number(link.control_interface);
    line.add(" data="); line.number(link.data_interface);
    line.add(" alt="); line.number(link.alternate);
    line.add(" in=0x"); line.hex(link.bulk_in, 2);
    line.add(" out=0x"); line.hex(link.bulk_out, 2);
    line.add(" packet="); line.number(link.packet);
    line.add(" mac-string="); line.number(link.mac_string);
    services.log(line.done());
}

// Reads the adapter's descriptors, once per device. False when it is not an ECM adapter this
// driver understands.
u32 prepared_device;
bool prepare(const usb::Services& services, const usb::Device& device) {
    if (prepared_device == device.handle) return link.complete();
    prepared_device = device.handle;
    link = Link();
    if (!services.control || !read_config(services, device)) { services.log("ecm: no configuration descriptor"); return false; }
    parse_config();
    log_link(services);
    if (!link.complete()) { services.log("ecm: not a CDC-ECM adapter this driver understands"); return false; }
    rx_length = sizeof rx / link.packet * link.packet;
    return rx_length != 0;
}

void bring_up(const usb::Services& services, const usb::Device& device) {
    if (!services.set_interface || !services.control || !services.transfer) return;
    S = &services;
    adapter = device;
    D = &adapter;
    step(services, "set-interface", services.set_interface(device, link.data_interface, link.alternate));
    // SET_ETHERNET_PACKET_FILTER, addressed to the control interface.
    step(services, "filter", services.control(device, 0x21, 0x43, kFilterDirectedBroadcast, link.control_interface, nullptr, 0));
    adapter_ready = read_mac(services, device);
    if (adapter_ready) lease_address();  // best effort at boot; a key tries again
}

u32 probe(const usb::Services& services, const usb::Device& device, u32 interface) {
    Line line = {{}, 0};
    line.add("ecm probe i=");
    line.number(static_cast<i32>(interface));
    line.add(" cfg=");
    line.number(device.configuration_value);
    line.add(" spd=");
    line.number(device.speed);
    services.log(line.done());
    if (!prepare(services, device)) return 0;  // declined: the stack goes on as without this driver
    if (interface == link.data_interface) bring_up(services, device);
    return interface + 1;  // claimed; handed back to remove as the context
}

void remove(const usb::Services& services, const usb::Device&, u32 context) {
    Line line = {{}, 0};
    line.add("ecm remove ctx=");
    line.number(static_cast<i32>(context));
    services.log(line.done());
    adapter_ready = leased = false;
    prepared_device = 0;
}

const usb::Match matches[] = {
    {usb::match::interface_class | usb::match::interface_subclass, 0, 0, 0, 0, 0, 0x02, 0x06, 0},
    {usb::match::interface_class, 0, 0, 0, 0, 0, 0x0a, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0},
};
const usb::Driver driver = {"usb-ecm", matches, probe, remove};

void usb_host_ready(const usb::Registry& registry) { registry.add(driver); }

// --- The trigger and the answer ---
// The popup holds 31 characters: "<n> ids, <bytes> bytes", or what went wrong.
void say(const Actions& actions, const char* first, u32 number = 0, const char* second = "") {
    char text[32];
    u32 length = 0;
    for (const char* p = first; *p && length < 31; ++p) text[length++] = *p;
    if (number || !*second) {
        char digits[10];
        u32 count = 0;
        do { digits[count++] = static_cast<char>('0' + number % 10); number /= 10; } while (number);
        while (count && length < 31) text[length++] = digits[--count];
    }
    for (const char* p = second; *p && length < 31; ++p) text[length++] = *p;
    text[length] = 0;
    actions.notify(text);
}

// Runs on the controller path and blocks it until the page is read or a step gives up: a
// proof of concept, which a real feature would move to a task of its own.
void fetch(const Actions& actions) {
    if (!adapter_ready) { actions.notify("No USB Ethernet adapter"); return; }
    if (!lease_address()) { actions.notify("No DHCP lease"); return; }
    const Target& target = kTargets[kTarget];
    switch (http_get(target)) {
        case kOk: {
            char more[24] = " ids, ";
            u32 length = 6;
            u32 bytes = http.total, digits = 1;
            for (u32 n = bytes; n >= 10; n /= 10) ++digits;
            for (u32 i = digits; i > 0; --i) { more[length + i - 1] = static_cast<char>('0' + bytes % 10); bytes /= 10; }
            length += digits;
            for (const char* p = " bytes"; *p; ++p) more[length++] = *p;
            more[length] = 0;
            say(actions, "", id_count, more);
            break;
        }
        case kNoDns: actions.notify("DNS lookup failed"); break;
        case kNoArp: actions.notify("No answer from the router"); break;
        case kNoConnect: actions.notify("Could not connect"); break;
        case kRefused: actions.notify("Connection refused"); break;
        case kStatus: say(actions, "HTTP status ", http.status); break;
        case kNoIds: actions.notify("No ids in the response"); break;
    }
}

DoubleTap record_tap;

// A double-tap on Record (stock still records) reads the page and shows the outcome.
void queued_control(const Context&, const Control& control, const Actions& actions) {
    if (control.key != keys::record) { record_tap.cancel(); return; }
    if (control.pressed) { record_tap.press(control.time, control.unmodified); return; }
    if (record_tap.release(control.time)) fetch(actions);
}
}
extern constexpr Hooks hooks = Hooks().with_usb_host_ready(usb_host_ready).with_queued_control(queued_control);
}}}
