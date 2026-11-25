# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The debugprobe-wifi authors
"""Write a deterministic Intel HEX test image for the nRF52840 simulator:
several flash ranges (not page aligned) and two UICR words."""
import random, sys

def records(addr, data):
    out, base = [], None
    for off in range(0, len(data), 16):
        a = addr + off
        if base != a >> 16:
            base = a >> 16
            b = bytes([2, 0, 0, 4, base >> 8, base & 255])
            out.append(b)
        chunk = data[off:off + 16]
        out.append(bytes([len(chunk), (a >> 8) & 255, a & 255, 0]) + chunk)
    return out

rnd = random.Random(52840)
segs = [
    (0x00000000, bytes(rnd.randrange(256) for _ in range(0x1200))),
    (0x00026000, bytes(rnd.randrange(256) for _ in range(0x5A3C))),
    (0x000F4000, bytes(rnd.randrange(256) for _ in range(0x2E50))),
    (0x10001014, (0x000F4000).to_bytes(4, "little")),
    (0x10001018, (0x0007E000).to_bytes(4, "little")),
]
with open(sys.argv[1], "w") as f:
    for addr, data in segs:
        for r in records(addr, data):
            f.write(":" + (r + bytes([(-sum(r)) & 255])).hex().upper() + "\n")
    f.write(":00000001FF\n")
