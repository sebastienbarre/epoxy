# USB ECM

An experimental CDC-ECM host driver, and a proof that an OP-XY can read a file from the Internet
through a USB-C Ethernet adapter. It needs the SDK's `usb_host_ready` event and USB host services
(OS 1.2.1) and works with any adapter that offers a CDC-ECM configuration; it was tried on a
CableCreation adapter with a Realtek RTL8153.

**At boot** (the stock USB host stack probes the adapter) it reads the adapter's configuration
descriptor and takes everything it needs from it: the ECM control interface (class `02/06`), the
data interface (class `0a`) with its alternate setting and bulk endpoints and their packet size,
and the string holding the MAC address. It claims both interfaces, selects the data interface's
alternate setting, enables directed and broadcast frames, reads the MAC address and asks DHCP for
an address. A configuration it cannot understand (vendor mode, no data interface) is declined, so
the stack goes on to the next one. It matches interface classes, never a vendor or product ID.

**On a double-tap of Record** (the extension claims `record_double_tap`; stock still records) it
reads one JSON file over plain HTTP, counts the objects of an array in it, collects their `id` and
shows the outcome as a popup, for example `<n> ids, <bytes> bytes`, or `No DHCP lease`,
`DNS lookup failed`, `No answer from the router`, `Could not connect`, `Connection refused`,
`HTTP status 301`, `No ids in the response`, `No USB Ethernet adapter`.

What it reads is `kTargets[kTarget]` in `extension.cpp`: `http://jsonplaceholder.typicode.com/users`
(a JSON array of users) or `http://open-xy.com/machines.json` (the `machines` array of the
catalog; the site does not serve it). Plain HTTP only: there is no TLS, and a redirect to HTTPS
shows as `HTTP status 301` with a `location` line in the log.

The device's log (`logs/log_*.txt` over MTP; `grep usb_host_ext`) has the detail:

```text
ecm probe i=0 cfg=2 spd=3
ecm probe i=1 cfg=2 spd=3
ecm link control=0 data=1 alt=1 in=0x81 out=0x02 packet=512 mac-string=3
ecm set-interface ret=0
ecm filter ret=0
ecm mac <12 hex digits>
ecm dhcp-discover ret=<bytes sent>
ecm dhcp offer ip=<address> server=<address> lease=<seconds>
ecm dhcp bound ip=<address> server=<address> lease=<seconds>
ecm dhcp mask=<address> router=<address> dns=<address>
-- on the double-tap --
ecm dns <host>=<address>
ecm tcp connected
ecm http HTTP/1.1 200 OK
ecm http status=200 bytes=<n> body=<n> chunked complete
ecm ids: <id> <id> ...
ecm json elements=<n> ids found=<n>
```

`ecm: not a CDC-ECM adapter this driver understands` after the `ecm link` line means the
configuration lacked a control interface, a data interface with bulk endpoints or a MAC string.
`ecm dhcp no offer` means nothing answered within 2 s (it tries three times).

Limits: the read runs inside the key's callback and blocks the controller path until it finishes
(about a second, up to tens of seconds when a step fails). IPv4 only, one connection at a time,
at most 256 ids kept. It can share a build with `usb_probe` (the SDK takes two drivers) but not
with another extension that claims Record.

```sh
python ../open-xy/sdk.py test --extension extensions/usb_ecm
```

Installing a build is a device operation: ask for it explicitly.
