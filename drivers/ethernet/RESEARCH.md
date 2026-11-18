# OP-XY USB Ethernet Research: RTL8153 Support

> **Status (2026-11-17):** The goal is met. An extension on the OP-XY, through the stock USB host
> stack, used a standard USB-C Ethernet adapter (CableCreation, Realtek RTL8153, in its CDC-ECM
> configuration) to get an address by DHCP, resolve a name, open a TCP connection and read a JSON
> file over plain HTTP, and showed the number of ids in it on a Record double-tap. The SDK part (a
> `usb_host_ready` event and control, `SET_INTERFACE` and bulk transfer services) is merged as
> [etcetc000/open-xy#548](https://github.com/etcetc000/open-xy/pull/548); the extension is
> `extensions/usb_ecm` in this repository. Sections 1 to 9 are the offline analysis that led there;
> section 10 is the proof of concept and its device results.

## Objective

Investigate whether we can add USB Ethernet connectivity to the Teenage Engineering OP-XY by using its existing Blackfin USB host hardware and implementing the necessary USB Ethernet driver and networking support in firmware.

The immediate target is a **CableCreation USB-C to Gigabit Ethernet adapter**, identified as a Realtek RTL8153.

Repository:
https://github.com/etcetc000/open-xy

Linux driver reference:
https://github.com/wget/realtek-r8152-linux

The Linux driver is a particularly useful reference because it contains a C implementation for the same family of Realtek USB Ethernet controllers. The goal is not necessarily to port the entire Linux driver, but to understand the hardware initialization, USB control transfers, register access, packet transmission/reception, and other requirements needed for a minimal implementation on the Blackfin.

---

## 1. OP-XY hardware findings

The OP-XY hardware reference is available here:

- https://github.com/bagofwaffles/hardware-reference
- https://github.com/bagofwaffles/hardware-reference/blob/main/parts.md
- https://github.com/bagofwaffles/hardware-reference/blob/main/boards/backplane.md

Relevant hardware identified in the reference:

- **Two Analog Devices ADSP-BF703 Blackfin processors**, master and slave.
- The master ADSP-BF703 has a USB 2.0 OTG controller/PHY.
- A Cirrus Logic CS47L35 audio codec/DSP.
- A Nordic nRF52840 used for Bluetooth and other wireless functionality.
- A Skyworks SKY66404-11 RF front end associated with the 2.4 GHz radio.

The nRF52840 and Skyworks RF front end are not relevant to Ethernet connectivity through the USB adapter.

### USB host support is confirmed

Teenage Engineering explicitly documents the OP-XY USB-C port as:

> usb type-c audio/midi host & device

Official documentation:
https://teenage.engineering/guides/op-xy/hardware-overview

This is important: the OP-XY already supports USB host operation. We do not need to establish whether the hardware can operate as a USB host in general.

However, USB MIDI/audio host support does **not** establish that the firmware supports USB Ethernet. Ethernet requires an appropriate USB driver and networking implementation.

The USB-C port also has a FUSB302 Type-C/Power Delivery controller. This should not be confused with the Blackfin USB data controller. The FUSB302 handles USB-C configuration-channel and power-delivery functions; it is not itself an Ethernet controller or USB host data controller.

---

## 2. Ethernet adapter identification

The adapter being tested:

CableCreation USB-C to Gigabit Ethernet Adapter:
https://www.cablecreation.com/products/usb-c-ethernet-adapter-1000mbps

When connected to a Mac, its USB device information was:

```text
USB Product Name: USB 10_100_1000 LAN
USB Vendor Name: Realtek
idVendor: 3034
idProduct: 33107
bDeviceClass: 0
bNumConfigurations: 2
kUSBCurrentConfiguration: 2
UsbLinkSpeed: 5000000000
```

Converting the vendor and product IDs:

```text
VID: 0x0BDA
PID: 0x8153
```

This identifies the adapter as a **Realtek RTL8153-family USB Gigabit Ethernet device**.

Additional observations:

- The adapter is recognized by macOS as `USB 10/100/1000 LAN`.
- macOS reports a 5 Gbps USB link speed. This is the negotiated USB connection speed, not the Ethernet link speed.
- `bDeviceClass = 0` means the USB class is defined at the interface level rather than at the device level.
- The device exposes two USB configurations; macOS selected configuration 2.
- The adapter's Ethernet LEDs illuminate and blink when connected to the OP-XY with an Ethernet cable attached.

The LEDs suggest that the adapter is receiving power and that its Ethernet side is active. They do **not** prove that the OP-XY has enumerated the USB device or loaded a driver.

We have not yet captured the adapter's complete USB interface descriptors. These are important for determining its interface class, subclass, protocol, endpoints, and configuration requirements.

---

## 3. Linux driver reference

A C implementation of the Realtek USB Ethernet driver is available here:

https://github.com/wget/realtek-r8152-linux

This is a key reference for the project.

The Linux driver family is commonly known as `r8152`. It supports Realtek USB Ethernet controllers, including the RTL8153 family.

The Linux implementation should be examined to identify:

- USB device identification and supported revisions.
- Device initialization sequences.
- USB control transfers and register access.
- PHY initialization and configuration.
- Firmware loading requirements, if any for the relevant revision.
- USB bulk-in and bulk-out endpoint usage.
- RX and TX descriptor formats.
- Packet framing and buffer management.
- Link status and speed negotiation.
- Interrupt endpoint usage, if applicable.
- Reset and recovery behavior.

Do not assume that the Linux driver can simply be compiled for the Blackfin. It depends on Linux kernel infrastructure, USB APIs, networking APIs, memory-management facilities, synchronization primitives, and potentially other kernel services.

The useful approach is to identify the hardware-specific portions of the driver and isolate the minimum functionality required for a standalone or existing-firmware implementation.

---

## 4. What we have not established yet

The following questions remain open and should be investigated in the repository before implementation.

### Existing USB implementation

1. Where is USB host initialization implemented?
2. Which Blackfin processor owns the USB controller?
3. Is the USB host stack implemented by Teenage Engineering, imported from a third party, or provided by another library?
4. Does the current implementation support USB device enumeration?
5. Does it support configuration and interface descriptor retrieval?
6. Which USB device classes have drivers?
7. Is there an abstraction for registering or implementing additional USB device-class drivers?
8. Are bulk transfers supported?
9. Are interrupt transfers supported?
10. Is there an existing USB logging or diagnostic mechanism?

### Blackfin execution and build environment

1. How is C code compiled for the ADSP-BF703?
2. Which compiler and toolchain versions are used?
3. What linker scripts, startup code, and runtime libraries are required?
4. How are firmware components loaded and started?
5. Can a small diagnostic application be built and executed independently?
6. Is there an existing safe mechanism for running experimental code without replacing the main firmware?
7. What debugging and logging facilities are available?

### USB-C and power

1. Does the existing USB host implementation configure the USB-C controller appropriately for host operation?
2. What VBUS voltage and current are available to external devices?
3. Is there a power switch or current-limiting circuit involved?
4. Are there limitations that could affect the RTL8153 adapter's power requirements?

Host operation is officially supported, but the specific power characteristics and behavior with this Ethernet adapter still need verification.

### Networking

1. Is there already a TCP/IP stack in the OP-XY firmware?
2. Is there an existing DHCP implementation?
3. Are DNS and socket APIs available?
4. Is there an existing network interface abstraction?
5. Would it be practical to integrate a lightweight TCP/IP stack if none exists?

The Ethernet driver and the networking stack are separate concerns. Successfully transmitting and receiving Ethernet frames does not automatically provide IP networking.

---

## 5. Recommended investigation and implementation plan

### Phase 1: Understand the existing USB stack

Inspect the repository and identify:

- Blackfin USB controller initialization.
- USB host stack implementation.
- Enumeration code.
- Descriptor parsing.
- Transfer scheduling and endpoint handling.
- Existing USB class drivers.
- Build and linker configuration.

Map the relevant source files and dependencies before proposing changes.

Do not assume that the USB implementation is equivalent to a generic Linux USB host stack.

### Phase 2: Build a minimal USB diagnostic

Before attempting to implement Ethernet, create a small diagnostic C program or firmware component that uses the existing USB host infrastructure.

Its purpose should be to:

1. Initialize or reuse the existing USB host controller.
2. Detect the RTL8153 adapter.
3. Enumerate the device.
4. Read its device descriptor.
5. Read its configuration descriptors.
6. Enumerate its interfaces and endpoints.
7. Print or otherwise expose:
   - VID/PID
   - Device revision
   - Configuration count
   - Interface class, subclass, and protocol
   - Endpoint addresses and transfer types
   - Maximum packet sizes

Expected device identification:

```text
VID: 0x0BDA
PID: 0x8153
```

The diagnostic should avoid modifying persistent device state or performing unnecessary hardware initialization.

**Success criterion:** The OP-XY can enumerate the adapter and retrieve its descriptors.

This is the first major milestone. It will establish whether the existing USB host stack can communicate with this particular device.

### Phase 3: Analyze the RTL8153 driver

> **Revised in section 9:** the adapter offers a CDC-ECM configuration (value 2), a standard class that needs no Realtek register programming. The vendor-mode `r8152` port described below is the fallback if the stack cannot select that configuration.

Study the Linux `r8152` implementation and identify the minimum subset required for the RTL8153 revision found in the CableCreation adapter.

The port should be designed around the OP-XY's existing USB APIs, rather than attempting to reproduce Linux kernel interfaces.

Potential implementation components:

- RTL8153 device detection.
- USB control-transfer abstraction.
- Register read/write operations.
- Device initialization.
- RX/TX buffer and descriptor handling.
- Bulk endpoint transfers.
- Link-state detection.
- Reset and error recovery.

Avoid importing Linux-specific infrastructure unnecessarily.

### Phase 4: Implement Ethernet frame transmission and reception

Once initialization works, implement the minimum data path:

- Transmit an Ethernet frame.
- Receive an Ethernet frame.
- Handle buffers and USB transfer completion.
- Handle link loss and recovery.

At this stage, test with a controlled environment and packet-level diagnostics.

**Success criterion:** The OP-XY can reliably transmit and receive Ethernet frames through the RTL8153.

### Phase 5: Add IP networking

If the firmware does not already contain a suitable networking stack, investigate integrating a lightweight stack such as lwIP.

Potential functionality:

- Ethernet interface integration.
- ARP.
- IPv4.
- DHCP.
- DNS.
- TCP and UDP.

The actual networking requirements depend on the eventual use case. A basic DHCP lease and ICMP ping would be a reasonable initial test.

**Success criterion:** The OP-XY obtains an IP address and can communicate with another device on the local network.

---

## 6. Important implementation considerations

### Memory constraints

The Blackfin has finite memory and the OP-XY firmware has existing real-time audio responsibilities.

Investigate:

- Available RAM and memory regions.
- USB DMA requirements and buffer alignment.
- Whether the USB controller requires DMA-accessible memory.
- Cache coherency requirements.
- Stack and heap availability.
- Whether Ethernet traffic could interfere with audio processing.

Avoid large allocations and unbounded buffering. Ethernet traffic should not compromise audio performance.

### Real-time behavior

USB Ethernet processing must coexist with the synthesizer's audio engine.

Investigate how USB host transfers are scheduled and whether the existing architecture provides asynchronous or interrupt-driven completion.

Avoid blocking operations in time-critical audio paths.

### Power consumption

A USB Ethernet adapter adds power consumption. Confirm the available USB host power budget before assuming that the adapter can operate reliably from the OP-XY.

### Firmware safety

Initially, implement this as a diagnostic or isolated experimental component.

Do not modify the bootloader or flash-writing mechanisms as part of the first experiments. Establish a reliable recovery path before attempting changes to low-level firmware components.

---

## 7. Suggested first deliverables

The first Copilot research pass should produce:

1. A map of the existing USB host implementation, with exact source paths.
2. An explanation of how the Blackfin USB controller is initialized.
3. An explanation of how USB enumeration and descriptor retrieval work.
4. A list of currently supported USB device classes.
5. The relevant build and linker instructions for compiling Blackfin C code.
6. An assessment of whether the existing stack can enumerate an RTL8153 without modification.
7. A proposed minimal diagnostic implementation.
8. A porting analysis of the Linux `r8152` driver, identifying the hardware-specific code worth reusing.
9. An assessment of whether an IP networking stack already exists or whether lwIP would be needed.

**Do not start by implementing a complete Ethernet driver.** First establish whether the existing USB stack can enumerate the adapter. That is the cheapest experiment and will provide the information needed to plan the rest of the work.

---

## 8. Key references

| Subject | Reference |
| --- | --- |
| OP-XY firmware repository | https://github.com/etcetc000/open-xy |
| OP-XY hardware reference | https://github.com/bagofwaffles/hardware-reference |
| OP-XY hardware overview (official) | https://teenage.engineering/guides/op-xy/hardware-overview |
| CableCreation Ethernet adapter | https://www.cablecreation.com/products/usb-c-ethernet-adapter-1000mbps |
| Realtek Linux driver (C) | https://github.com/wget/realtek-r8152-linux |
| Potential lightweight TCP/IP stack | https://github.com/lwip-tcpip/lwip |

---

## 9. Findings so far (2026-11-17)

Every stock address below is for OS 1.2.1 only. This section is the offline analysis: the stock firmware was read without a device, and the adapter's descriptors were read from a Mac. Section 10 has what a device then showed. Statements are labelled **confirmed** (read directly from the image or the adapter) or **inferred** (a reading of disassembly that has not been tested).

### 9.1 Method

- The decrypted `os.ldr` 1.2.1 is parsed with the SDK's `tooling/formats.py` (`ldr`, `mapped_offset`). Its code regions are disassembled with the SDK toolchain's `bfin-elf-objdump -b binary -m bfin -D --adjust-vma=<load address>`.
- `objdump` prints 32-bit immediate loads as signed decimals (`R1=-2080...`), so an address is found by searching for `address - 2^32`.
- Strings come from `strings -a` over the image. Stock debug strings carry source paths (`../src/bf/smx_usb.cpp`) and are the main anchors.

### 9.2 The stock USB stack (answers part of section 4)

**Confirmed from strings:**

- It is the commercial **smx USB** stack from Micro Digital: `smxUSBD-Blackfin7` for the device side and `smxUSBH-Blackfin` for the host side, on **FreeRTOS** (`adi-freertos/FreeRTOSv10.0.0`, port `Blackfin_BF70x`) through a shim, `smx_usb/XBASE/bos-freertos.c`.
- Host class drivers present: **hub** (`smxUSBH-Hub`) and **audio/MIDI** (`XUSBH/Audio/uaudio.c`, `uaudio2.c`, task `smxUSBH-midi`, device name `usb-audio`). Reading the registrations (section 9.3) adds two small drivers matched by vendor ID: an Apple role switch and a USB-IF test-mode driver. There is no registration for CDC, ECM, RNDIS, HID or mass storage.
- `init cdc` in the boot sequence (beside `init rtc`, `init nordic`, `init pmic`) is the audio **codec**, not USB CDC.
- The OP-XY's own device side is a composite: audio and MIDI streaming, mass storage (`OP-XY Disk`), MTP, and a serial port (`PORT0`).
- Application glue is `src/bf/smx_usb.cpp`, `src/bf/usb_control.cpp`, `src/bf/usb_audio.cpp`, `src/midi/usb_midi.cpp` and `src/usb/mtp/`. `usb_control.cpp` switches between host and device roles, sets VBUS and a current limit (`smx callback: set ilim: %d mA`, `enable vbus`), and defaults to device mode when the Type-C controller does not answer (`no fusb response from psoc_kb`). It also logs `not enumerated within 3 seconds` and `usb host pid`.
- **There is no TCP/IP stack.** The image contains no strings for lwIP, TCP, DHCP, sockets, ethernet, ECM, RNDIS or NCM.

**Code locations:**

| What | Address |
| --- | --- |
| Task creation function (task, entry, priority, stack, name) | `0x8c05b6a8` |
| `smxUSBD-Blackfin7` task created at | `0x8c05fc08` |
| `smxUSBH-midi` task: entry / created at | `0x8c06bf40` / `0x8c06c0e0` |
| `smxUSBH-Blackfin` task created at | `0x8c06ffe2` |
| `smxUSBH-Hub` task: entry / created at | `0x8c07171c` / `0x8c071792` |
| Debug log call `(level, file, line, message)` | `0x8c042efc` (where the log goes is unknown) |
| Application host init function | `0x8d4d0dac` (calls `0x8d57c110` and `0x8d4d36a8`, not yet identified) |
| `smx_usb.cpp` glue, `usb_control.cpp` glue | around `0x8d4d09c2`, `0x8d4d514e` to `0x8d4d5ea0` |

The names `smxUSBH-*` and `smxUSBD-*` are task names. They are **not** class registrations.

### 9.3 Class driver registration (the key finding)

**Confirmed:** the audio class driver registers itself at `0x8c06c1ec` to `0x8c06c25a`:

- It fills a 28-byte driver record in RAM at `0x080112e4`, then calls `0x8c0734a4` with it.
- The record holds the driver's two callbacks: `+0xc` is `0x8c06a668` (the main entry, which logs `Found USB Audio`) and `+0x10` is `0x8c06aca4` (removal, which logs `Audio device removed`). `+0x14` points to a static template in read-only data at `0x84006ef0`, and several 16-bit fields are copied from that template.
- `0x8c0734a4` allocates a small object, stores it in the record, and links the record onto a list whose head is at RAM `0x080113d4` (through `0x8c071d2e`). It returns -1 if the allocation fails.

**Inferred:** the template at `0x84006ed0` holds a pointer to the name `usb-audio` and match records. They contain a flags value of 7 and the class and subclass bytes `01 01` (audio control) and `01 03` (MIDI streaming). So a host class driver appears to be "a record with probe and remove callbacks plus a class and subclass match table, linked onto a driver list". This has not been tested, and the matching code that walks the list has not been read.

The audio driver also keeps a small fixed table of per-device records, at RAM `0x080112d8`, indexed by device number.

**The stock drivers and the init order (from the host init at `0x8c073c00`).** It calls, in order: `0x8c073790`, the hub (`0x8c071750`), `0x8c0715a0`, `0x8c071b44`, the audio driver (`0x8c06c1a0`), `0x8c071a50`, and finally `0x8c070350`, which starts the host. The drivers are the hub (interface class 9), audio/MIDI (interface class 1, subclasses 1 and 3), a driver matched by vendor ID `0x05ac` (Apple, a role switch) and one matched by vendor ID `0x1a0a` (the USB-IF test mode). The vendor-ID entries have flags `0x0040` and the vendor ID at `+2`, which confirms the entry layout below. Which of the three unnamed calls registers which of these has not been matched up; it does not matter for the hook, which sits between `0x8c071a50` and `0x8c070350` (section 10).

**The matcher (inferred):** `0x8c0731e8(device, interface_index)` walks the driver list. For each driver it reads the match table at driver `+0x14`: an array of 12-byte entries ended by a zero flags word. Each entry is read as:

| Offset | Size | Field |
| --- | --- | --- |
| +0 | u16 | flags. Bits 3 to 7 select device-level comparisons (below), and bits 0 to 2 appear to select the interface class, subclass and protocol |
| +2, +4 | u16, u16 | vendor ID (flag bit 6), product ID (flag bit 7) |
| +6, +7, +8 | u8 | device class, subclass, protocol (flag bits 3, 4, 5) |
| +9, +10, +11 | u8 | interface class, subclass, protocol (flag bits 0, 1, 2; the audio entries are `01 01 00` and `01 03 00` with flags `0x0007`) |

The device-level comparisons are against the device structure's copy of the device descriptor: class, subclass and protocol at `+0x60` to `+0x62`, vendor ID at `+0x64`, product ID at `+0x66`. The interface-level comparison is made by `0x8c073132`, which has not been read. So a driver can bind by class, by vendor and product ID, or both.

When an entry matches, the matcher calls `SetConfiguration` (`0x8c0728bc`) with the current configuration's value and then calls the driver's probe through `[driver + 0xc]`. The other users of the list head are the registration function `0x8c0734a4` and a function at `0x8c073790`, which has not been read.

### 9.4 Enumeration and configuration selection (inferred)

The newly enumerated device is handled by `0x8c072ed4`. Reading it shows the stack does **not** just take configuration 1:

1. It sets the default control pipe's maximum packet size (64 if the speed field is 3, otherwise 8), reads the device descriptor (`0x8c072844`, an 18-byte minimum), assigns an address (`0x8c072726` is a call that precedes a 20 ms wait), and reads the descriptor again.
2. `0x8c072cbc` reads **all** of the device's configuration descriptors into an array of 60-byte records, pointed to by device `+0x70`. Device `+0x6d` is `bNumConfigurations`. In each record, `+4` is `bNumInterfaces`, `+5` is `bConfigurationValue`, `+8` is `bMaxPower`, and `+0xc` points to the 24-byte interface records.
3. The current configuration value (device `+0x78`) is first set to -1 (none).
4. **It then tries each configuration in order.** For configuration index 0, 1, ... it sets the current-record pointer (device `+0x74`) to that record and calls the binder `0x8c073aa0(device, pass)`, which calls the matcher for each interface. The first configuration for which a driver binds wins (the function returns 0). If none binds, it returns -2.
5. This is repeated for up to three passes, each with a different value from a three-word table at `0x840078f8`, which is passed to the binder. Their meaning is not decoded. If the vendor ID is `0x2367` (Teenage Engineering) and the product ID is `0x001d`, the first pass is skipped.

So a CDC-ECM driver that matches class `0x02`, subclass `0x06` should bind when the loop reaches configuration index 1 (value 2), after configuration index 0 (the vendor configuration) finds no driver.

`SetConfiguration` (`0x8c0728bc`, called only from the matcher) looks up the configuration value in the 60-byte records, skips the request if it is already current, otherwise sends the standard `SET_CONFIGURATION` request (`bRequest` 9, 3000 ms timeout) through the control-transfer function `0x8c07389c`, and then opens the endpoints of the new configuration (`0x8c071f2a`). It first checks power: when the global flag byte at `0x84007874` is 0 and the device's `+2` byte is 0, a configuration whose `bMaxPower` is above 50 (100 mA in high-speed units) is refused with -23. `SetInterface` (`0x8c0729b8`, `bRequest` 11, 5000 ms timeout) is the matching call that class drivers use for alternate settings. The audio driver calls it from about ten places.

### 9.5 The driver interface (inferred)

All of this comes from reading the audio driver and the stack's core, and has not been exercised.

- **Probe.** The matcher calls `[driver + 0xc]` as `probe(device, interface_index, match_result)`. A non-zero return claims the interface, and zero lets the matcher try the next entry. After a successful probe the stack stores the driver and the return value. When the device is removed (`0x8c073976`) it calls `[driver + 0x10]` as `remove(device, context)`, with that stored return value as the context.
- **Interface match.** `0x8c073132` compares an entry's interface fields with the interface descriptor: `entry+9` against `bInterfaceClass` (descriptor offset 5), `entry+10` against `bInterfaceSubClass`, and presumably `entry+11` against `bInterfaceProtocol`. Interface descriptors are 12-byte records indexed from the interface array. A table ends with an entry whose vendor ID, device class and interface class are all zero.
- **Control transfers.** `0x8c07389c(device, pipe, bRequest, bmRequestType, wValue, wIndex, data, wLength, timeout_ms)` builds the 8-byte setup packet, submits it, waits on a semaphore for the timeout, and returns the status (negative on error, for example -22 when an allocation fails). The `pipe` word for the default control pipe is the device address in bits 8 to 14, plus bit 26 set when the device's speed field is 1. `SetConfiguration` and `SetInterface` are two callers: they pass `bmRequestType` 0 and 1, `wValue` the configuration or alternate setting, and `wIndex` the interface. An ECM class request such as `SetEthernetPacketFilter` (`bmRequestType` `0x21`, `bRequest` `0x43`) would be one more call of this function.
- **Requests.** `0x8c0736e4(iso_packet_count)` allocates a zeroed request: 80 bytes plus 16 bytes per isochronous packet descriptor, with a semaphore at `+0x44` and the packet array pointed to by `+0x4c`. It returns 0 on failure. `0x8c0737e4(request)` is the generic submit. It finds the host controller driver through the request's device (`+0x48`) and calls its submit operation. The request object has the pipe word at `+0x8`, a status at `+0x10` (preset to -20 while pending), the setup packet pointer at `+0x28`, the data buffer at `+0x2c`, a driver context pointer at `+0x34`, and a completion callback at `+0x40`. The audio driver fills these fields itself for its isochronous and bulk endpoints and calls the submit function from about ten places. Its completion callbacks keep rings of preallocated requests and read the result from the request (the field at `+0x18` is read as the transferred length, which is inferred).
- **Bulk and interrupt requests.** In the audio driver's probe (`0x8c06a668`), each endpoint gets a preallocated request (`0x8c0736e4(0)`), initialised once and reused:
  - The pipe word at `+0x8` is the device address in bits 8 to 14, the endpoint number in bits 15 to 18, bit 26 set when the device's speed field is 1 (a low-speed flag), and bit 31 set. For an IN endpoint, bit 7 is set as well.
  - The transfer length is at `+0x14` (the audio driver stores the endpoint's `wMaxPacketSize`), the data buffer at `+0x2c`, a driver context pointer at `+0x34`, the device at `+0x48`, and the completion callback at `+0x40`. The field at `+0xc` is set to 8 in the loop that creates the requests, and its meaning is unknown.
  - The completion callback is called with the request. It reads the driver context from `+0x34` and the result from `+0x18` and `+0x2c`, and then hands them to the driver's own handler.
  - The driver resubmits the same request after each completion, so a steady stream is a request that is resubmitted from its own callback.
- **Memory.** `0x8c073ba0(size)` allocates and returns 0 on failure, and `0x8c073be4(pointer)` frees. They are used for request objects and driver structures.

### 9.6 What has not been found

- The meaning of the three passes in the enumeration loop and what the binder does with the pass value, and the function at `0x8c073790`. The smx core has no log strings, which is what the string-based search relied on.
- How a driver builds a bulk or interrupt request is now mostly known (section 9.5). Still unknown: the meaning of the request field at `+0xc`, the type of the transfer (bulk, interrupt or isochronous) and how it is chosen, buffer alignment and DMA requirements, the context the completion callback runs in (interrupt or task), the largest transfer size, and how a request is cancelled.
- Searches that found nothing: the standard setup-packet bytes (`80 06 00 01 ...`) as data (the packets are built in code), and direct pointers or calls to the audio driver functions (their addresses are loaded as immediates).
- Whether the stack enumerates this adapter at all on the OP-XY, what it does with an interface no driver claims, and the OP-XY side of the adapter's descriptors (a high-speed view, power). That needs a device experiment.

### 9.7 The adapter's descriptors (read from a Mac with libusb)

A read-only `GET_DESCRIPTOR` of the device and both configurations, over a SuperSpeed (5 Gb/s) link. `bcdUSB` is 0x0300, `bcdDevice` is 0x3000 and the device class is 0. macOS reports 256 mA allocated.

| | Configuration value 1 | Configuration value 2 (macOS selects it) |
| --- | --- | --- |
| Mode | Vendor-specific, `0xff/0xff/0x00` (the `r8152` mode) | **CDC-ECM** |
| Interfaces | 1 | 2 |
| Endpoints | bulk IN `0x81`, bulk OUT `0x02`, interrupt IN `0x83` (2 bytes) | control interface (class `0x02`, subclass `0x06`): interrupt IN `0x83` (16 bytes); data interface (class `0x0a`) alternate 1: bulk IN `0x81`, bulk OUT `0x02` (alternate 0 has none) |
| Descriptors | | CDC header, union, and Ethernet networking (MAC address in string index 3, maximum segment 1514 bytes) |
| `bMaxPower` | 0x20 (256 mA in SuperSpeed units) | 0x20 (256 mA) |

Each endpoint also has a SuperSpeed companion descriptor. The OP-XY's controller is USB 2.0 high speed, so expect 512-byte bulk packets, no companion descriptors and possibly a different power value. The interface layout is expected to be the same but is unverified.

### 9.8 Consequences for the plan

- **ECM path (preferred).** ECM carries raw Ethernet frames over the bulk endpoints and needs no Realtek register programming. A driver needs `SET_INTERFACE 1/1`, the `SetEthernetPacketFilter` class request, the MAC address from string 3, and the bulk and interrupt transfers. The stack's enumeration loop (section 9.4) tries every configuration, so registering a driver for class `0x02`/`0x06` should make it bind on configuration value 2 without any special code to select it. This still has to be confirmed on the device, along with the power check: the adapter's `bMaxPower` is 0x20, which is 64 mA in high-speed units and below the 100 mA limit, if it reports the same value at high speed.
- **Vendor path (fallback).** Configuration 1 is tried first, and a driver matching vendor ID `0x0bda`, product ID `0x8153` would bind there. It would have to speak the Realtek vendor protocol, which is the `r8152` port the original plan described, and is a much larger task.
- **Networking.** A network stack (lwIP or similar) must be added either way. Nothing in the firmware provides one.
- **Where code would go.** The SDK's model is a linked module with reviewed hooks into stock code. A new host class driver would be built as module code that fills a driver record and registers it (section 9.3), so it needs a hook at a startup point that runs after the stack is initialised. The audio driver registers from within the host task's startup, around `0x8c06c1ec`.

### 9.9 Next steps

Done in section 10: the diagnostic, the transfers, the ECM driver and the IP stack.

## 10. Proof of concept: a host driver hook, a diagnostic and an Ethernet client

Goal: show that code running on the OP-XY can use the USB port and an Ethernet adapter to read a file from the Internet. The sections below keep the order things happened in, including the dead ends (the log file that never appeared, the failed control transfers); 10.4m and 10.4n are the final state.

### 10.1 What was built

In the SDK (`etcetc000/open-xy`, merged as #548; written on a `usb-host-poc` branch):

- **Public API** `sdk/include/opxy/usb_host.hpp` in namespace `opxy::usb`: `Match` entries (flags `match::interface_class`, `vendor`, `product` and so on, mirroring section 9.3), a `Device` view (speed, address, vendor and product ID, class triple), `Driver{name, matches, probe, remove}`, `Registry::add`, and `Services::log`. The extension hooks gain an eighth event, `usb_host_ready` (`Hooks::usb_host_ready`, `with_usb_host_ready`).
- **Hook** in `profiles/opxy-1.2.1.json`: an eight-byte absolute call at `0x8c073c34`, which replaces `CALL 0x8c071a50; CALL 0x8c070350` (the last driver registration and the host start, section 9.2) with `sdk_extensions_usb_host_init`. That function makes the same two calls, with the extension drivers registered between them, and returns the second call's result. It is the one point where the driver list exists, every stock driver is registered and no device has been enumerated yet.
- **Glue** `sdk/platform/opxy-1.2.1/usb_host.cpp`: builds a 28-byte stock driver record per extension driver (at most two) and registers it with `0x8c0734a4`. The record's probe and remove are small per-slot trampolines that translate the stock device structure into the public `Device` and pass the probe's return value on as the remove context. `Services::log` goes to the device's own log through the stock logger. (The first version wrote a file, `usbhost.oxy`, from the GUI tick; it never appeared on the device, section 10.4a, and was removed.)
- **Build wiring**: `tooling/native.py` (`usb_host` unit in the `extensions` service), and the pinned profile hash in `builder/core.mjs`.
- **Diagnostic extension** `extensions/usb_probe` (this repository): one driver that matches the adapter by vendor and product ID (`0bda:8153`). Its probe logs what it is given (vendor and product ID, speed, address, device class triple, the configuration being tried and the count, the current configuration, interface count, `bMaxPower`, interface index) and **returns 0**, so the stack goes on to the next interface or configuration exactly as it does without it. Its remove logs the same line.

### 10.2 Verification before the first device run (host only)

- A host fixture (`tests/fixtures/usb_host.cpp`, `tests/test_usb_host.py` in the SDK) fakes the stock register, file and clock calls and a device image. It checks the event order, the two-driver limit, probe and remove forwarding with the context, the log flush and its overflow, and a refused registration.
- `extensions/usb_probe/tests/probe.cpp`: passes (`sdk.py test --extension extensions/usb_probe`).
- The SDK's own check: 610 tests pass, with and without the stock OS image (`OPXY_TEST_STOCK_OS`); with it, the profile's `before` bytes at `0x8c073c34` are checked against the real `os.ldr`. Eight tests that need the private package are skipped.
- A scratch firmware build with the Splitter Player machine and `usb_probe` completed (`../firmware/dev/003`, experimental, device operations off). The patched bytes at `0x8c073c34` decode to a call to `sdk_extensions_usb_host_init`.

### 10.3 Trade-offs and risks

- Event hooks in this SDK use the `extensions` service, so the host hook is present in **every** build that links any extension. With no registered driver it calls the same two stock functions in the same order; it does write `usbhost.oxy` (two status lines), which a release would drop. A maintainer might prefer a service of its own.
- `probe` and `remove` run on the host stack's task, not the GUI: they must not block or allocate. The log is therefore a bounded buffer flushed from the GUI.
- The two stock calls at the hook site were read from a disassembly, never run. A wrong argument or register assumption would show up as a crash at boot of the USB host; the diagnostic is the way to find out safely (a device build is a scratch build, and the stock OS can be restored).
- The probe returns 0 and never claims: if the matching or the log is wrong, the stack is not disturbed beyond the registration itself.

### 10.4 What the first device run will answer

(Superseded by 10.4a: the lines come from the device's own `logs/` file, not `usbhost.oxy`.)

Plug the adapter into the OP-XY, boot a scratch build with `usb_probe`, then read `usbhost.oxy`. Expected results and what each means:

| Log | Meaning |
| --- | --- |
| No file | The file was not written or not listed: the build is not the one installed, the controller-update hook did not run, the file system refused the file, or the MTP view is stale (reconnect it) |
| `usb host: gui tick` only | The log works, but the host init hook never ran: the stack did not start, or the hook site is wrong |
| `usb host: hook ran, drivers=1`, no `probe` | The driver is registered but the stack never offered it the adapter: not enumerated, or not matched |
| `probe` lines with `cfg=1/2` and then `cfg=2/2` | The stack reached both configurations, so a real ECM driver would be offered configuration 2. `spd=3` is high speed; `pwr` is the `bMaxPower` the stack saw |
| `probe` lines with `cfg=1/2` only | The stack stopped after configuration 1, for example because the power check refused configuration 2 (section 9.4) |
| Nothing logged though the adapter is powered | Enumeration failed before the driver list was consulted |

This needs the user's explicit instruction each time, as `AGENTS.md` requires for any device operation.

### 10.4a First device runs

Scratch builds `003` and `004` were installed by the user with the adapter plugged in. **No `usbhost.oxy` appeared** over MTP (`004` also wrote a `gui tick` line at the first tick, so the file route itself is suspect: the MTP listing, the path or `fopen`). The device keeps its own log, `logs/log_<date>_<n>.txt`, a new file each boot, which is the better channel. Its USB lines (`grep usb`) from one boot, with the adapter attached from before the boot:

| Time (s) | Line |
| --- | --- |
| 8.97 | `usb_control: usb attached: power:[to sink] data:[host]`, then `init host driver` |
| 9.12 | `smx_usb: Usb host init done` (the host init the hook sits in, so the stack did start at boot) |
| 17.5 | `usb attached: power:[to source] data:[host]` three times (the OP-XY now supplies VBUS) |
| 17.8 | `stop usb driver: 10`, `usb detached`, `pd identity cleared (disconnect)`, `restart hostdriver! mode:0` |
| 20.07 | `data:[device]`, `init device driver: 1` (the computer's MTP connection) |

What it shows: the stack starts the host at boot, and the stack **restarts the host on role or connection changes** (`restart hostdriver!`). The stock smx core has no log lines, so enumeration of the adapter is invisible there. A restart rebuilds the driver list, so the glue now registers again on every host init (earlier builds registered once, so a restart dropped the driver). The glue also sends its lines to the stock logger (`0x8c042efc(level, file, line, format, ...)`, level 3 as the stock's usual calls, 451 call sites in the image), so they land in the same `logs/` file as `usb_probe[1]`. Scratch build `005` does that.

### 10.4b Result of build 005 (hook confirmed on the device)

```text
8.940 usb attached: power:[to sink] data:[host]   /  init host driver
8.940 usb_probe[1]: usb host: hook ran, drivers=1
9.081 smx_usb[867]: Usb host init done
9.110 usb_probe[1]: usb host: gui tick
9.265 usb_probe[1]: probe 0bda:8153 spd=3 adr=1 cls=00/00/00 cfg=1/2 cur=1 ifc=1 pwr=90 i=0
9.265 usb_probe[1]: probe 0bda:8153 spd=3 adr=1 cls=00/00/00 cfg=2/2 cur=2 ifc=2 pwr=90 i=0
```

Established on the device:

- **The hook runs** at host init, before `Usb host init done`, and the driver registers.
- **The stack enumerates the adapter at high speed** (`spd=3`, address 1) and offers it to a registered driver. Both configurations are tried, in order: value 1 (vendor mode, one interface), then value 2 (two interfaces, the ECM layout). A driver that matches the ECM control interface (class `0x02`, subclass `0x06`) would be offered configuration 2.
- **The power check did not refuse configuration 2**: the stack issued `SetConfiguration` for both (`cur` is 1, then 2). `bMaxPower` is 90 here, 180 mA in high-speed units, against 0x20 in the SuperSpeed view on the Mac (section 9.7).
- The adapter is probed about 0.18 s after the host init, with no further action by the user.
- The driver declined both, and the stack left no visible trace of what it did next; no `remove` line appeared.

Build 006 adds a control-transfer service to the API (`Services::control`, which calls the stack's `0x8c07389c` with the default pipe) and makes the diagnostic read both configuration descriptors and the MAC string (index 3) with `GET_DESCRIPTOR`, logging the bytes as hex. That answers what the high-speed descriptors say, and tests the control path an ECM driver needs.

### 10.4c Result of build 006: control transfers fail with -13

```text
9.261 usb_probe[1]: cfg 0 ret=-13
9.261 usb_probe[1]: cfg 1 ret=-13
9.262 usb_probe[1]: str 3 ret=-13
```

All three `GET_DESCRIPTOR` reads (255 bytes requested) failed within 0.1 ms, far short of the 1 s timeout. Reading the stack: the call's argument order is confirmed against `SetConfiguration` and `SetInterface` (pipe word, setup packet fields, data, length, timeout), and the status is set by the host controller's interrupt handlers (`0x8c06f150` and the code at `0x8c06f18e`, `0x8c06f302`, `0x8c06f5bc`, `0x8c06f814` write -13 into the request's status when a pipe error bit is set), so the transfer reached the hardware and failed. The stock's own descriptor reads (`0x8c072844`) read the 9-byte header first and then exactly `wTotalLength`, never a longer length, so build 007 does the same (header, then exact length), to find out whether a short packet is the cause.

### 10.4d Build 007 also fails with -13; the pipe word's direction bit

`cfg 0 ret=-13/-13` again with the exact 9-byte header read, so short packets were not the cause. The stack's own `GET_DESCRIPTOR` wrapper (`0x8c0727b8`) builds the pipe word as `address<<8 | lowspeed<<26` and **sets bit 7 for the IN direction** (`BITSET (R1, 0x7)`), which `SetConfiguration` (an OUT request) never does and my glue left out. The hardware handlers write -13 on a direction/pipe error. Build 008 sets bit 7 when `bmRequestType` has its direction bit. The wrapper also retries a failed read three times, except on -13, which it returns at once.

### 10.4e Result of build 008: control transfers work; the adapter's descriptors at high speed

The direction bit was the cause: both reads succeed (`cfg 0 ret=9/39`, `cfg 1 ret=9/80`, `str 3 ret=2/26`), from the probe, with a static 32-byte aligned buffer. The raw bytes, decoded:

| Configuration value 1 (39 bytes): vendor mode | |
| --- | --- |
| `09 02 2700 01 01 00 a0 5a` | 1 interface, value 1, bus powered with remote wakeup, `bMaxPower` 0x5a = 90 (180 mA) |
| `09 04 00 00 03 ff ff 00 00` | interface 0, alt 0, 3 endpoints, class `ff/ff/00` |
| `07 05 81 02 0002 00` | bulk IN `0x81`, 512 bytes |
| `07 05 02 02 0002 00` | bulk OUT `0x02`, 512 bytes |
| `07 05 83 03 0200 08` | interrupt IN `0x83`, 2 bytes, interval 8 |

| Configuration value 2 (80 bytes): CDC-ECM | |
| --- | --- |
| `09 02 5000 02 02 00 a0 5a` | 2 interfaces, value 2, same power (180 mA) |
| `09 04 00 00 01 02 06 00 00` | interface 0, alt 0, 1 endpoint, class `02/06/00` (ECM control) |
| `05 24 00 1001`, `05 24 06 00 01` | CDC header (1.10), union (control 0, data 1) |
| `0d 24 0f 03 00000000 ea05 0000 00` | Ethernet networking: MAC in string 3, no statistics, maximum segment 0x05ea = 1514, no multicast or power filters |
| `07 05 83 03 1000 08` | interrupt IN `0x83`, 16 bytes, interval 8 (link and speed notifications) |
| `09 04 01 00 00 0a 00 00 00` | interface 1 (data), alt 0, no endpoints |
| `09 04 01 01 02 0a 00 00 00` | interface 1, alt 1, 2 endpoints |
| `07 05 81 02 0002 00`, `07 05 02 02 0002 00` | bulk IN `0x81` and bulk OUT `0x02`, 512 bytes each |

The MAC string (`1a 03` then UTF-16) is `A0CEC812D660`, so the adapter's address is `A0:CE:C8:12:D6:60`.

So the high-speed layout is the Mac's SuperSpeed layout without companion descriptors and with 512-byte bulk packets, as predicted in section 9.7. An ECM driver needs, in this order: claim interface 0 (class `02`/`06`) and, with a second match entry, interface 1 (class `0a`); `SetInterface` data interface 1 to alternate 1; `SetEthernetPacketFilter` (`bmRequestType` `0x21`, `bRequest` `0x43`, `wValue` = directed, broadcast and multicast bits, `wIndex` 0); bulk IN `0x81` and bulk OUT `0x02` requests; and optionally the interrupt IN `0x83` for the link state (`NetworkConnection` notification). Control transfers, which the first three steps need, now work. Bulk and interrupt requests are still the open point (section 9.6).

### 10.4f Bulk and interrupt requests, and the ECM driver (build 009)

Reading the stock audio driver's MIDI endpoints (`0x8c06a668`), the request allocator (`0x8c0736e4`), the control function (`0x8c07389c`) and its completion (`0x8c07347c`) gives, for a request object `r`:

- `r+0x08` the pipe word: device address in bits 8 to 14, endpoint number in bits 15 to 18, bit 26 for low speed, bit 7 for IN, and **the transfer type in bits 30 to 31**: the endpoint descriptor's `bmAttributes` type (control 0, isochronous 1, bulk 2, interrupt 3). The audio MIDI pipes set only bit 31 (bulk); the control pipes have 0; the controller's code tests `pipe >> 30 == 3` (interrupt). This reading is inferred, not yet exercised for bulk.
- `r+0x0c` is set to 8 by the audio driver and is still unexplained; `r+0x14` the length, `r+0x18` the actual length on completion, `r+0x10` the status (-20 while pending, negative on error), `r+0x1c` -1, `r+0x2c` the data pointer, `r+0x34` a context, `r+0x40` the completion callback, `r+0x44` a binary semaphore the allocator creates, `r+0x48` the device.
- The control function submits with `0x8c0737e4`, waits on the semaphore with `0x8c03e6f4(semaphore, timeout)` (returns 0 on timeout), cancels a timed-out request with `0x8c073840(request)`, then reads the status and length and frees the request with `0x8c07375c`. Its completion callback, `0x8c07347c`, gives the semaphore (from the controller's interrupt context), so the same callback serves any waited request.
- Per-endpoint packet sizes are collected from the descriptors when an alternate setting is opened (`0x8c071f2a`): a table of 16 sizes per direction in the device structure at `+0x38` and `+0x18`; `SetInterface` (`0x8c0729b8(device, interface, alternate)`) looks the interface record up with `0x8c072000` and issues the request.

The SDK glue now offers this as `Services::set_interface` and `Services::transfer` (a waited bulk or interrupt transfer built exactly as above). Unproven: that a bulk request needs nothing more, that `r+0x0c` = 8 suits our endpoints, and that the timeout cancel works.

Build 009 carries the `usb_ecm` extension (this repository) as its only USB extension. It matches interface class `02/06` and class `0a`, claims both, and when the data interface (index 1) is probed it runs: `SetInterface(1, 1)`, `SET_ETHERNET_PACKET_FILTER` (`0x21`, `0x43`, directed and broadcast, interface 0), and one bulk IN read of 1536 bytes on `0x81` with a 3 s timeout, logging `ecm set-interface`, `ecm filter`, `ecm bulk-in` and, if a frame arrived, its first 14 bytes. It is a probe of the path, not a network driver. A cable to a live network is needed to see a frame; with none, `bulk-in ret=-14` (a timeout) is the expected result.

### 10.4g Result of build 009: the OP-XY receives Ethernet frames

No cable (adapter only):

```text
ecm probe i=0 cfg=2 spd=3
ecm probe i=1 cfg=2 spd=3
ecm set-interface ret=0
ecm filter ret=0
17.05 ecm bulk-in ret=-14     (3.000 s after the filter: the timeout)
```

With a live Ethernet cable (boot to boot):

```text
9.413 ecm set-interface ret=0
9.413 ecm filter ret=0
9.597 ecm bulk-in ret=60
9.597 ecm frame ffffffffffffd2a2946723380806
```

The frame is 60 bytes: destination `ff:ff:ff:ff:ff:ff` (broadcast), source `d2:a2:94:67:23:38`, EtherType `0x0806` (ARP), padded to the Ethernet minimum. It arrived 0.18 s after the filter was set.

Established on the device: the claim, `SetInterface` and the class request work; **a bulk IN transfer built by the SDK glue completes and delivers a real frame**; the timeout and cancel path works without upsetting the stack (the host kept running and returned -14 on time). The pipe word's type bits (bulk 2 in bits 30 to 31) and `r+0x0c` = 8 are therefore good enough for bulk IN. Not yet exercised: bulk OUT (transmit), interrupt IN, and repeated or concurrent requests.

### 10.4h Build 010: DHCP

Reading from the Internet still needs the OP-XY to **send**: it has to learn an address (DHCP), ask for the gateway's hardware address (ARP), resolve a name (DNS), open a connection (TCP SYN) and ask for the file (the HTTP request). DHCP cannot be skipped unless the address is configured by hand, which would tie the proof of concept to one network, so it is the next step and also the first test of bulk OUT.

`usb_ecm` in build 010 reads the MAC address from string descriptor 3, then broadcasts a DHCP DISCOVER (UDP 68 to 67, with the broadcast flag set, since there is no address yet), waits for an OFFER (ignoring other traffic, 2 s per read, three tries), sends a REQUEST for the offered address and waits for the ACK, logging the address, server, mask, router, DNS and lease time. The frames are built by hand (Ethernet, IPv4 with its checksum, UDP without one, BOOTP, options); there are no library dependencies and all buffers are static, because the host stack's task has a small stack. Expected log: `ecm mac`, `ecm dhcp-discover ret=<bytes>` (the first bulk OUT), `ecm dhcp offer ...`, `ecm dhcp-request ret=<bytes>`, `ecm dhcp bound ...`.

### 10.4i Result of build 010: DHCP works, in both directions

```text
9.250 ecm mac a0cec812d660
9.251 ecm dhcp-discover ret=301          (the first bulk OUT: 301 bytes sent)
9.518 ecm dhcp offer ip=192.168.1.21 server=192.168.1.1 lease=86400
9.518 ecm dhcp mask=255.255.255.0 router=192.168.1.1 dns=192.168.1.1
9.519 ecm dhcp-request ret=313
9.588 ecm dhcp bound ip=192.168.1.21 server=192.168.1.1 lease=86400
```

The OP-XY obtained a lease from the router (0.27 s for the offer, 0.07 s for the acknowledgement). So **bulk OUT works**, the hand-built Ethernet, IPv4 and UDP frames are accepted, and replies are parsed, all on the host stack's task.

### 10.4j Build 011: ARP, DNS and a plain HTTP GET

After the lease, `usb_ecm` runs the rest of the chain once, logging each step: ARP for the next hop (the router, or the host itself on our subnet; it also answers ARP requests for our address while waiting), a DNS A query for `example.com` to the DHCP-provided server, ARP for the next hop of the answer, a TCP connection to port 80 (SYN with MSS 1460, ACK of each in-order segment, a duplicate ACK for a gap or repeat, FIN handled), and `GET / HTTP/1.1` with `Connection: close`. The response (up to 8 KB) is held in memory and its first 16 lines are logged. It is plain HTTP: HTTPS would need a TLS client, a much larger piece, so the proof of concept reads a file over HTTP. Expected log: `ecm dns example.com=<address>`, `ecm tcp connected`, `ecm http bytes=<n> closed`, then `ecm http | HTTP/1.1 200 OK` and the headers and first lines of the page.

### 10.4k Result of build 011: a web page read over the USB Ethernet adapter

```text
14.375 ecm dhcp bound ip=192.168.1.21 server=192.168.1.1 lease=86400
14.414 ecm dns example.com=172.66.147.243
14.454 ecm tcp connected
14.508 ecm http bytes=932 closed
14.508 ecm http |HTTP/1.1 200 OK
       ... Content-Type: text/html; charset=utf-8, Transfer-Encoding: chunked, Server: cloudflare ...
14.508 ecm http |241                                   (the first chunk's size, in hex)
14.508 ecm http |<!doctype html><html lang=en><head><meta charset=utf-8> ...
```

**The proof of concept is met.** Code running on the OP-XY, through the stock USB host stack and one SDK hook, used a standard USB-C Ethernet adapter to obtain an address, resolve a name, open a TCP connection to a server on the Internet and read an HTTP response, 932 bytes in all, closed cleanly by the server. From the lease at 14.375 s to the end of the response took 0.13 s.

What the run does not yet show, and what a usable feature would need:

- **The body is raw.** The server used chunked transfer encoding, so the first body line is a chunk size (`241`), not content. Handling that is a few lines.
- **The result is only logged.** Nothing is stored: the response is held in a static 8 KB buffer and the first lines go to the device log. Writing the body to a file under `/fat32` (the glue already uses the stock `fopen`/`fwrite`/`fclose`) would make it a file read off the Internet.
- **It runs inside the host stack's `probe`**, once, at boot, blocking that task. A real feature needs its own task or trigger (a menu action, a pattern for a file name), and must not run at enumeration time.
- **Plain HTTP only.** Most hosts redirect to HTTPS; a TLS client is the next large piece, and this one example works because `example.com` still answers on port 80.
- **One connection, no retransmission of our own data**, a fixed host and path, and a stop-and-wait style receive. These are enough for small files.
- **The SDK side is a fork.** The event, the hook at `0x8c073c34` and the transfer services live on the `usb-host-poc` branch of `../open-xy` and are not accepted by the maintainer; their cost and review points are in sections 10.1 and 10.3.

### 10.4l Next stage: count the ids of a JSON file, from an extension on a key

Target: read `http://open-xy.com/machines.json` (the SDK's `catalog/machines.json`, 75 KB, 95 machines today), list the `id` of each entry of its `machines` array and report how many were found; until the site serves it, do the same for `http://jsonplaceholder.typicode.com/users` (a JSON array of users); show the progress on the device, from an extension triggered by a key chord.

Done (build 012, not yet run): the read is now streaming. A small HTTP reader decodes the status line, `Transfer-Encoding: chunked` or `Content-Length`, and a redirect's `Location`; a streaming JSON scanner counts the objects of the target array (the root array, or the array under a named key) and keeps each element's top-level `id`; neither stores the response, so 75 KB costs no memory. The log gets `ecm http status=... bytes=... body=... chunked complete`, `ecm ids: ...` lines and `ecm json elements=<n> ids found=<n>`. `kTargets`/`kTarget` in `extension.cpp` choose the URL.

The change in `../open-xy` that would have published `catalog/machines.json` on open-xy.com was dropped: the proof of concept reads `http://jsonplaceholder.typicode.com/users` instead, which is enough to show the idea (10.4m).

Open: **what the extension can show and how it is triggered.** The current SDK (staging at `ba943a8`) has no extension surface other than `Actions::notify` (one popup of 1 to 31 characters, only inside the callback of a claimed key) and only one claim in `profiles/extensions.json`, `record_double_tap` (Record). The layer claims (a gesture such as −/+ opening the extension's layer) and the canvas (`Hooks::layer`, `Hooks::draw`, `layer.svg.fragment`) of #343 are in history (commits `733aebc` to `0b15017`, 35 files in the first one, mixed with snapshot code) but are **not in staging**: the "core split" (`4241cd3`, #360) left them behind with the Snapshots extension (open-xy-core-machines #15). Using them means either porting them into the PoC branch, or a smaller SDK change of our own.

Decision (user): for now use what staging has: the **Record double-tap claim** as the trigger and **one popup** for the answer. Build 013 does that: `usb_ecm` brings the adapter up and leases an address from the host stack's probe at boot (the lease is retried by the key if the cable came later), and a double-tap on Record, handled in `queued_control` with the SDK's `DoubleTap`, runs the DNS, TCP, HTTP and JSON steps and shows `<n> ids, <bytes> bytes` or the reason it failed. The popup is the only on-screen output, and the whole read runs inside the callback, blocking the controller path (a proof of concept: a task of its own is the proper design). Moving to the 4-line screen means the layer and canvas port described above.

### 10.4m Outcome of build 013, and what was merged

Build 013 on the device: a double-tap on Record read the jsonplaceholder users list and showed the result in the popup (reported by the user: it works). The SDK side went in as #548 with the limits its description lists: the hook is in every build that links an extension, at most two drivers, the services block, and what was not exercised on a device (the `remove` path, re-registration after a host restart, interrupt transfers, low-speed devices). The read still runs inside the key's callback and blocks the controller path; a real feature needs a task of its own.

### 10.4n What in `usb_ecm` depended on this adapter, and what does now

An audit of the extension for values that would only match this adapter or this network. Hard-coded before: the MAC's string index (3) and language (`0x0409`), the interface numbers (0 and 1), the data interface's alternate setting (1), the bulk endpoint addresses (`0x81`, `0x02`) and the 512-byte packet size behind the zero-length-packet rule and the read length. Not hard-coded, then or now: the vendor and product ID (the driver matches the interface classes `02/06` and `0a`, so any CDC-ECM adapter is offered), the MAC address (read from the adapter), the IP address, router and DNS server (DHCP), and the server's address (DNS).

Now the driver reads the configuration descriptor being tried (found by its `bConfigurationValue`) and takes from it: the ECM control interface (class `02`, subclass `06`), the first alternate setting of a data interface (class `0a`) that has endpoints and its bulk IN and OUT addresses and `wMaxPacketSize`, and the MAC's string index from the Ethernet networking functional descriptor; the language is the first one string descriptor 0 lists. The zero-length-packet rule and the read length (whole packets, at most 1536 bytes) follow `wMaxPacketSize`, so a full-speed adapter's 64 bytes work as well as 512. `SetInterface` and the packet filter are addressed with the parsed numbers. A configuration it cannot understand (vendor mode, no data interface, no MAC string) is declined, so the stack tries the next configuration, as it did before the driver existed.

What remains assumed: the adapter offers a CDC-ECM configuration (an RTL8153 does; RNDIS or vendor-only adapters are not handled); an Ethernet frame is one bulk transfer, as ECM defines; the network gives a lease by DHCP; IPv4 only. Interface numbers are taken to equal the stack's interface index, which the USB specification requires within a configuration. Only the RTL8153 has been tried on a device.

Tests (`extensions/usb_ecm/tests/ecm.cpp`, with fake services and a fake network that answers ARP, DHCP, DNS and TCP): the RTL8153's real descriptor bytes give its layout; a differently laid out full-speed adapter (other endpoints, string index, language, 64-byte packets) is driven end to end with every frame accepted by its packet rule; a vendor-mode configuration is declined; the full read of a chunked response split across segments, a plain one, and the catalog's nested shape (only each entry's own `id`); each failure's popup (redirect, no ids, no connection, no DNS answer, no lease, no adapter); and that a tap that is not a double-tap does nothing.

### 10.5 What is left

1. The paths a device has not shown: unplugging (`remove`), the re-registration after a host restart, interrupt IN for link state, another adapter.
2. Move the read out of the key callback onto a task of its own (an SDK question).
3. HTTPS needs a TLS client; most hosts redirect plain HTTP to it (the extension reports the redirect).
4. A screen instead of a popup needs the layer and canvas code of #343, which is not in the SDK (the "core split" left it with the Snapshots extension).

## Final objective

Determine whether the OP-XY can use a standard USB-C Ethernet adapter to obtain wired network connectivity, and if so, implement the necessary support in its Blackfin firmware.

The first target is the Realtek RTL8153 adapter identified above. The long-term goal should be a reusable USB Ethernet implementation, rather than a one-off solution tied exclusively to this particular CableCreation adapter.
