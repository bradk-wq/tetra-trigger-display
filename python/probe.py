#!/usr/bin/env python3
"""Read-only probe of an MCT Trigger 6 USB display adapter.

Every request here is device-to-host. Nothing is written, no mode is set, and
no session is opened, so this cannot disturb a working display.

    uv run --with pyusb --with libusb1 probe.py [--watch SECONDS]
"""

import struct
import sys

import usb.core
import usb.util

VID, PID = 0x0711, 0x560B
IN = 0xC0  # vendor request, device-to-host

REQUESTS = (
    (0x88, 4, "VRAM size (MB)"),
    (0x87, 4, "connector status"),
    (0xB0, 64, "adapter info"),
    (0xB1, 64, "session info"),
)


def edid_name(edid):
    """Return the EDID's monitor name, from its 0xfc descriptor block."""
    for off in (0x36, 0x48, 0x5A, 0x6C):
        block = edid[off:off + 18]
        if block[0:3] == b"\x00\x00\x00" and block[3] == 0xFC:
            return block[5:18].decode("ascii", "replace").strip()
    return None


def edid_preferred(edid):
    """Return (width, height, kHz) from the EDID's first detailed timing."""
    d = edid[0x36:0x36 + 18]
    clock = struct.unpack_from("<H", d, 0)[0] * 10
    width = d[2] | ((d[4] & 0xF0) << 4)
    height = d[5] | ((d[7] & 0xF0) << 4)
    return width, height, clock


def modes(raw):
    """Yield the adapter's mode records until they stop looking like timings."""
    for i in range(0, len(raw), 32):
        record = raw[i:i + 32]
        if not any(record):
            continue
        clock = struct.unpack_from("<I", record, 0)[0]
        hz = struct.unpack_from("<H", record, 4)[0]
        htotal = struct.unpack_from("<H", record, 6)[0]
        hactive = struct.unpack_from("<H", record, 8)[0]
        vtotal = struct.unpack_from("<H", record, 14)[0]
        vactive = struct.unpack_from("<H", record, 16)[0]
        # Past the real entries the array is uninitialised, not terminated.
        if not (1 <= hz <= 240 and 0 < hactive <= 4096 and 0 < vactive <= 4096):
            return
        yield clock, hz, hactive, vactive, htotal, vtotal


def main(argv):
    device = usb.core.find(idVendor=VID, idProduct=PID)
    if device is None:
        print("no %04x:%04x on the bus" % (VID, PID))
        return 1

    print("%04x:%04x  %s  %s" % (
        VID, PID,
        usb.util.get_string(device, device.iManufacturer),
        usb.util.get_string(device, device.iProduct)))

    for cfg in device:
        for intf in cfg:
            print("  interface %d  class 0x%02x" % (
                intf.bInterfaceNumber, intf.bInterfaceClass))
            for ep in intf:
                kind = ("control", "iso", "bulk", "interrupt")[ep.bmAttributes & 3]
                way = "IN " if ep.bEndpointAddress & 0x80 else "OUT"
                print("    ep 0x%02x %s %-9s %d bytes" % (
                    ep.bEndpointAddress, way, kind, ep.wMaxPacketSize))

    print("\nread-only requests:")
    for request, length, name in REQUESTS:
        try:
            data = bytes(device.ctrl_transfer(IN, request, 0, 0, length, timeout=1000))
            print("  0x%02x %-18s %s" % (request, name, data[:16].hex()))
        except Exception as error:
            print("  0x%02x %-18s %s" % (request, name, error))

    edid = bytes(device.ctrl_transfer(IN, 0x80, 0, 0, 128, timeout=1000))
    if edid[:8] == b"\x00\xff\xff\xff\xff\xff\xff\x00":
        width, height, clock = edid_preferred(edid)
        print("\nEDID: %s, preferred %dx%d at %.2f MHz"
              % (edid_name(edid), width, height, clock / 1000.0))
    else:
        print("\nEDID: no valid header")

    raw = bytes(device.ctrl_transfer(IN, 0x89, 0, 0, 512, timeout=1000))
    print("\nadapter modes:")
    print("    clock    Hz   active      total")
    for clock, hz, hactive, vactive, htotal, vtotal in modes(raw):
        print("  %7d  %4d  %4dx%-4d  %4dx%-4d" % (clock, hz, hactive, vactive, htotal, vtotal))

    if "--watch" in argv:
        seconds = float(argv[argv.index("--watch") + 1])
        usb.util.claim_interface(device, 0)
        print("\nwatching the interrupt endpoint for %gs:" % seconds)
        import time
        start = time.time()
        while time.time() - start < seconds:
            try:
                data = bytes(device.read(0x83, 64, timeout=200))
            except usb.core.USBTimeoutError:
                continue
            print("  %6.2fs %s" % (time.time() - start, data[:32].hex()))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
