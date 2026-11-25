#!/bin/bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The debugprobe-wifi authors
# Host-side tests of debugprobe-wifi: no Pico, no target needed.
#
# Required: a C compiler (gcc/clang with ASan/UBSan), make, python3.
# Optional stages (skipped with a message when the tool is missing):
#   pyserial     provisioning-script tests   (python3 -c 'import serial', or `uv`)
#   OpenOCD      built from git master with --enable-cmsis-dap-tcp; set OPENOCD=/path/to/openocd
#                or have such an `openocd` in PATH
#   pyOCD        set PYOCD="/path/to/pyocd" (with host/pyocd-daptcp installed in its environment),
#                or have `uvx` (uses pyocd==0.46.1 + ./host/pyocd-daptcp)
# Firmware build / stack / upstream-identity checks: scripts/check_firmware.sh (needs pico-sdk).
#
# Usage: test/host/run_host_tests.sh      (from anywhere)
set -u
H=$(cd "$(dirname "$0")" && pwd); ROOT=$(cd "$H/../.." && pwd); cd "$H"
OUT=$H/out; mkdir -p "$OUT"
pass=0; fail=0; skip=0
res()  { if [ "$1" = 0 ]; then pass=$((pass+1)); echo "PASS  $2"; else fail=$((fail+1)); echo "FAIL  $2"; fi; }
skipm(){ skip=$((skip+1)); echo "SKIP  $1"; }
port() { python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])'; }
SIM=$H/daptcp-sim
start_sim() { local p=$1; shift; "$SIM" -p "$p" "$@" > "$OUT/sim_$p.log" 2>&1 & echo $!; sleep 0.3; }
stop_sim()  { kill -INT "$1" 2>/dev/null; wait "$1" 2>/dev/null; }

echo "== required"
make -s -C "$H" > "$OUT/make.log" 2>&1; res $? "build host harness (see out/make.log)"
"$H/unit" > "$OUT/unit.log"; rc=$?; tail -1 "$OUT/unit.log"; res $rc "unit tests: framing, command filter, settings, console, magic string, log, crash records"
P=$(port); PID=$(start_sim "$P")
python3 -I "$H/test_server.py" "$P" > "$OUT/server.log"; rc=$?; tail -1 "$OUT/server.log"; res $rc "TCP server protocol / robustness against the simulated probe"
stop_sim "$PID"; ! grep -q "AddressSanitizer" "$OUT/sim_$P.log"; res $? "no AddressSanitizer report in the server run"
P=$(port); PID=$(start_sim "$P" --allow 10.0.0.0/8)
python3 -I - "$P" <<'PY'; res $? "peer allow list refuses 127.0.0.1 when only 10.0.0.0/8 is allowed"
import socket, struct, sys
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=3)
try:
    s.sendall(struct.pack("<IHBB", 0x00504144, 2, 1, 0) + b"\x00\xff"); d = s.recv(64)
except ConnectionResetError:
    d = b""
sys.exit(0 if d == b"" else 1)
PY
stop_sim "$PID"

echo "== provisioning script (pyserial)"
if python3 -c 'import serial' 2>/dev/null; then PYS="python3 -I"
elif command -v uv >/dev/null; then PYS="uv run -q --no-project --with pyserial python -I"
else PYS=""; fi
if [ -z "$PYS" ]; then skipm "provisioning tests: no pyserial and no uv"; else
  $PYS "$H/test_find_port.py" "$ROOT/host/wifiprobe-provision.py" > "$OUT/findport.log" 2>&1
  [ "$(grep -c usbmodem1101 "$OUT/findport.log")" = 2 ] && grep -q "no 'WiFi'" "$OUT/findport.log" && grep -q "/dev/ttyACM0" "$OUT/findport.log"
  res $? "port auto-detection (macOS cu./tty. pair, stock firmware, no probe, Linux)"
  rm -f "$OUT/saved_cfg.bin"
  "$H/console_pty" "$OUT/saved_cfg.bin" > "$OUT/pty.txt" & CPID=$!; sleep 0.3
  WIFIPROBE_PSK='test password 123' timeout 60 $PYS "$ROOT/host/wifiprobe-provision.py" --port "$(head -1 "$OUT/pty.txt")" \
     --method magic --ssid "Example Net" --country DE --hostname myprobe --allow 192.168.1.0/24 > "$OUT/prov.log" 2>&1
  python3 -I - "$OUT/saved_cfg.bin" <<'PY'; res $? "provisioning via magic string on a pty: stored record correct, password not printed"
import struct, sys
d = open(sys.argv[1], "rb").read()
ok = (len(d) == 160 and d[8:41].split(b"\0")[0] == b"Example Net" and d[41:106].split(b"\0")[0] == b"test password 123"
      and d[106:139].split(b"\0")[0] == b"myprobe" and d[139:141] == b"DE"
      and struct.unpack_from("<BBHHII", d, 142)[4:] == (0xC0A80100, 0xFFFFFF00))
log = open(sys.argv[1].replace("saved_cfg.bin", "prov.log")).read()
sys.exit(0 if ok and "test password" not in log and "connected to Example Net" in log else 1)
PY
  kill "$CPID" 2>/dev/null
fi

echo "== OpenOCD (cmsis-dap tcp backend)"
OO=${OPENOCD:-$(command -v openocd || true)}
if [ -z "$OO" ] || ! "$OO" -c "adapter driver cmsis-dap; cmsis-dap backend tcp; shutdown" > /dev/null 2>&1; then
  skipm "OpenOCD stages: no OpenOCD with the cmsis-dap tcp backend (set OPENOCD=..., needs git master)"
else
  python3 -I "$H/gen_test_hex.py" "$OUT/test.hex"
  OOC="adapter driver cmsis-dap; cmsis-dap backend tcp; cmsis-dap tcp host 127.0.0.1"
  P=$(port); "$SIM" -p "$P" --once --dump "$OUT/flash_oocd.bin" > "$OUT/sim_oocd.log" 2>&1 & PID=$!; sleep 0.3
  timeout 300 "$OO" -c "$OOC; cmsis-dap tcp port $P; set WORKAREASIZE 0" -f target/nordic/nrf52.cfg \
    -c "init; echo \"VARIANT [mdw 0x10000104]\"; nrf52_recover; program $OUT/test.hex verify reset exit" > "$OUT/oocd.log" 2>&1
  grep -q "VARIANT 0x10000104: 41414430" "$OUT/oocd.log" && grep -q "successfully erased" "$OUT/oocd.log" && grep -q "Verified OK" "$OUT/oocd.log"
  res $? "OpenOCD: read FICR, nrf52_recover (CTRL-AP ERASEALL), program + verify (slow NVMC path)"
  wait "$PID" 2>/dev/null
  python3 -I "$H/cmp_hex.py" "$OUT/test.hex" "$OUT/flash_oocd.bin" > "$OUT/cmp.log"; rc=$?; cat "$OUT/cmp.log"; res $rc "simulated flash + UICR equal the hex, old content erased"
  P=$(port); PID=$(start_sim "$P" --protected --once)
  timeout 60 "$OO" -c "$OOC; cmsis-dap tcp port $P" -f target/nordic/nrf52.cfg \
    -c "init; nrf52_recover; echo \"VARIANT [mdw 0x10000104]\"; shutdown" > "$OUT/oocd_prot.log" 2>&1
  grep -q "VARIANT 0x10000104: 41414430" "$OUT/oocd_prot.log"; res $? "OpenOCD: recover an APPROTECT-locked device"
  stop_sim "$PID"
fi

echo "== pyOCD + host/pyocd-daptcp"
if [ -n "${PYOCD:-}" ]; then PO="$PYOCD"
elif command -v uvx >/dev/null; then PO="uvx -q --from pyocd==0.46.1 --with $ROOT/host/pyocd-daptcp pyocd"
else PO=""; fi
if [ -z "$PO" ]; then skipm "pyOCD stages: set PYOCD=... or install uv"; else
  P=$(port); PID=$(start_sim "$P" --protected)
  timeout 180 $PO commander -O auto_unlock=true -u "daptcp:127.0.0.1:$P" -t nrf52840 -c "read32 0x10000104" > "$OUT/py1.log" 2>&1
  grep -q "41414430" "$OUT/py1.log"; res $? "pyOCD: unlock a protected device (CTRL-AP), read FICR"
  timeout 120 $PO erase --mass -u "daptcp:127.0.0.1:$P" -t nrf52840 > "$OUT/py2.log" 2>&1
  grep -q "Mass erase complete" "$OUT/py2.log"; res $? "pyOCD: mass erase"
  timeout 120 $PO commander -u "daptcp:localhost:$P" -t nrf52840 -c "read32 0x10000100" > "$OUT/py3.log" 2>&1
  grep -q "00052840" "$OUT/py3.log"; res $? "pyOCD: probe addressed by host name"
  stop_sim "$PID"
  if command -v uv >/dev/null; then
    uv run -q --no-project --with pyocd==0.46.1 --with "$ROOT/host/pyocd-daptcp" python -I "$H/test_plugin_resolve.py" > "$OUT/resolve.log" 2>&1
    res $? "pyocd-daptcp resolves a name once, IPv4 only first (no repeated .local lookups)"
  else
    skipm "plugin resolver test needs uv"
  fi
fi

echo "== $pass passed, $fail failed, $skip skipped (logs in test/host/out/)"
[ "$fail" = 0 ]
