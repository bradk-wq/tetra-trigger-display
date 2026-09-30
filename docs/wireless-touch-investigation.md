# Wireless Tetra with touch: feasibility investigation

Research only; nothing here was run against hardware. Every claim is tagged
**[V]** verified in code, **[P]** publicly documented, **[I]** inferred, or
**[U]** unknown, needs hardware. Source snapshot: this repo at `d69e4fa`;
cyrozap/mct-usb-display-adapter-re and rfxDarth/mobilepixels-linux-driver
shallow clones read 2026-09-30 (rhgndf's fork was not read separately; it is a fork of cyrozap's). Vendor pages were not reachable from the
research environment, so vendor specs below come from search snippets and are
**[P]** at best.

## Recommendation

Do the video and touch problems separately, because they are coupled only by
the cable. **Video over the network is easy and low-risk**: a small Linux
receiver on the desk runs this library's `Display` and accepts whole JPEG
frames from the Mac (about 20 KB each, about 10 Mbit/s at 60 fps). **Touch is
the blocker**: nothing in the evidence shows the digitizer is reachable from
the USB-A video path, and the only public statement that it appears over DP alt
mode as `2575:fe13` comes from this repo's own README. The recommended
production design is therefore **option (a)+(d)**: a receiver with a real USB-C
DP-alt-mode port that scans video out over DP (hardware H.264/HEVC decode, so
this library is not used) and reads the digitizer locally as evdev, forwarding
compact touch events to a Mac app. Build the USB-A video receiver first as the
proof of concept, because it is cheap and de-risks the Mac sender, pacing and
latency work, and run the hardware experiments below before buying a DP-alt
receiver.

## Findings by question

**1. Video path.** [V] `Display` bring-up is: open 0711:560b, set config 1,
claim interface 0; control-IN `0xB0` (platform version, project code), `0x88`
(VRAM MB, must be >= 16), `0x80` (128-byte EDID); `0x31` software-ready; read
mode table (`0x84`/`0x89`) and synthesise the native 1280x800 mode from EDID
(`readModes`, `edidTiming`); `setMode` sends `0x12` with PLL terms recomputed
from the pixel clock (40 MHz base, 48 when platform version is 0), then `0x31`;
`setPowered` sends `0x03`. A frame is two bulk writes to EP `0x02`: a 32-byte
`BulkCommand`, then a 48-byte header plus JPEG padded by 1024 bytes
(`beginSubmission`, `presentJpeg`). State that matters: a command ring that
advances 1 MB per submission and wraps below `frameAddress_[0]` (VRAM-8 MB),
two framebuffers at VRAM-8 MB and VRAM-4 MB, a reset-JPEG flag forced on the
first 10 submissions and on each wrap, and (partial mode only) per-buffer
pending-region lists. **All of this is device-local**: the network never needs
to carry timing, EDID or addresses, only "1280x800 baseline 4:2:0 JPEG".
`presentJpeg` does no validation of JPEG size or format [V], and decoder
errors arrive on the interrupt EP as `displayEventJpegDecoderError` [V]
(`poll`), so the receiver should watch for them and ask for a keyframe.
Bulk writes time out at 5 s and throw [V]; the receiver needs a supervisor that
re-runs bring-up on any throw or unplug, since `Display` has no reconnect path.

*Split:* sender (Mac) = render + JPEG encode (`turbojpeg` is what this repo
uses; 4:2:0 baseline) + UDP/QUIC datagrams with `{seq, jpeg}`; receiver = reassembly +
`Display::presentJpeg`. **Pacing** [V]: there is no vblank, and fences
coalesce (roughly 1 in 3 reported), so `pace()` sleeps to the mode refresh
interval from the receiver's own steady clock. Over a network use
*latest-wins*: one pending slot, never a queue; a frame that arrives after a
newer one is dropped; a late frame is sent anyway (it is a full image, so it is
never "wrong", only old). **Partial updates do not survive loss**: `pending_[]`
assumes every region was delivered to both buffers, so dropping a region packet
corrupts the back buffer [I, from `Display::present`]. v1 should use full
frames only (about 1.2 MB/s), and add sender-side dirty rects later with a
"receiver acks seq, sender falls back to a full frame on gap" rule.

**2. Touch path.** [U] HID descriptor, contact count, coordinate range and
orientation of `2575:fe13`: no public source found. The only hits are this
repo's README and the Gadgetoid upstream README ("No touch, sorry"); a web
search "confirming" `fe13` just echoed those READMEs, so count it as zero
independent sources. Neither reference reverse-engineering repo mentions
`fe13`, HID or digitizers in its notes [V]. One real lead [V]: the rfxDarth
Linux tool has a touch on/off *control request on the Trigger 6 itself*
(read: control-IN `0xC0`, request `0x8B`, wValue 1; set: control-OUT `0x40`,
request `0x16`, wValue 1, wIndex on/off). That suggests the bridge gates touch
on some products. Whether the Tetra's touch rides that path, or only the
separate USB-C hub, is [U]. [I] A panel with 10-point capacitive touch is very
likely a standard HID multitouch digitizer (Windows/Linux need no vendor
driver for that class), but absent a descriptor this is a guess. The README
claim that the macOS vendor driver contains no HID code [P, author's own
disassembly] fits this.

**3. Receiver options.**

| | Video | Touch | Receiver hardware | Est. tap-to-action / tap-to-screen | Complexity | Risks |
|---|---|---|---|---|---|---|
| (a) DP alt out + local touch (+d) | H.264/HEVC stream, HW decode, DRM/KMS scanout over DP | evdev on receiver, events to Mac | USB-C with DP alt **and** simultaneous USB data: e.g. RK3588 boards (Orange Pi 5 Plus, Radxa Orion O6 class [P]), or an x86 mini PC / NUC with DP-alt USB-C [I] | 25-50 ms / 80-130 ms [I] | High (new video stack) | Board must give DP + USB at once on one cable [U]; Type-C/DP driver maturity [U] |
| (b) USB-A video + touch elsewhere | This library, JPEG | Only if fe13 enumerates on a second, non-DP USB-C cable [U] | Any Linux SBC/mini PC with a USB-A and USB-C port (a Pi 5 works; it has **no** DP alt [P]) | 25-50 ms / 70-110 ms [I] | Low | Probably fails: README says touch is absent on USB-A [P], and it may only exist in DP mode [U] |
| (c) USB/IP or VirtualHere | n/a (DP is not USB; T6 over USB/IP is possible but 1 MB/submission bulk patterns are untested [U]) | fe13 forwarded to Mac | Linux box with the DP-alt-mode host, anyway | 40 ms typical, spikes 200-1000 ms reported on Wi-Fi [P] | Medium | macOS usbip client is experimental and does not support HID [P]; VirtualHere does support HID but has reported latency spikes [P]; adds nothing over (d) |
| (d) Local read + events | (either) | evdev -> UDP to Mac menu-bar app -> `CGEvent` | Any Linux receiver that can see fe13 | 20-40 ms median [I] | Low-medium | Synthetic multitouch is not available on macOS; expect pointer, click, drag and scroll only [I]; needs Accessibility permission |

Note that (a) and (b)/(d) both need fe13 visible to the receiver; (a)'s only
extra requirement is DP alt mode. Checked: Raspberry Pi 4/5 USB-C is
power + USB 2.0 only, no DP alt [P: Pi forums]. Not checked: any specific mini PC
model's USB-C implementation; "supports DP alt" on a spec sheet does not
guarantee DP and USB data simultaneously [I].

**4. Latency** (all [I], from typical 5 GHz figures: 2-10 ms median RTT with
tails to tens or hundreds of ms under contention). Touch -> action: USB poll
(1-8 ms) + Wi-Fi one-way (1-5 ms) + Mac inject (1-2 ms) = 10-20 ms median, under
100 ms holds except in tail events; use a UDP socket with `IPTOS_DSCP`/WMM voice
class and Wi-Fi power-save off. Touch -> screen: above, plus Mac render (up to
16 ms), encode (turbojpeg at 1280x800: a few ms), Wi-Fi, USB write (20 KB,
under 1 ms), T6 decode and panel scan (up to 16 ms, decode time [U]). That is about
70-110 ms median, so the 150 ms target is plausible and the tail is the risk.
Bulk writes and 5 s timeouts mean one stalled USB transfer blocks the
receiver; run USB in its own thread so the network thread can keep dropping.

**5. Mac side.** Two designs. (i) *Custom UI rendered to JPEGs*: no virtual
display; you own the pixels, so dirty-rect tracking is exact and latency is
lowest, but it is an app, not a desktop. (ii) *Virtual display*:
`CGVirtualDisplay` (private API) or a DriverKit virtual display plus
ScreenCaptureKit capture gives a real extended desktop; adds capture latency
(about 1 frame) and TCC permissions [I]. Start with (i) for the PoC, since
`examples/sdl_mirror.cpp` already shows the render-to-`present` loop.
Touch returns as network events (d). The HID/USB-IP route (c) is not
recommended: macOS would have to map a foreign digitizer to a specific
display, and the usbip client cannot do HID [P].

**6. Licensing.** This repo is MIT and its README lists what it read. A
clean-room design means: write `protocol`-level facts (request numbers,
struct layouts) from notes, never paste or translate code from
`rfxDarth/mobilepixels-linux-driver/evdi_t6_1` (MCT headers marked all rights
reserved, no licence) or the proprietary macOS driver. cyrozap's repo is 0BSD
code and CC BY-SA notes; facts are safe, prose copying would need attribution
and share-alike. This document used the rfxDarth tree only for the touch-
request numbers above. **Flag:** the README says `pixelClockDivisors` was
"ported from `T6CalculatePixelClock`" from that unlicensed tree; that function
is in `src/display.cpp` and is the one place where the MIT claim is weakest.
Recommend re-deriving it from cyrozap's notes or measurements before any
redistribution of a wireless build.

## Proof-of-concept plan (video first, touch probe in parallel)

1. Run the hardware experiments below (two hours, no code). Decision gate:
   if fe13 shows on a non-DP path, go (b)/(d); otherwise (a)+(d).
2. Receiver v0: Linux box, USB-A to Tetra, tiny daemon wrapping `Display`
   (`setMode(nativeMode)`, `setPowered(true)`), reading `{seq, jpeg}`
   from UDP, latest-wins, refresh-rate pacing. Test with `testcard --save`
   JPEGs replayed from a file.
3. Mac sender v0: `sdl_mirror`-style render loop, full-frame JPEG, sequence
   numbers, fragmentation to fit MTU, receiver ack of last displayed seq.
4. Measure: pacing, drops, end-to-end latency with a 240 fps phone camera.
5. Touch v0 (per gate): evdev reader on receiver -> UDP -> Mac app injecting
   `CGEvent` pointer events; coordinate map from the descriptor range to 1280x800.
6. Only then decide on dirty-rect partial updates, or move to (a).

## Hardware validation experiments

Do them in this order. All are read-only on the device.

| # | Setup and command | What each outcome means |
|---|---|---|
| E1 | Tetra on USB-A of a Mac: `system_profiler SPUSBDataType` and `ioreg -p IOUSB -l -w0 \| grep -iE '0711\|560b\|2575\|fe13'` | Only 0711:560b: README confirmed. fe13 present: option (b) is alive, go to E4. |
| E2 | Tetra on the USB-C *data* port of a Mac (DP alt capable), same commands, plus `ioreg -r -c IOHIDDevice -l \| grep -B3 -A30 -i 2575` | fe13 + Genesys hub + HID node: confirms the DP-alt behavior and that macOS sees it as HID. Note whether touches move the cursor. |
| E3 | Tetra on a USB-C port **without** DP alt (Pi 5, or a hub-less USB 3 port with a C-to-C cable): `lsusb -t`, `lsusb -d 2575:fe13 -v` | fe13 appears: touch does not need DP alt, so (b)/(d) work with a cheap receiver (video would then use the USB-A cable). fe13 absent: touch needs DP alt, go (a). |
| E4 | Dump the descriptor: `sudo usbhid-dump -d 2575:fe13` (or `lsusb -v -d 2575:fe13 \| sed -n '/HID/,$p'`), then `sudo evtest` and `libinput record` | Gives contact count, X/Y logical max, and report ID. If usage page 0x0D/Digitizer, it is standard HID multitouch and (d) is straightforward. If vendor usage page, parsing needs a packet capture. |
| E5 | Capture with `sudo modprobe usbmon; sudo tshark -i usbmon<bus> -w touch.pcap` while tapping corners and pinching | Lets you derive coordinate space and orientation, and see whether the vendor driver sends extra feature reports. |
| E6 | Read-only query of the bridge's touch flag on USB-A (control-IN request `0x8B`, wValue 1, 1 byte; `python/probe.py` is the model, add the request) | 0/1 returned: the bridge knows about touch, try-enable later under your own risk assessment. STALL/error: flag unused on this model. |
| E7 | Candidate receiver: `ls /sys/class/typec/`, `dmesg \| grep -iE 'typec\|altmode\|dp'`, `modetest -c`, with the Tetra plugged in | DP connector present plus fe13 in `lsusb` at the same time: (a) is viable on this board. DP only: no touch on this board. |
| E8 | Wi-Fi: `ping -i 0.01 -c 2000 <receiver>` and `iperf3 -u -b 20M -l 1200 -t 60 -c <receiver>` | p99 RTT over 30 ms or loss over 1%: use wired Ethernet or expect stutter. |

## Open questions

- Does fe13 enumerate without DP alt? (E2/E3; **the biggest unknown**.)
- Is the bridge touch request (`0x8B`/`0x16`) related to the Tetra digitizer at all?
- Which small boards give DP **and** USB data on one USB-C cable in practice?
- Does T6 JPEG decode plus scanout add more than one frame of latency?
- Can macOS treat a foreign digitizer as touch on a given display, or only as
  a pointer? No native touchscreen support is assumed [I].
- Re-derivation of `pixelClockDivisors` to settle the licence question.
