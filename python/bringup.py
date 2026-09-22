#!/usr/bin/env python3
"""Bring up an MCT Trigger 6 display over USB-A and push a JPEG frame.

Unlike probe.py this writes to the device: it sends the software-ready command,
sets a video mode, enables the output and flips one frame.

    uv run --with pyusb --with libusb1 bringup.py --list
    uv run --with pyusb --with libusb1 bringup.py testcard.jpg
    uv run --with pyusb --with libusb1 bringup.py --mode 1280x800 testcard.jpg
    uv run --with pyusb --with libusb1 bringup.py --off
"""

import struct
import sys
import time

import usb.core
import usb.util

VID, PID = 0x0711, 0x560B
OUT, IN = 0x40, 0xC0
EP_BULK_OUT, EP_INTERRUPT_IN = 0x02, 0x83

SET_MONITOR = 0x03
SET_TIMING = 0x12
SOFTWARE_READY = 0x31
GET_EDID = 0x80
TABLE_NUM = 0x84
CONNECTION = 0x87
VRAM_SIZE = 0x88
TIMING_TABLE = 0x89
GET_VERSION = 0xB0

SIGNATURE_DISPLAY = 0
CMD_FLIP_PRIMARY = 3
COLOUR_NV12 = 6
COLOUR_JPEG = 13
FLAG_RESET_JPEG = 0x80

TIMING = "<I2B10H6B"
BULK_COMMAND = "<6I8x"
FLIP_HEADER = "<4I2H4I11xB"

PIXEL_CLOCK, FREQUENCY = 0, 1
H_TOTAL, H_ACTIVE, H_SYNC_START, H_SYNC_WIDTH = 3, 4, 5, 6
V_TOTAL, V_ACTIVE, V_SYNC_START, V_SYNC_WIDTH = 7, 8, 9, 10
FNUM, FDEN, IDIV, OUTPUT_SELECT = 11, 12, 13, 14
H_POLARITY, V_POLARITY, REDUCED, FLAG = 15, 16, 17, 18

OUTPUT = 0


def align32(value):
    """Round up to a multiple of 32."""
    return (value + 31) // 32 * 32


def pll(pixel_clock_khz, base_mhz):
    """Return (N, P, Q, output_select) for the PLL, or None if out of range.

    Ported from T6CalculatePixelClock in the MCT-originated Linux driver.
    """
    if not 25000 <= pixel_clock_khz <= 400000:
        return None
    mhz = pixel_clock_khz / 1000.0
    multiplier = 4
    for select in (3, 1, 2, 0):
        if mhz * multiplier >= 800.0:
            ratio = (mhz * multiplier) / float(base_mhz)
            whole = int(ratio)
            return whole, int((ratio - whole) * 1000), 1000, select
        multiplier *= 2
    return None


class Trigger6:
    def __init__(self, device):
        self.device = device
        self.timings = []

    def read(self, request, value=0, index=0, length=1):
        return bytes(self.device.ctrl_transfer(IN, request, value, index, length,
                                               timeout=3000))

    def write(self, request, value=0, index=0, data=None):
        return self.device.ctrl_transfer(OUT, request, value, index, data or None,
                                         timeout=3000)

    def versions(self):
        """Return the five version fields the vendor driver reads at start."""
        fields = []
        for index, length in ((0, 8), (1, 4), (2, 4), (3, 8), (4, 4)):
            fields.append(self.read(GET_VERSION, 0, index, length))
        return fields

    def software_ready(self):
        """Send the command that takes the adapter out of its idle state."""
        self.write(SOFTWARE_READY, 0, SIGNATURE_DISPLAY)

    def modes(self):
        """Return the adapter's resolution timing table for OUTPUT."""
        count = struct.unpack("<I", self.read(TABLE_NUM, OUTPUT, 0, 4))[0]
        if not 0 < count <= 128:
            return []
        raw = self.read(TIMING_TABLE, OUTPUT, 0, count * 32)
        self.timings = [raw[i * 32:(i + 1) * 32] for i in range(count)]
        return self.timings

    def set_mode(self, timing):
        """Patch the PLL fields into a timing record and apply it."""
        fields = list(struct.unpack(TIMING, timing))
        platform = struct.unpack("<I", self.read(GET_VERSION, 0, 0, 8)[:4])[0]
        divisors = pll(fields[PIXEL_CLOCK], 48 if platform == 0 else 40)
        if divisors is None:
            raise SystemExit("pixel clock %d kHz out of PLL range"
                             % fields[PIXEL_CLOCK])
        whole, numerator, denominator, select = divisors
        fields[FNUM] = numerator
        fields[FDEN] = denominator
        fields[IDIV] = whole
        fields[OUTPUT_SELECT] = select
        self.write(SET_TIMING, OUTPUT, 0, struct.pack(TIMING, *fields))
        self.software_ready()

    def monitor(self, on):
        self.write(SET_MONITOR, OUTPUT, 1 if on else 0)

    def flip(self, jpeg, width, height, fb_address, command_address, reset=True):
        """Upload one JPEG and flip it to the primary output."""
        total = len(jpeg) + 48 + 1024
        self.device.write(EP_BULK_OUT,
                          struct.pack(BULK_COMMAND, SIGNATURE_DISPLAY, total,
                                      command_address, total, 0, 0),
                          timeout=5000)
        pitch = align32(width)
        block = pitch * align32(height) + 1024
        header = struct.pack(FLIP_HEADER, CMD_FLIP_PRIMARY, total - 48, 0,
                             COLOUR_NV12, pitch, pitch, fb_address,
                             fb_address + block, 0, COLOUR_JPEG,
                             FLAG_RESET_JPEG if reset else 0)
        payload = header + jpeg
        payload += b"\x00" * (total - len(payload))
        self.device.write(EP_BULK_OUT, payload, timeout=5000)

    def drain(self, seconds):
        """Report interrupt packets seen within the given window."""
        seen = []
        deadline = time.time() + seconds
        while time.time() < deadline:
            try:
                seen.append(bytes(self.device.read(EP_INTERRUPT_IN, 64, timeout=200)))
            except usb.core.USBError:
                continue
        return seen


def describe(timing):
    """Return a one-line summary of a 32-byte timing record."""
    f = struct.unpack(TIMING, timing)
    return ("%4dx%-4d @%3dHz  clock %7d kHz  total %4dx%-4d"
            % (f[H_ACTIVE], f[V_ACTIVE], f[FREQUENCY], f[PIXEL_CLOCK],
               f[H_TOTAL], f[V_TOTAL]))


def valid(timing):
    f = struct.unpack(TIMING, timing)
    return (0 < f[H_ACTIVE] <= 4096 and 0 < f[V_ACTIVE] <= 4096
            and 1 <= f[FREQUENCY] <= 240)


def edid_preferred(edid):
    """Return (width, height) from the EDID's first detailed timing."""
    d = edid[0x36:0x36 + 18]
    return d[2] | ((d[4] & 0xF0) << 4), d[5] | ((d[7] & 0xF0) << 4)


def edid_timing(edid):
    """Build a timing record from the EDID's first detailed timing block.

    The adapter's own table holds only standard CEA modes, so a panel whose
    native mode is absent needs a custom record flagged as a customer timing.
    """
    d = edid[0x36:0x36 + 18]
    clock = struct.unpack("<H", d[0:2])[0] * 10
    if clock == 0:
        return None
    h_active = d[2] | ((d[4] & 0xF0) << 4)
    h_blank = d[3] | ((d[4] & 0x0F) << 8)
    v_active = d[5] | ((d[7] & 0xF0) << 4)
    v_blank = d[6] | ((d[7] & 0x0F) << 8)
    h_offset = d[8] | ((d[11] & 0xC0) << 2)
    h_sync = d[9] | ((d[11] & 0x30) << 4)
    v_offset = (d[10] >> 4) | ((d[11] & 0x0C) << 2)
    v_sync = (d[10] & 0x0F) | ((d[11] & 0x03) << 4)
    h_total = h_active + h_blank
    v_total = v_active + v_blank

    fields = [0] * 19
    fields[PIXEL_CLOCK] = clock
    fields[FREQUENCY] = int(round(clock * 1000.0 / (h_total * v_total)))
    fields[H_TOTAL] = h_total
    fields[H_ACTIVE] = h_active
    fields[H_SYNC_START] = h_active + h_offset
    fields[H_SYNC_WIDTH] = h_sync
    fields[V_TOTAL] = v_total
    fields[V_ACTIVE] = v_active
    fields[V_SYNC_START] = v_active + v_offset
    fields[V_SYNC_WIDTH] = v_sync
    fields[H_POLARITY] = 1 if d[17] & 0x02 else 0
    fields[V_POLARITY] = 1 if d[17] & 0x04 else 0
    fields[FLAG] = 1
    return struct.pack(TIMING, *fields)


def main(argv):
    device = usb.core.find(idVendor=VID, idProduct=PID)
    if device is None:
        print("no %04x:%04x on the bus" % (VID, PID))
        return 1
    try:
        device.set_configuration(1)
    except usb.core.USBError:
        pass
    usb.util.claim_interface(device, 0)
    t6 = Trigger6(device)

    if "--off" in argv:
        t6.monitor(False)
        print("output disabled")
        return 0

    listing = "--list" in argv
    wanted = None
    if "--mode" in argv:
        wanted = argv[argv.index("--mode") + 1].lower()

    print("versions:")
    for name, value in zip(("hw platform", "boot code", "image code",
                            "prjcode", "vendor cmd"), t6.versions()):
        print("  %-12s %s" % (name, value.hex()))

    connected = t6.read(CONNECTION, OUTPUT, 0, 1)[0]
    vram = t6.read(VRAM_SIZE, 0, 0, 1)[0]
    edid = t6.read(GET_EDID, 0, OUTPUT, 128)
    preferred = edid_preferred(edid) if edid[:8] == b"\x00\xff\xff\xff\xff\xff\xff\x00" else None
    print("\npanel %s, VRAM %d MB, EDID preferred %s"
          % ("attached" if connected else "absent", vram,
             "%dx%d" % preferred if preferred else "unknown"))

    timings = [t for t in t6.modes() if valid(t)]
    print("\nmode table (%d entries):" % len(timings))
    for i, timing in enumerate(timings):
        print("  %2d  %s" % (i, describe(timing)))

    if listing:
        return 0

    if not timings:
        print("\nno usable modes")
        return 1

    def size(timing):
        f = struct.unpack(TIMING, timing)
        return f[H_ACTIVE], f[V_ACTIVE]

    table_only = "--table" in argv
    chosen, source = None, "table"
    if wanted:
        target = tuple(int(v) for v in wanted.split("x"))
        chosen = next((t for t in timings if size(t) == target), None)
        if chosen is None and preferred == target and not table_only:
            chosen, source = edid_timing(edid), "EDID"
        if chosen is None:
            print("\n%s is not in the mode table" % wanted)
            return 1
    elif preferred:
        chosen = next((t for t in timings if size(t) == preferred), None)
        if chosen is None and not table_only:
            chosen, source = edid_timing(edid), "EDID"
        if chosen is None:
            chosen = min(timings,
                         key=lambda t: (abs(size(t)[0] - preferred[0])
                                        + abs(size(t)[1] - preferred[1])))
    else:
        chosen = max(timings, key=lambda t: size(t)[0] * size(t)[1])

    width, height = size(chosen)
    print("\nsetting %dx%d from %s: %s" % (width, height, source, describe(chosen)))
    t6.software_ready()
    t6.set_mode(chosen)
    t6.monitor(True)

    paths = [a for a in argv if not a.startswith("--") and a != wanted]
    if not paths:
        print("output enabled, no frame to send")
        return 0

    jpeg = open(paths[0], "rb").read()
    if jpeg[:2] != b"\xff\xd8":
        print("%s is not a JPEG" % paths[0])
        return 1

    command_address = 0
    frame_address = (vram - 4) * 1024 * 1024
    print("pushing %d byte JPEG to 0x%x" % (len(jpeg), frame_address))
    t6.flip(jpeg, width, height, frame_address, command_address)

    for packet in t6.drain(1.0):
        print("  interrupt %s" % packet[:20].hex())
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
