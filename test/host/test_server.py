# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The debugprobe-wifi authors
"""Protocol / robustness tests against daptcp-sim (the firmware's server code on the host)."""
import os, random, socket, struct, sys, time
HOST, PORT = "127.0.0.1", int(sys.argv[1])
H = struct.Struct("<IHBB")
SIG = 0x00504144
ok = fail = 0

def check(cond, what):
    global ok, fail
    if cond:
        ok += 1
    else:
        fail += 1
        print("FAIL:", what)

def conn():
    s = socket.create_connection((HOST, PORT), timeout=3)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    return s

def req(payload, typ=1, sig=SIG):
    return H.pack(sig, len(payload), typ, 0) + bytes(payload)

def recv_exact(s, n):
    b = b""
    while len(b) < n:
        c = s.recv(n - len(b))
        if not c:
            raise EOFError
        b += c
    return b

def resp(s):
    sig, n, typ, r = H.unpack(recv_exact(s, 8))
    assert sig == SIG and typ == 2 and r == 0, (hex(sig), typ, r)
    return recv_exact(s, n)

def closed(s):
    try:
        s.settimeout(2)
        return s.recv(1) == b""
    except ConnectionResetError:
        return True
    except socket.timeout:
        return False

# 1. DAP_Info packet size / count are the TCP values, FW version from DAP core
s = conn()
s.sendall(req([0x00, 0xFF])); r = resp(s); check(r == bytes([0, 2, 0x00, 0x04]), f"pkt size {r.hex()}")
s.sendall(req([0x00, 0xFE])); r = resp(s); check(r == bytes([0, 1, 4]), f"pkt count {r.hex()}")
s.sendall(req([0x00, 0x04])); r = resp(s); check(r[0] == 0 and r[1] > 0 and r[2:2 + r[1] - 1] == b"2.0.0", f"fw ver {r.hex()}")

# 2. pipelining: 4 requests in one segment, 4 responses in order
s.sendall(req([0x02, 0x01]) + req([0x11, 0x40, 0x42, 0x0F, 0x00]) + req([0x00, 0xFF]) + req([0x00, 0xFE]))
rs = [resp(s) for _ in range(4)]
check(rs[0] == bytes([0x02, 0x01]), f"connect swd {rs[0].hex()}")
check(rs[1] == bytes([0x11, 0x00]), f"swj clock {rs[1].hex()}")
check(rs[2][0] == 0 and rs[3][0] == 0, "info order")

# 3. a real SWD sequence: line reset + JTAG-to-SWD + line reset + idle, read DPIDR
seq = [0x12, 136] + [0xFF] * 7 + [0x9E, 0xE7] + [0xFF] * 7 + [0x00]
s.sendall(req(seq)); check(resp(s) == bytes([0x12, 0x00]), "swj sequence")
s.sendall(req([0x05, 0x00, 0x01, 0x02])); r = resp(s)
check(len(r) == 7 and r[1] == 1 and r[2] == 1 and struct.unpack("<I", r[3:7])[0] == 0x2BA01477, f"DPIDR {r.hex()}")

# 4. refused commands answer 0xFF and do not disturb the session
for bad in ([0x7F, 1, 0x00, 0xFF], [0x7E, 1, 0x00, 0xFF], [0x80], [0x14, 1, 0], [0x06, 0, 0xFF, 0xFF, 0x02], [0x06, 0, 0xFF, 0x03, 0x02], [0x99]):
    s.sendall(req(bad)); r = resp(s); check(r == b"\xff", f"refuse {bytes(bad).hex()} -> {r.hex()}")
s.sendall(req([0x05, 0x00, 0x01, 0x02])); r = resp(s); check(r[1] == 1 and r[2] == 1, "still alive after refusals")

# 5. maximal TransferBlock read (255 words of DPIDR: OpenOCD's maximum with 1024-byte packets)
s.sendall(req([0x06, 0x00, 0xFF, 0x00, 0x02])); r = resp(s)
check(len(r) == 4 + 4 * 255 and r[1] == 0xFF and r[3] == 1, f"block read len {len(r)} {r[:4].hex()}")

# 6. split header / payload across segments
p = req([0x00, 0xFF])
for b in p:
    s.sendall(bytes([b])); time.sleep(0.002)
check(resp(s)[0] == 0, "byte-wise delivery")
s.sendall(req([0x03])); check(resp(s) == bytes([0x03, 0x00]), "disconnect")
s.close()

# 7. framing errors drop only that client
for raw in (req([0x00, 0xFF], sig=0x11223344), req([0x00, 0xFF], typ=2), H.pack(SIG, 0, 1, 0), H.pack(SIG, 1025, 1, 0)):
    s = conn(); s.sendall(raw); check(closed(s), f"closed after bad frame {raw[:8].hex()}"); s.close()

# 8. random garbage, then a good client still works
rnd = random.Random(1)
for i in range(30):
    s = conn()
    blob = bytes(rnd.randrange(256) for _ in range(rnd.randrange(1, 3000)))
    if i % 3 == 0:  # valid header, random payload of random length: exercises the DAP core
        n = rnd.randrange(1, 1025)
        blob = req(bytes(rnd.randrange(256) for _ in range(n)))
    try:
        s.sendall(blob)
        s.settimeout(1)
        while s.recv(4096):
            pass
    except (socket.timeout, ConnectionResetError, BrokenPipeError):
        pass
    s.close()
s = conn(); s.sendall(req([0x00, 0xFF])); check(resp(s)[0] == 0, "good client after garbage"); s.close()

# 9. second client waits while the first is connected, is served after it leaves
a = conn(); a.sendall(req([0x00, 0xFF])); resp(a)
b = conn(); b.sendall(req([0x00, 0xFF])); b.settimeout(1.0)
try:
    b.recv(1); check(False, "second client served concurrently")
except socket.timeout:
    check(True, "")
a.close()
b.settimeout(3); check(resp(b)[0] == 0, "second client served after first left"); b.close()

print(f"{ok} passed, {fail} failed")
sys.exit(1 if fail else 0)
