#!/usr/bin/env bash
# End-to-end check of the network path with no hardware: sender -> UDP on
# loopback -> receiver with the null sink. Runs a clean pass, then a lossy and
# reordered pass, then a slow-panel pass that must show superseded frames
# rather than a growing delay.
set -euo pipefail

receiver=${1:?receiver binary}
sender=${2:?sender binary}
work=$(mktemp -d)
trap 'rm -rf "$work"; kill $(jobs -p) 2>/dev/null || true' EXIT

port=$((20000 + RANDOM % 20000))

run() {  # name, sender args..., with RX_ARGS for the receiver
    local name=$1; shift
    "$receiver" --sink null --port "$port" --save-last "$work/last.jpg" \
        --exit-after 4 $RX_ARGS >"$work/rx.$name" 2>"$work/rx.$name.err" &
    local rx=$!
    sleep 0.5
    "$sender" --to "127.0.0.1:$port" --seconds 3 "$@" >"$work/tx.$name" 2>&1
    wait "$rx"
    echo "--- $name"; tail -n 1 "$work/rx.$name"; tail -n 2 "$work/tx.$name" | head -n 1
}

field() { sed -n "s/.*FINAL.* $1=\([0-9]*\).*/\1/p" "$work/rx.$2"; }
fail() { echo "FAIL: $*" >&2; exit 1; }

RX_ARGS="" run clean --fps 60
[ "$(field presented clean)" -ge 100 ] || fail "clean: too few frames shown"
[ "$(field abandoned clean)" -eq 0 ] || fail "clean: frames abandoned"
[ "$(field rejected clean)" -eq 0 ] || fail "clean: frames rejected"
[ "$(head -c 2 "$work/last.jpg" | od -An -tx1 | tr -d ' \n')" = ffd8 ] || fail "saved frame is not a JPEG"

RX_ARGS="" run lossy --fps 60 --loss 0.03 --shuffle
[ "$(field presented lossy)" -ge 30 ] || fail "lossy: too few frames shown"
[ "$(field rejected lossy)" -eq 0 ] || fail "lossy: corrupt frame reached the sink"

RX_ARGS="--sink-delay-ms 50" run slow --fps 60
slowShown=$(field presented slow)
slowSuper=$(field superseded slow)
[ "$slowShown" -le 70 ] || fail "slow: shown $slowShown frames in 3 s at 50 ms each"
[ "$slowSuper" -ge 50 ] || fail "slow: expected frames to be superseded, got $slowSuper"

echo "loopback: all passes ok"
