#!/usr/bin/env python3
"""Provision WiFi on a Pico 2 W running debugprobe-wifi, over its USB serial port.

The password is asked for interactively (not echoed) or read from the
environment variable WIFIPROBE_PSK; it is sent once over USB and stored only
in the Pico's own flash. Nothing is written to disk on the host.

  wifiprobe-provision.py --ssid "MyNet" --country SK --allow 192.168.1.0/24
  wifiprobe-provision.py --status          # show link state and IP
  wifiprobe-provision.py --console         # interactive console (raw)

Needs pyserial. The console is reached by opening the probe's CDC port at the
magic baud rate 9727; any other baud rate is the normal target UART bridge.
"""
import argparse
import getpass
import glob
import os
import re
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("pyserial is required: pip install pyserial")

MAGIC_BAUD = 9727


def find_port():
    cands = sorted(glob.glob("/dev/serial/by-id/*Debugprobe_WiFi*"))
    if len(cands) == 1:
        return cands[0]
    if not cands:
        sys.exit("no debugprobe-wifi serial port found under /dev/serial/by-id; use --port")
    sys.exit("several probes found, pick one with --port:\n  " + "\n  ".join(cands))


class Console:
    def __init__(self, port, baud=MAGIC_BAUD, timeout=5.0):
        self.s = serial.Serial(port, baud, timeout=0.2)
        try:
            self.s.dtr = True            # the probe only talks while DTR is set
        except OSError:
            pass                         # not a real serial port (tests on a pty)
        self.timeout = timeout
        self.s.write(b"\r")              # get a fresh prompt (banner may be lost)
        self.read_until_prompt()

    def read_until(self, pattern, timeout=None):
        end = time.time() + (timeout or self.timeout)
        buf = b""
        while time.time() < end:
            buf += self.s.read(256)
            if re.search(pattern, buf):
                return buf.decode(errors="replace")
        raise TimeoutError(f"probe did not answer (got {buf[-200:]!r})")

    def read_until_prompt(self, timeout=None):
        return self.read_until(rb"\*?> $", timeout)

    def cmd(self, line, timeout=None):
        self.s.write(line.encode() + b"\r")
        out = self.read_until_prompt(timeout)
        if "\r\nERR" in out or out.startswith("ERR"):
            raise RuntimeError(f"{line.split()[0]}: " + out.split("ERR", 1)[1].splitlines()[0].strip())
        return out

    def set_psk(self, psk):
        self.s.write(b"psk\r")
        self.read_until(rb"password: ")
        self.s.write(psk.encode() + b"\r")
        out = self.read_until_prompt()
        if "ERR" in out:
            raise RuntimeError("psk: " + out.split("ERR", 1)[1].splitlines()[0].strip())

    def status(self):
        out = self.cmd("status")
        return dict(re.findall(r"^(\w[\w ]*?)\s*: (.*?)\r?$", out, re.M))

    def close(self):
        self.s.close()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", help="serial device (default: auto-detect)")
    ap.add_argument("--ssid")
    ap.add_argument("--auth", choices=["wpa2", "wpa3", "open"])
    ap.add_argument("--hostname")
    ap.add_argument("--country", help="two letters, e.g. SK (default XX = worldwide)")
    ap.add_argument("--allow", help="a.b.c.d/nn or 'any': only this network may connect to the DAP port")
    ap.add_argument("--dap-port", type=int)
    ap.add_argument("--wifi", choices=["on", "off"])
    ap.add_argument("--status", action="store_true", help="only show status")
    ap.add_argument("--erase", action="store_true", help="wipe the stored settings")
    ap.add_argument("--console", action="store_true", help="interactive console")
    ap.add_argument("--baud", type=int, default=MAGIC_BAUD, help=argparse.SUPPRESS)  # tests on a pty
    ap.add_argument("--wait", type=float, default=45, help="seconds to wait for the WiFi join")
    a = ap.parse_args()
    port = a.port or find_port()

    if a.console:
        try:
            import serial.tools.miniterm as mt
        except ImportError:
            sys.exit("miniterm not available")
        sys.argv = ["miniterm", "--eol", "CR", port, str(MAGIC_BAUD)]
        return mt.main()

    con = Console(port, a.baud)
    try:
        if a.erase:
            con.cmd("erase")
            print("settings erased")
        if a.ssid is not None:
            con.cmd("ssid " + a.ssid)
        if a.auth:
            con.cmd("auth " + a.auth)
        if a.ssid is not None and a.auth != "open":
            psk = os.environ.get("WIFIPROBE_PSK") or getpass.getpass(f"WiFi password for {a.ssid!r}: ")
            con.set_psk(psk)
            psk = None
        for opt, cmd in ((a.hostname, "hostname"), (a.country, "country"), (a.allow, "allow"),
                         (a.dap_port, "port"), (a.wifi, "wifi")):
            if opt is not None:
                con.cmd(f"{cmd} {opt}")
        changed = any(v is not None for v in (a.ssid, a.auth, a.hostname, a.country, a.allow, a.dap_port, a.wifi))
        if changed:
            con.cmd("save", timeout=10)
            print("saved to the probe's flash")
            if a.country:
                print("country code changes take effect after a reboot: use --console and type 'reboot'")
        print(con.cmd("show").split("\r\n", 1)[1].rsplit("OK", 1)[0].rstrip())
        end = time.time() + (a.wait if changed else 0)
        st = con.status()
        while not st.get("wifi", "").startswith(("connected", "off")) and time.time() < end:
            time.sleep(1)
            st = con.status()
        for k in ("wifi", "ip", "dap", "mdns", "client", "last err"):
            if k in st:
                print(f"{k:9}: {st[k]}")
    finally:
        con.close()


if __name__ == "__main__":
    sys.exit(main())
