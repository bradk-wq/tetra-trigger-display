# Wireless Tetra: proof of concept

Sends frames to a Tetra over UDP. A sender (stand-in for the Mac app) encodes
JPEGs and streams them; a small receiver next to the panel hands each one to
`tetra::Display::presentJpeg`. Design and reasoning are in
`docs/wireless-touch-investigation.md`. The library in `src/` is used as is.

**This proves the video transport, not touch.** Whether the digitizer is
reachable at all is still the open hardware question (experiments E1 to E3 in
that document).

## Status

| Piece | State |
|---|---|
| Wire format, fragmentation, latest-wins reassembly | Tested: unit tests, ASan/UBSan |
| Receiver + sender over UDP, loss, reorder, slow panel | Tested on loopback, also under TSan and ASan/UBSan |
| `DisplaySink` (real panel bring-up, re-open after USB error) | **Compiles, not run.** No device was available. Fails cleanly with "no Trigger 6 adapter found" when none is attached |
| Evdev touch forwarder (`--touch-device`) | **Compiles, not run.** No input devices were available; written for MT and single-touch, unverified against the Tetra |
| Mac-side touch injection (`CGEvent`) | Not written. The sender only prints touch events |
| Wi-Fi latency | **Not measured.** Loopback numbers say nothing about radio behaviour |

## Build

Needs libusb, CMake 3.20+ and **libjpeg-turbo 3.0 or newer** (the library uses
the `tj3` API; Ubuntu 24.04 still ships 2.1, so build 3.x and set
`PKG_CONFIG_PATH`).

```
cmake -S poc -B build-poc
cmake --build build-poc -j
ctest --test-dir build-poc --output-on-failure
```

## Run

On the machine the Tetra is plugged into (USB-A):

```
build-poc/tetra_receiver                 # 1280x800 panel, UDP port 47800
build-poc/tetra_receiver --sink null     # no panel: validate and count only
build-poc/tetra_receiver --touch-device /dev/input/eventN   # Linux only
```

On the Mac, or anywhere:

```
build-poc/tetra_sender --to receiver.local              # animated pattern, 60 fps
build-poc/tetra_sender --to 192.168.1.50 --jpeg a.jpg   # one fixed 1280x800 JPEG
build-poc/tetra_sender --to HOST --loss 0.02 --shuffle  # impairment for testing
```

Each second the sender prints frames sent, encode time, and from the
receiver's acks the round trip, the receiver's hold time (frame complete to
`presentJpeg` returned), and a one-way network estimate `(rtt - hold) / 2`.
The pattern carries the frame number in binary across the top edge, so a
240 fps camera pointed at the panel gives true glass-to-glass latency.

## How it behaves

- **Latest wins.** One mailbox slot between the network and display threads;
  a newer frame overwrites an unshown one (`superseded`). A newer frame also
  abandons a half-assembled older one (`abandoned`); fragments of older
  frames are dropped (`stale`). Nothing queues, so delay cannot build up. On
  loopback with a 50 ms panel the round trip stayed flat near 60 ms and two
  thirds of frames were skipped.
- **Pacing** is the library's: `Pacing::refreshRate` from the receiver's own
  clock, since the adapter has no vblank.
- **Validation.** Every frame is checked (JPEG, panel size, 4:2:0, baseline)
  before reaching the adapter, because `presentJpeg` checks nothing.
- **Recovery.** Any USB error discards the `Display` and re-runs bring-up on
  the next frame, after a 1 s back-off.
- **Full frames only.** Partial updates keep per-buffer bookkeeping in the
  sender's `Display`; a lost region would corrupt the back buffer, so they are
  deferred until there is an ack/keyframe scheme.

## Known limits

- **Loss hurts a lot:** a frame needs every fragment. With 37 KB test frames
  (32 fragments) 3% packet loss lost about 60% of frames in the loopback test.
  Real frames are nearer 20 KB, but expect to want FEC, smaller fragments
  of a lower-quality stream, or a wired receiver link.
- **No authentication or encryption.** Anyone who can reach the UDP port can
  show frames, and the receiver replies (acks, touch) to whichever address
  sent the last frame. Keep it on a trusted network; do not expose it.
- One sender at a time; IPv4 only; little-endian hosts only.
