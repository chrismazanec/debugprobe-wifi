# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The debugprobe-wifi authors
"""Compare a simulator flash dump (1 MiB flash + 4 KiB UICR) with an Intel HEX file."""
import sys
def load_hex(path):
    mem = {}
    base = 0
    for line in open(path):
        line = line.strip()
        if not line.startswith(':'):
            continue
        b = bytes.fromhex(line[1:])
        n, addr, typ = b[0], (b[1] << 8) | b[2], b[3]
        data = b[4:4 + n]
        if (sum(b) & 0xFF) != 0:
            raise SystemExit("bad checksum")
        if typ == 0:
            for i, v in enumerate(data):
                mem[base + addr + i] = v
        elif typ == 2:
            base = ((data[0] << 8) | data[1]) << 4
        elif typ == 4:
            base = ((data[0] << 8) | data[1]) << 16
        elif typ == 1:
            break
    return mem
mem = load_hex(sys.argv[1])
dump = open(sys.argv[2], 'rb').read()
FL = 1024 * 1024
bad = 0
for a, v in mem.items():
    if a < FL:
        d = dump[a]
    elif 0x10001000 <= a < 0x10002000:
        d = dump[FL + a - 0x10001000]
    else:
        print("address outside model", hex(a)); bad += 1; continue
    if d != v:
        bad += 1
        if bad < 5:
            print("mismatch at", hex(a), hex(d), hex(v))
covered = set(mem)
stale = sum(1 for a in range(0, 0x2000) if a not in covered and dump[a] != 0xFF)
print(f"hex bytes={len(mem)} mismatches={bad} pre-existing bytes left in 0..0x2000 outside the hex={stale}")
sys.exit(1 if bad or stale else 0)
