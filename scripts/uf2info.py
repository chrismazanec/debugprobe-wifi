# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The debugprobe-wifi authors
import struct, sys, collections
for path in sys.argv[1:]:
    d = open(path, 'rb').read()
    per = collections.defaultdict(list); payload = {}
    for i in range(0, len(d), 512):
        m0, m1, fl, addr, size, bno, nb, fam = struct.unpack_from('<8I', d, i)
        assert m0 == 0x0A324655 and m1 == 0x9E5D5157
        per[hex(fam)].append((addr, size))
        if fam == 0xe48bff59:
            payload[addr] = d[i + 32:i + 32 + size]
    print(path.rsplit('/', 1)[-1])
    for fam, blks in per.items():
        lo = min(a for a, _ in blks); hi = max(a + s for a, s in blks)
        print(f"  family {fam}: {len(blks)} blocks, 0x{lo:08x}..0x{hi:08x}")
    img = b"".join(payload[a] for a in sorted(payload))
    first4k = img[:4096]
    print("  IMAGE_DEF block markers in first 4 KiB:",
          first4k.find(struct.pack('<I', 0xffffded3)) >= 0 and first4k.find(struct.pack('<I', 0xab123579)) >= 0)
