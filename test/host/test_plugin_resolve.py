# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The debugprobe-wifi authors
"""pyocd-daptcp resolves a host name once, IPv4 first (the cause of ~10 s connects to
`name.local` on macOS was repeated AF_UNSPEC lookups waiting for AAAA answers)."""
import socket, sys
import pyocd.probe.aggregator  # noqa: F401  (pyOCD loads the plugin itself; import it the same way)
import pyocd_daptcp as p
calls = []
real = socket.getaddrinfo
def fake(host, port, family=0, type=0, *a, **k):
    calls.append((host, family))
    if host == "myprobe.local":
        return real("127.0.0.1", port, socket.AF_INET, type)
    return real(host, port, family, type)
socket.getaddrinfo = fake
r1 = p._resolve("myprobe.local", 4441)
r2 = p._resolve("myprobe.local", 4441)
ok = r1 == r2 and r1[0] == socket.AF_INET and calls == [("myprobe.local", socket.AF_INET)]
print("resolve calls:", calls, "result:", r1)
iface = p.DapTcpInterface(*p._parse("myprobe.local:4441"))
ok &= (iface.host, iface.port) == ("myprobe.local", 4441) and p._parse("10.0.0.7") == ("10.0.0.7", 4441)
sys.exit(0 if ok else 1)
