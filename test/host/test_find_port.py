# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The debugprobe-wifi authors
import importlib.util, sys, types
spec = importlib.util.spec_from_file_location("prov", sys.argv[1]); prov = importlib.util.module_from_spec(spec); spec.loader.exec_module(prov)
P = lambda dev, prod, vid=0x2E8A, pid=0x000C: types.SimpleNamespace(device=dev, product=prod, description=prod, vid=vid, pid=pid, serial_number="E6614103E7123456")
def run(ports, platform):
    prov.list_ports.comports = lambda: ports; prov.sys.platform = platform
    try: return prov.find_port()
    except SystemExit as e: return "EXIT: " + str(e).splitlines()[0]
print(run([P("/dev/tty.usbmodem1101", "Debugprobe WiFi (CMSIS-DAP)"), P("/dev/cu.usbmodem1101", "Debugprobe WiFi (CMSIS-DAP)")], "darwin"))
print(run([P("/dev/cu.usbmodem1101", "Debugprobe WiFi on Pico 2 W (CM")], "darwin"))
print(run([P("/dev/cu.usbmodem1101", "Debugprobe on Pico (CMSIS-DAP)")], "darwin"))
print(run([P("/dev/cu.Bluetooth", None, None, None)], "darwin"))
print(run([P("/dev/ttyACM0", "Debugprobe WiFi (CMSIS-DAP)")], "linux"))
