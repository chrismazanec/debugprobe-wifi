#!/bin/bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The debugprobe-wifi authors
#
# Firmware checks (need PICO_SDK_PATH -> pico-sdk >= 2.3.0, arm-none-eabi-gcc,
# cmake, ninja, and the freertos submodule):
#   1. build debugprobe_on_pico2_w.uf2 (build-pico2w/)
#   2. stack usage: no function in src/ uses more than STACK_LIMIT bytes of
#      frame, and the console / USB-callback paths are listed for review
#   3. the non-W build (pico2) is byte-identical to the upstream base commit
#      built the same way (same version string forced)
# Usage: scripts/check_firmware.sh [--no-upstream]
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd); cd "$ROOT"
: "${PICO_SDK_PATH:?set PICO_SDK_PATH to a pico-sdk >= 2.3.0 checkout (with submodules tinyusb, cyw43-driver, lwip)}"
STACK_LIMIT=${STACK_LIMIT:-600}
fail=0
ok()  { echo "PASS  $1"; }
bad() { echo "FAIL  $1"; fail=1; }
command -v arm-none-eabi-gcc >/dev/null || { echo "arm-none-eabi-gcc not found"; exit 2; }
[ -e freertos/portable/ThirdParty/Community-Supported-Ports/GCC/RP2350_ARM_NTZ ] || git submodule update -q --init --recursive

build() {   # dir board [cmake args...]
    local d=$1 b=$2; shift 2
    mkdir -p "$d" && (cd "$d" && cmake -G Ninja -DPICO_BOARD="$b" "$@" .. > cmake.log 2>&1 && ninja > ninja.log 2>&1)
}

echo "== 1. pico2_w build"
if build build-pico2w pico2_w; then
    ok "build-pico2w/debugprobe_on_pico2_w.uf2 $(arm-none-eabi-size build-pico2w/debugprobe_on_pico2_w.elf | awk 'NR==2{print "text "$1" bss "$3}') version $(grep -o '"[^"]*"' build-pico2w/generated/probe/version.h)"
    grep -E "warning" build-pico2w/ninja.log | grep -E "/src/net/|board_pico2_w|lwipopts" && bad "warnings in fork files" || ok "no compiler warnings in fork files"
else
    bad "pico2_w build (see build-pico2w/ninja.log)"
fi

echo "== 2. stack usage (frames in src/, limit $STACK_LIMIT bytes)"
if build build-su pico2_w -DPROBE_STACK_USAGE=ON; then
    find build-su/CMakeFiles/debugprobe.dir/src -name '*.su' -size +0 -exec cat {} + |
        awk -F'\t' '{n=split($1,a,":"); m=split(a[1],b,"/"); print $2"\t"b[m]":"a[n]}' | sort -n > build-su/frames.txt
    tail -8 build-su/frames.txt | sed 's/^/      /'
    big=$(awk -F'\t' -v L="$STACK_LIMIT" '$1 > L' build-su/frames.txt)
    [ -z "$big" ] && ok "largest frame $(tail -1 build-su/frames.txt | cut -f1) bytes" || bad "frames above $STACK_LIMIT bytes: $big"
    echo "      task stacks (words): TUD/DAP/ABR 256 (stock), UART 1024, CFG/NET/DTCP 2048, LED/WDT 1024, CYW43 2048"
    echo "      a call chain must fit its task: TinyUSB callbacks run in the 1 KiB TUD task (flags only there)"
else
    bad "stack-usage build"
fi

if [ "${1:-}" != "--no-upstream" ]; then
    echo "== 3. non-W build identical to upstream"
    first=$(git log --reverse --format=%H -- src/net/dap_tcp_proto.c | head -1)
    base=$(git rev-parse "$first^")
    # Build both commits at the same path: build paths end up in the image.
    wt=$(mktemp -d); export PROBE_VERSION_OVERRIDE=identity-check
    # HEAD = committed state; uncommitted edits are not part of this check
    for c in "$base" HEAD; do
        git worktree add -q --detach "$wt/src" "$c"
        # same version stamp in both: use this checkout's get-version.sh (build helper only)
        cp "$ROOT/get-version.sh" "$wt/src/get-version.sh"
        (cd "$wt/src" && git submodule update -q --init --recursive && build build-id pico2) || bad "pico2 build of $c"
        cp "$wt/src/build-id/debugprobe_on_pico2.uf2" "$wt/$( [ "$c" = HEAD ] && echo fork || echo upstream ).uf2" 2>/dev/null
        git worktree remove --force "$wt/src"
    done
    unset PROBE_VERSION_OVERRIDE
    if cmp -s "$wt/upstream.uf2" "$wt/fork.uf2"; then
        ok "pico2 UF2 of HEAD byte-identical to upstream $(git rev-parse --short "$base") (committed state)"
    else
        bad "pico2 UF2 of HEAD differs from upstream $(git rev-parse --short "$base")"
    fi
    rm -rf "$wt"
fi
exit $fail
