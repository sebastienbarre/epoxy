# USB probe

A diagnostic extension: it adds a USB host driver that matches a Realtek RTL8153 adapter
(`0bda:8153`) by vendor and product ID and logs what the stock USB host stack tells it about the
device and each configuration and interface it tries. It is specific to that adapter by design,
and it declines every interface, so the stack behaves as it does without it. It reads the
adapter's configuration descriptors and its MAC string (index 3) with control transfers and logs
them as hex. Needs the SDK's `usb_host_ready` event and USB host services.

The lines go into the device's own log, the `logs/log_<date>_<n>.txt` file it starts at each boot
(visible over MTP), tagged `usb_host_ext[1]`; `grep usb_host_ext` finds them. The first lines
say how far things got:

| Line | Meaning |
| --- | --- |
| `usb host: hook ran, drivers=1` | The host stack started and this extension's driver was added (`drivers=0`: it was not). Printed again each time the stack restarts the host |
| `usb host: register refused` | The stack refused the driver |

A probe line looks like:

```text
probe 0bda:8153 spd=3 adr=1 cls=00/00/00 cfg=2/2 cur=2 ifc=2 pwr=90 i=0
```

| Field | Meaning |
| --- | --- |
| `spd` | the stack's speed code (3 is high speed) |
| `adr` | the device's USB address |
| `cls` | device class, subclass and protocol |
| `cfg` | the configuration being tried: its value, then the device's configuration count |
| `cur` | the configuration the device is set to, -1 before any |
| `ifc`, `pwr` | its interface count and raw `bMaxPower` (in 2 mA units at high speed) |
| `i` | the interface index |

Then, for the second configuration, `cfg <index> ret=<header>/<full>` lines with the descriptor
in hex (24 bytes a line), and `str 3 ret=<header>/<full>` with the MAC string.

Check and build, from the repository that holds this folder, with the SDK checkout beside it:

```sh
python ../open-xy/sdk.py test --extension deliverables/usb/usb_probe
```

Installing a build is a device operation: ask for it explicitly.
