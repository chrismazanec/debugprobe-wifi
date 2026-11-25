# Changelog

## Unreleased

- pyocd-daptcp: resolve the probe's name once, IPv4 first. Connecting to an IPv4-only
  `name.local` took ~10 s because each lookup waited 5 s for an IPv6 answer and pyOCD opens the
  probe twice (seen on macOS with the probe; reproduced on Linux with nss-mdns: 10.4 s, now 0.36 s).
- Version stamp of `pico2_w` builds from `git describe` (0.2.0 reported a stale hash).
- Host test suite and firmware checks in the repo (`test/host/`, `scripts/`).
- Firmware unchanged from 0.2.0 apart from licence comments and the version stamp; no new
  hardware test.

## 0.2.0 (tag `v0.2.0`, `dist/debugprobe_on_pico2_w.uf2`)

Tested on one Pico 2 W with an nRF52840 target, WPA2, pyOCD 0.46.1 (see the README tables).

- Fix: the config console ran in the stock UART task (1 KiB stack); `status` / `show` needed about
  1.4 KiB, the stack-overflow hook stopped the firmware and USB DAP died with it. The console now
  has its own task with an 8 KiB stack; TinyUSB callbacks only set flags.
- All WiFi tasks and the CYW43 async context below the USB and UART task priorities.
- Diagnostics: event log (console `log`, printed when the serial port is opened and the target
  UART is silent), crash records across reboots (stack overflow, panic, hard fault, 8 s watchdog),
  LED boot / crash codes, safe mode after two crashes in a row.
- Console also opens with `+++wifiprobe` + Enter at any baud rate; `exit`; closing the port
  closes it.
- USB product string "Debugprobe WiFi (CMSIS-DAP)" (0.1.0's was cut at 31 characters).
- Provisioning script: macOS support (`/dev/cu.*` found by USB id), `--log`, `--raw-log`,
  `--reboot`, clearer errors.

## 0.1.0 (tag `v0.1.0`)

First build: CMSIS-DAP over TCP (OpenOCD `cmsis-dap tcp` framing, port 4441) on the Pico 2 W,
bounded network command filter, peer allow list, settings in flash via a USB serial console at
9727 baud, mDNS, pyOCD plugin, provisioning script, lwIP tick fix for the 20 kHz FreeRTOS tick.
Passed the host tests; on hardware it enumerated, but the first console command crashed it
(fixed in 0.2.0).
