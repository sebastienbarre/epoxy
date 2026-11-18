#include <opxy/extension.hpp>

// USB probe: a diagnostic. It adds a USB host driver that matches a Realtek RTL8153 gigabit
// Ethernet adapter (0bda:8153) and logs what the stock host stack tells it about the device
// and each configuration and interface it tries. The driver declines every interface, so
// the stack goes on exactly as it would without it. The log is the device's own
// (logs/ over MTP).
namespace opxy { namespace extensions { namespace usb_probe {
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
    const char* done() { text[length] = 0; return text; }
};

void describe(Line& line, const usb::Device& device) {
    line.hex(device.vendor_id, 4);
    line.add(":");
    line.hex(device.product_id, 4);
    line.add(" spd=");
    line.number(device.speed);
    line.add(" adr=");
    line.number(device.address);
    line.add(" cls=");
    line.hex(device.device_class, 2);
    line.add("/");
    line.hex(device.device_subclass, 2);
    line.add("/");
    line.hex(device.device_protocol, 2);
    line.add(" cfg=");
    line.number(device.configuration_value);
    line.add("/");
    line.number(device.configuration_count);
    line.add(" cur=");
    line.number(device.current_configuration);
    line.add(" ifc=");
    line.number(device.interface_count);
    line.add(" pwr=");
    line.number(device.max_power);
}

alignas(32) u8 buffer[256];
bool dumped;

// GET_DESCRIPTOR as the stock stack does it: the header first, then exactly the length it gives.
// Logged as `label index ret=<header status>/<full status>`, then the bytes in lines of 24.
void read(const usb::Services& services, const usb::Device& device, const char* label, u32 index, u16 value, u16 language) {
    const bool configuration = label[0] == 'c';
    for (u32 i = 0; i < sizeof buffer; ++i) buffer[i] = 0;
    const i32 first = services.control(device, 0x80, 6, value, language, buffer, configuration ? 9 : 2);
    // A configuration descriptor says how long it is; a string's first byte does.
    u32 length = first < 0 ? 0 : configuration ? (buffer[2] | buffer[3] << 8) : buffer[0];
    if (length > sizeof buffer) length = sizeof buffer;
    i32 status = first;
    if (length) status = services.control(device, 0x80, 6, value, language, buffer, static_cast<u16>(length));
    Line head = {{}, 0};
    head.add(label);
    head.add(" ");
    head.number(static_cast<i32>(index));
    head.add(" ret=");
    head.number(first);
    head.add("/");
    head.number(status);
    services.log(head.done());
    if (status < 0) return;
    for (u32 offset = 0; offset < length; offset += 24) {
        Line line = {{}, 0};
        line.hex(offset, 2);
        line.add(":");
        for (u32 i = offset; i < offset + 24 && i < length; ++i) line.hex(buffer[i], 2);
        services.log(line.done());
    }
}

u32 probe(const usb::Services& services, const usb::Device& device, u32 interface) {
    Line line = {{}, 0};
    line.add("probe ");
    describe(line, device);
    line.add(" i=");
    line.number(static_cast<i32>(interface));
    services.log(line.done());
    if (services.control && device.configuration_value == 2 && !dumped) {
        dumped = true;
        for (u32 index = 0; index < device.configuration_count; ++index)
            read(services, device, "cfg", index, 0x0200 | index, 0);
        read(services, device, "str", 3, 0x0300 | 3, 0x0409);  // the ECM MAC address
    }
    return 0;  // decline: the stack tries the next interface, configuration or driver
}

void remove(const usb::Services& services, const usb::Device& device, u32) {
    Line line = {{}, 0};
    line.add("remove ");
    describe(line, device);
    services.log(line.done());
}

const usb::Match matches[] = {
    {usb::match::vendor | usb::match::product, 0x0bda, 0x8153, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0},
};
const usb::Driver driver = {"usb-probe", matches, probe, remove};

void usb_host_ready(const usb::Registry& registry) {
    dumped = false;
    registry.add(driver);
}
}
extern constexpr Hooks hooks = Hooks().with_usb_host_ready(usb_host_ready);
}}}
