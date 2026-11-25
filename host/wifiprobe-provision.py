#!/usr/bin/env python3
"""Provision / inspect a Pico 2 W running debugprobe-wifi over its USB serial port.

  wifiprobe-provision.py --ssid "MyNet" --country SK --allow 192.168.1.23/32
  wifiprobe-provision.py --status        # link state, IP, diagnostics
  wifiprobe-provision.py --log           # boot / event log
  wifiprobe-provision.py --raw-log       # no console: just open the port and print what comes
  wifiprobe-provision.py --console       # interactive

The WiFi password is asked for (not echoed) or taken from $WIFIPROBE_PSK; it is
sent once over USB and stored only in the Pico's flash. Nothing is written on
the host. Works on Linux (/dev/ttyACM*) and macOS (/dev/cu.usbmodem*).

The console is reached either by typing the magic string "+++wifiprobe" + Enter
at any baud rate (default here) or by opening the port at 9727 baud
(--method baud). Needs pyserial (e.g. `uvx --with pyserial python wifiprobe-provision.py ...`).
"""
import argparse
import getpass
import os
import re
import sys
import time

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    sys.exit("pyserial is required: pip install pyserial  (or: uvx --with pyserial python " + sys.argv[0] + " ...)")

MAGIC_BAUD = 9727
NORMAL_BAUD = 115200
MAGIC = b"+++wifiprobe\r"
VID, PID = 0x2E8A, 0x000C        # Raspberry Pi CMSIS-DAP debug probe
PROMPT = rb"(\r\n|^)\*?> $"


def find_port():
    probes = [p for p in list_ports.comports() if p.vid == VID and p.pid == PID]
    if sys.platform == "darwin":   # use the call-out device, not the dial-in /dev/tty.*
        probes = [p for p in probes if "/cu." in p.device] or probes
    wifi = [p for p in probes if "WiFi" in (p.product or "") or "WiFi" in (p.description or "")]
    if len(wifi) == 1:
        return wifi[0].device
    if len(wifi) > 1:
        sys.exit("several debugprobe-wifi probes found, pick one with --port:\n  " +
                 "\n  ".join(f"{p.device}  serial {p.serial_number}" for p in wifi))
    if probes:
        sys.exit("found a Raspberry Pi debug probe, but its product string has no 'WiFi':\n  " +
                 "\n  ".join(f"{p.device}  '{p.product or p.description}'" for p in probes) +
                 "\nit is running the stock debugprobe firmware; flash debugprobe_on_pico2_w.uf2 first"
                 " (or pass --port to force)")
    others = [p.device for p in list_ports.comports()]
    sys.exit("no debug probe (USB 2e8a:000c) found. Serial ports present: " + (", ".join(others) or "none") +
             "\nIs the Pico plugged in with a data cable? On macOS, did you allow the accessory?")


class Console:
    def __init__(self, port, method="auto", timeout=4.0):
        self.port, self.timeout = port, timeout
        self.s = None
        errors = []
        for m in (["magic", "baud"] if method == "auto" else [method]):
            try:
                self._open(m)
                self.method = m
                return
            except (TimeoutError, serial.SerialException, OSError) as e:
                errors.append(f"{m}: {e}")
                self.close()
        raise SystemExit(
            "the probe did not open its config console.\n  " + "\n  ".join(errors) +
            "\nIf --raw-log shows a log, the firmware runs but the console failed: send that log."
            "\nIf --raw-log shows nothing and the LED is dark, the firmware is not running"
            " (old build? crash?): unplug / replug and try again, then send the LED pattern.")

    def _open(self, method):
        baud = MAGIC_BAUD if method == "baud" else NORMAL_BAUD
        self.s = serial.Serial(self.port, baud, timeout=0.2)
        try:
            self.s.dtr = True            # the probe only talks while DTR is set
        except OSError:
            pass                         # not a real serial port (tests on a pty)
        time.sleep(0.3)
        self.s.reset_input_buffer()      # drop the boot log printed on open
        self.s.write(MAGIC if method == "magic" else b"\r")
        self.read_until(PROMPT)

    def read_until(self, pattern, timeout=None):
        end = time.time() + (timeout or self.timeout)
        buf = b""
        while time.time() < end:
            buf += self.s.read(512)
            if re.search(pattern, buf):
                return buf.decode(errors="replace")
        raise TimeoutError(f"no answer within {timeout or self.timeout:.0f} s (received {len(buf)} bytes: {buf[-120:]!r})")

    def cmd(self, line, timeout=None):
        self.s.write(line.encode() + b"\r")
        try:
            out = self.read_until(PROMPT, timeout)
        except TimeoutError as e:
            raise SystemExit(f"'{line.split()[0]}' got no answer: {e}\n"
                             "The probe may have crashed: unplug/replug, then run --log (the reset reason is in it).")
        if "\r\nERR" in out:
            raise SystemExit(f"{line.split()[0]}: " + out.split("ERR", 1)[1].splitlines()[0].strip())
        return out

    def body(self, line, timeout=None):
        out = self.cmd(line, timeout)
        out = out.split("\r\n", 1)[1] if "\r\n" in out else out        # drop the echo
        return out.rsplit("OK", 1)[0].rstrip()

    def set_psk(self, psk):
        self.s.write(b"psk\r")
        self.read_until(rb"password: ")
        self.s.write(psk.encode() + b"\r")
        out = self.read_until(PROMPT)
        if "ERR" in out:
            raise SystemExit("psk: " + out.split("ERR", 1)[1].splitlines()[0].strip())

    def status(self):
        return dict(re.findall(r"^(\w[\w ]*?)\s*: (.*?)\r?$", self.body("status"), re.M))

    def close(self):
        if self.s is not None:
            try:
                self.s.write(b"exit\r")      # back to the UART bridge (closing the port does it too)
                time.sleep(0.1)
            except Exception:
                pass
            self.s.close()
            self.s = None


def raw_log(port, seconds):
    s = serial.Serial(port, NORMAL_BAUD, timeout=0.2)
    try:
        s.dtr = True
    except OSError:
        pass
    end, got = time.time() + seconds, b""
    while time.time() < end:
        got += s.read(1024)
    s.close()
    text = got.decode(errors="replace")
    print(text if text else "(nothing received: firmware not running, or the target UART has sent data,"
                            " which suppresses the log; use --log)")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", help="serial device (default: auto-detect by USB id 2e8a:000c)")
    ap.add_argument("--method", choices=["auto", "magic", "baud"], default="auto",
                    help="how to open the console (default: magic string, then 9727 baud)")
    ap.add_argument("--ssid")
    ap.add_argument("--auth", choices=["wpa2", "wpa3", "open"])
    ap.add_argument("--hostname")
    ap.add_argument("--country", help="two letters, e.g. SK (default XX = worldwide)")
    ap.add_argument("--allow", help="a.b.c.d/nn or 'any': only this network may connect to the DAP port")
    ap.add_argument("--dap-port", type=int)
    ap.add_argument("--wifi", choices=["on", "off"])
    ap.add_argument("--status", action="store_true", help="show status / diagnostics")
    ap.add_argument("--log", action="store_true", help="print the probe's boot / event log")
    ap.add_argument("--raw-log", action="store_true", help="only open the port at 115200 and print what arrives")
    ap.add_argument("--erase", action="store_true", help="wipe the stored settings")
    ap.add_argument("--reboot", action="store_true", help="reboot the probe at the end")
    ap.add_argument("--console", action="store_true", help="interactive console")
    ap.add_argument("--wait", type=float, default=45, help="seconds to wait for the WiFi join")
    a = ap.parse_args()
    port = a.port or find_port()
    print(f"probe port: {port}")

    if a.raw_log:
        return raw_log(port, 3)

    if a.console:
        import serial.tools.miniterm as mt
        print("type +++wifiprobe and Enter (nothing is echoed until then), 'help' for commands; Ctrl-] quits")
        sys.argv = ["miniterm", "--eol", "CR", port, str(NORMAL_BAUD)]
        return mt.main()

    con = Console(port, a.method)
    try:
        if a.log:
            print(con.body("log"))
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
            con.cmd("save", timeout=15)
            print("saved to the probe's flash")
        if changed or a.status or not (a.log or a.erase):
            print(con.body("show"))
            end = time.time() + (a.wait if changed else 0)
            st = con.status()
            while not st.get("wifi", "").startswith(("connected", "off", "CYW43", "SAFE")) and time.time() < end:
                time.sleep(1)
                st = con.status()
            print(con.body("status"))
        if a.reboot:
            con.cmd("reboot")
    finally:
        con.close()


if __name__ == "__main__":
    sys.exit(main())
