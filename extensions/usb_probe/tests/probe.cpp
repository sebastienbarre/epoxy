#include <extension.cpp>
#include <cstdio>
#include <cstring>
#include <string>

// The USB probe through the usb_host_ready event, as the SDK's adapter delivers it: a fake
// registry takes the driver, and fake services collect what it logs.
using namespace opxy;
namespace {
#define CHECK(value) do { if (!(value)) return __LINE__; } while (false)

const usb::Driver* added[4];
u32 added_count;
bool add(const usb::Driver& driver) {
    if (added_count == 4) return false;
    added[added_count++] = &driver;
    return true;
}
std::string log_text;
void log(const char* line) { log_text += line; log_text += '\n'; }
}

int main() {
    const extensions::Hooks& hooks = extensions::usb_probe::hooks;
    CHECK(hooks.usb_host_ready && !hooks.context && !hooks.queued_control);
    const usb::Registry registry = {add};
    hooks.usb_host_ready(registry);
    CHECK(added_count == 1);
    const usb::Driver& driver = *added[0];
    CHECK(std::strcmp(driver.name, "usb-probe") == 0 && driver.probe && driver.remove);

    // It matches the RTL8153 by vendor and product and nothing else, and its table ends
    // with the all-zero entry the stack looks for.
    const usb::Match& entry = driver.matches[0];
    CHECK(entry.flags == (usb::match::vendor | usb::match::product));
    CHECK(entry.vendor_id == 0x0bda && entry.product_id == 0x8153);
    CHECK(entry.interface_class == 0 && entry.device_class == 0);
    const usb::Match& end = driver.matches[1];
    CHECK(end.flags == 0 && end.vendor_id == 0 && end.device_class == 0 && end.interface_class == 0);

    // A device in its second configuration, high speed: one line a probe, and the probe
    // declines so the stack's own choice of driver is unchanged.
    const usb::Services services = {log};
    usb::Device device = {};
    device.speed = 3; device.address = 5;
    device.vendor_id = 0x0bda; device.product_id = 0x8153;
    device.configuration_count = 2; device.configuration_value = 2; device.interface_count = 2;
    device.max_power = 0x20; device.current_configuration = -1;
    CHECK(driver.probe(services, device, 1) == 0);
    CHECK(log_text == "probe 0bda:8153 spd=3 adr=5 cls=00/00/00 cfg=2/2 cur=-1 ifc=2 pwr=32 i=1\n");
    log_text.clear();
    device.current_configuration = 2;
    driver.remove(services, device, 0);
    CHECK(log_text == "remove 0bda:8153 spd=3 adr=5 cls=00/00/00 cfg=2/2 cur=2 ifc=2 pwr=32\n");

    // A log line never overruns its buffer, whatever the values.
    log_text.clear();
    device.vendor_id = 0xffff; device.product_id = 0xffff;
    device.speed = 255; device.address = 255; device.max_power = 255;
    device.current_configuration = -32768;
    driver.probe(services, device, 4000000000u);
    CHECK(log_text.size() <= 121 && log_text.back() == '\n');
    std::puts("ok");
    return 0;
}
