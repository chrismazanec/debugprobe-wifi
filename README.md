# debugprobe-wifi

A Raspberry Pi **Pico 2 W** as a CMSIS-DAP SWD probe that you reach over **WiFi**.

The firmware is Raspberry Pi's [debugprobe](https://github.com/raspberrypi/debugprobe)
with one addition: the Pico joins your WiFi and serves CMSIS-DAP over TCP using the
framing of OpenOCD's `cmsis-dap backend tcp` (port 4441). USB works exactly as before, so
the same Pico is still a normal USB debug probe. Power it from any USB charger, wire it to
the target, and flash / debug from your desk.

Why it exists: OpenOCD (git master since 2025) and probe-rs (master since 2026-10) can talk
CMSIS-DAP over TCP, and there is an ESP32 firmware for it
([bkuschak/cmsis_dap_tcp_esp32](https://github.com/bkuschak/cmsis_dap_tcp_esp32)), but no
Pico W / Pico 2 W firmware spoke that protocol. yapicoprobe's networking is USB-NCM only and
upstream debugprobe has no network support. pyOCD has no TCP CMSIS-DAP backend at all, so
this repo also ships a small pyOCD probe plugin (`host/pyocd-daptcp`).

## Status

Version 0.2.0 (`dist/debugprobe_on_pico2_w.uf2`). Hardware testing so far: **one** Pico 2 W,
an nRF52840 target through 1 kΩ series resistors on SWCLK / SWDIO, a WPA2 network, the host on
wired LAN, pyOCD 0.46.1 with the plugin, 2026-11-25.

| Area | Status | What was seen |
|---|---|---|
| Flash via BOOTSEL, boot, LED codes | tested | 3 quick blinks, then one flash per 2 s while unconfigured; steady on once joined |
| USB enumeration | tested | product string "Debugprobe WiFi (CMSIS-DAP)"; pyOCD lists the probe |
| Provisioning with `host/wifiprobe-provision.py` on macOS | tested | password typed at the prompt, `--reboot`, then joined |
| `--status` diagnostics | tested | crashes 0, cyw43 ready rc 0, heap 104152 B free, free stack NET 7376 / DTCP 7816 / LED 3672 / CFG 6920 B |
| mDNS name (`<hostname>.local`) | tested | resolves and answers ping |
| pyOCD over WiFi: register / memory reads | tested | same values as over USB with the stock firmware |
| pyOCD over WiFi: memory dump 512 KiB | tested | byte-identical at 1, 4 and 10 MHz, equal to the known image |
| pyOCD over WiFi: sector erase + program + verify | tested | 55 sectors, 225280 B, compare OK, target runs after reset |
| OpenOCD (`cmsis-dap backend tcp`) against the real probe | **untested** | works against the host simulator only (see Tests) |
| probe-rs (`--probe tcp://cmsis-dap@host`) | **untested** | |
| Mass erase / CTRL-AP recover over WiFi | **untested** on hardware | passes against the simulator (OpenOCD and pyOCD) |
| IP allow list | **untested** on hardware | passes against the simulator |
| WPA3, open networks | **untested** | only WPA2 so far |
| Plain Pico W (RP2040) | **not supported** | the build only enables `pico2_w`, see "Other boards" |
| Targets other than nRF52840 | **untested** | |
| 1.8 V targets | **untested**, see the warning below | |
| Weak signal, long-run stability | **untested** | |
| USB and WiFi clients at the same time | **untested** | serialised by a lock, not exercised |
| UART bridge (GP4 / GP5) in this build | **untested** | |
| Provisioning on Linux / Windows | Linux: tested on a pseudo-terminal only; Windows: untested | |

## Performance

pyOCD 0.46.1, nRF52840, host on wired LAN, probe on WPA2 WiFi. Wall times include connecting.

| Job | Result |
|---|---|
| connect + one `read32`, probe addressed by IP | 0.52 s |
| connect + one `read32`, addressed by `name.local` (plugin 0.1.0) | ~10 s; fixed in the current plugin, see Troubleshooting |
| dump 512 KiB (`savemem 0x0 0x80000`), SWD 1 MHz | 9.7 s |
| same, 4 MHz | 4.4 s |
| same, 10 MHz | 3.2 s |
| `pyocd flash -f 1m -O smart_flash=false --base-address 0x26000 --erase sector app.bin` (225280 B, 55 sectors) | 16.18 kB/s (pyOCD's figure), 14.4 s wall |
| the same job over USB with the stock debugprobe | 24.28 kB/s |

## Hardware and wiring

Same pins as upstream `debugprobe_on_pico2`:

| Pico 2 W pin | Signal |
|---|---|
| GP2 | SWCLK |
| GP3 | SWDIO |
| GP1 | target reset (optional, active low) |
| GP4 | UART TX (to the target's RX) |
| GP5 | UART RX (from the target's TX) |
| GND | target GND (always connect it) |

GP23 / 24 / 25 / 29 belong to the WiFi chip. Series resistors (e.g. 1 kΩ) in SWCLK / SWDIO
are a good idea and were used in the tests. The probe does not power the target.

**1.8 V targets.** The Pico 2 W's I/O runs at 3.3 V and cannot be changed. Driving 3.3 V into
a target powered at 1.8 V exceeds the usual absolute maximum (VDD + 0.3 V) and can back-power
the target through its protection diodes; a 1.8 V high level from the target is below the
RP2350's guaranteed input-high level at 3.3 V I/O, so reads may be unreliable. This is the same
with the stock USB debugprobe. Use a level translator referenced to the target's VDD, or debug
while the target runs at 3.3 V. Untested here.

## Quick start

### 1. Flash

Hold BOOTSEL, plug the Pico 2 W in, copy `dist/debugprobe_on_pico2_w.uf2` to the `RP2350`
drive. The LED blinks 3 times quickly, then flashes once every 2 s (alive, not configured).
macOS asks to allow the new USB accessory; until you allow it, the device shows up without
any interfaces.

### 2. Put it on your WiFi

The script needs Python 3 and pyserial; it finds the probe by its USB id (2e8a:000c) and
asks for the WiFi password without echoing it (or reads `$WIFIPROBE_PSK`).

```sh
# with uv:
uvx --with pyserial python host/wifiprobe-provision.py --ssid "MyNetwork" --country DE --hostname myprobe --allow 192.168.1.0/24
# or: pip install pyserial; python3 host/wifiprobe-provision.py ...
uvx --with pyserial python host/wifiprobe-provision.py --reboot      # applies the country code
uvx --with pyserial python host/wifiprobe-provision.py --status      # shows the IP address
```

macOS: tested (the probe appears as `/dev/cu.usbmodem*`). Linux (`/dev/ttyACM*`): the script is
tested against the firmware's console code on a pseudo-terminal, not on a real probe; you may
need to be in the `dialout` / `uucp` group. Windows (`COMx`): untested; it should work the same
way with `--port COMx`.

Settings are stored in the Pico's own flash and survive firmware updates. Nothing is compiled in.

### 3. Find it

`--status` prints the IP address; the probe also answers mDNS as `<hostname>.local`
(e.g. `myprobe.local`), and your router's DHCP list shows it. Giving it a fixed DHCP lease
is recommended.

### 4. Use it

**pyOCD** (tested with 0.46.1) with the plugin from this repo:

```sh
# without installing anything (uv):
uvx --from pyocd --with ./host/pyocd-daptcp pyocd commander -u daptcp:192.168.1.50 -t nrf52840 -c "read32 0x10000104"
# or install the plugin next to pyOCD:
pip install ./host/pyocd-daptcp
pyocd commander -u daptcp:myprobe.local -t nrf52840 -f 4m
pyocd load      -u daptcp:192.168.1.50:4441 -t nrf52840 firmware.hex
pyocd erase --mass -u daptcp:192.168.1.50 -t nrf52840     # untested on hardware
pyocd gdbserver -u daptcp:192.168.1.50 -t nrf52840
```

The unique id after `daptcp:` is `host` or `host:port` (default port 4441).

**OpenOCD**: needs a build from git master (no release has the TCP backend yet; configure with
`--enable-cmsis-dap-tcp`). Untested against the real probe.

```sh
openocd -c "adapter driver cmsis-dap; cmsis-dap backend tcp; cmsis-dap tcp host 192.168.1.50" \
        -f target/nordic/nrf52.cfg -c "init; echo [mdw 0x10000104]; shutdown"
openocd -c "adapter driver cmsis-dap; cmsis-dap backend tcp; cmsis-dap tcp host 192.168.1.50" \
        -f target/nordic/nrf52.cfg -c "init; nrf52_recover; program firmware.hex verify reset exit"
```

**probe-rs**: master since 2026-10 has `--probe tcp://cmsis-dap@192.168.1.50:4441`. Untested.

**USB**: unchanged; the probe is still a USB CMSIS-DAP v2 probe for any tool.

### Going back to the stock firmware

Optionally run `erase` in the console first (wipes the stored WiFi password), then BOOTSEL and
copy `debugprobe_on_pico2.uf2` from <https://github.com/raspberrypi/debugprobe/releases>.

## LED codes

The LED belongs to the WiFi chip, so it only works once that chip has started.

| Pattern | Meaning |
|---|---|
| 3 quick blinks right after power-up | firmware running, WiFi chip OK |
| then N long blinks, repeated 3 times | the previous boot crashed: 1 stack overflow, 2 panic, 3 hard fault, 4 watchdog (hang) |
| short flash every 2 s | alive, no WiFi configured (or `wifi off`) |
| 1 Hz blink | joining |
| double flash every 2 s | join failed, retrying every 10 s |
| steady on | joined, no client |
| fast blink (5 Hz) | a DAP client is connected over WiFi |
| never lights | not running, WiFi chip failed, or **safe mode** (2 crashes in a row: WiFi not started, USB DAP only); the log tells which |

## Config console

Open the probe's USB serial port and type `+++wifiprobe` followed by Enter (any baud rate), or
open the port at 9727 baud. `exit`, or closing the port, returns to the UART bridge. The script
above does this for you; `--console` gives an interactive session.

| Command | Effect |
|---|---|
| `show` | settings being edited (password never shown) |
| `status` | WiFi state, IP, DAP client, firmware version, reset reason, crash count, heap, task stacks |
| `log` | boot / event log |
| `ssid <name>` | network name (rest of the line, spaces allowed) |
| `psk` | asks for the password on the next line, not echoed |
| `auth wpa2\|wpa3\|open` | security (default wpa2; only wpa2 tested) |
| `hostname <name>` | mDNS name, `a-z 0-9 -`, max 32 |
| `country <XX>` | WiFi regulatory country; `XX` = worldwide; needs `reboot` |
| `port <n>` | CMSIS-DAP TCP port (default 4441) |
| `allow <a.b.c.d/nn>\|any` | only accept DAP clients from this network |
| `wifi on\|off` | enable / disable the radio |
| `save` / `revert` / `erase` | write to flash and apply / drop edits / wipe stored settings |
| `reboot` / `bootsel` | restart / restart into the UF2 bootloader |
| `exit` | back to the UART bridge |

When the serial port is opened at a normal baud rate and the target UART has never sent a
byte, the probe prints its boot log (useful when nothing else works:
`wifiprobe-provision.py --raw-log`).

## Security

Plainly:

- The CMSIS-DAP TCP protocol has **no authentication and no encryption**. Anyone who can reach
  the probe's TCP port can read the target's memory, erase it and reflash it.
- `allow` restricts which source addresses may connect; on a shared LAN addresses can be spoofed,
  so treat it as a guard rail, not as security. Better: a network only you use, a fixed
  `allow <your-host>/32`, and power the probe only while you need it.
- The probe accepts one client at a time and only the SWD commands that OpenOCD and pyOCD use,
  with bounds (the upstream DAP core trusts its input; a crafted packet could otherwise corrupt
  the probe's memory). The network side cannot change the probe's own firmware or settings;
  settings change only over USB.
- The WiFi password is stored **in plain text** in the Pico's flash. Anyone with the Pico in
  hand can read it (BOOTSEL + a flash dump). Run `erase` before giving the Pico away.

## Troubleshooting

| Symptom | Cause / fix |
|---|---|
| macOS: device present but no serial port, pyOCD sees nothing | macOS "allow accessory" prompt not answered yet; until then the device enumerates without interfaces |
| commands take ~10 s when using `name.local` | the resolver waits for an IPv6 answer the IPv4-only probe never gives (5 s per lookup on macOS and Linux with nss-mdns; pyOCD opens the probe twice). The current plugin resolves once, IPv4 first (measured on Linux: 10.4 s to 0.36 s). OpenOCD and older plugins: use the IP address |
| pyOCD "Target not detected" / no ACK | wiring (SWDIO / SWCLK swapped, no GND), target not powered, or locked (nRF: `-O auto_unlock=true` or `nrf52_recover`, erases the chip) |
| LED never lights | see `wifiprobe-provision.py --raw-log` and `--status`: WiFi chip init failure, safe mode, or the firmware is not running |
| join failed (double flash) | `--status` shows the reason (wrong password, network not found, no DHCP); 2.4 GHz only |
| the script cannot open the console | old firmware (product string without "WiFi"), another program holds the port, or the probe crashed: replug, then `--raw-log` |

## Building from source

Needs pico-sdk ≥ 2.3.0 (with the tinyusb, cyw43-driver and lwip submodules), arm-none-eabi-gcc,
cmake, ninja.

```sh
git submodule update --init --recursive
mkdir build-pico2w && cd build-pico2w
cmake -G Ninja -DPICO_BOARD=pico2_w -DPICO_SDK_PATH=/path/to/pico-sdk ..
ninja        # -> debugprobe_on_pico2_w.uf2
```

Other `PICO_BOARD` values build upstream's firmware unchanged.

### Tests

```sh
test/host/run_host_tests.sh          # no hardware needed
scripts/check_firmware.sh            # needs PICO_SDK_PATH: build, stack usage, upstream identity
```

The host suite runs the real CMSIS-DAP core (`DAP.c`) and debugprobe's SWD layer
(`sw_dp_pio.c`) against a bit-level SWD model of an nRF52840 (DP, AHB-AP, Nordic CTRL-AP, NVMC
flash rules, FICR / UICR, minimal Cortex-M4 debug registers), behind the firmware's TCP server
code, with AddressSanitizer / UBSan. Stages: unit tests, protocol robustness, allow list,
provisioning script on a pseudo-terminal, and, when available, OpenOCD master (FICR read,
`nrf52_recover`, program + verify), pyOCD + plugin (unlock, mass erase, name resolution).
Missing tools are skipped with a message. It does not test the WiFi chip, timing or electrics.

### Other boards

Only `pico2_w` is enabled. A plain **Pico W** (RP2040) port is not expected to build as is and
would need: a board config and CMake condition for `pico_w`, a much smaller FreeRTOS heap (the
current image uses 280 KB of RAM; the RP2040 has 264 KB), a Cortex-M0+ version of the hard-fault
handler in `src/net/diag_rp.c` (it uses Thumb-2 instructions), and checking that the second PIO
block is free for the WiFi chip. Then hardware testing.

## How it differs from upstream

New files:

| Path | Purpose |
|---|---|
| `src/net/dap_tcp_proto.[ch]` | framing, command filter with bounds, packet size / count for TCP |
| `src/net/dap_tcp_server.[ch]` | single-client TCP server (lwIP sockets; also builds on POSIX for tests) |
| `src/net/net_task.[ch]` | WiFi join, mDNS, LED, DAP server task, config console task |
| `src/net/netcfg*.{c,h}` | settings record, flash sector, console |
| `src/net/diag*.{c,h}`, `src/net/plog.[ch]` | crash records across reboots, watchdog, safe mode, event log |
| `src/lwipopts.h`, `include/board_pico2_w_config.h` | lwIP options, board pins |
| `host/wifiprobe-provision.py`, `host/pyocd-daptcp/` | provisioning script, pyOCD plugin |
| `test/host/`, `scripts/` | host test suite, firmware checks, UF2 inspection |
| `dist/` | the tested UF2 |

Changed upstream files (all changes under `PROBE_NET_WIFI` or the `pico2_w` CMake branch):
`CMakeLists.txt` (WiFi build, a generated copy of lwIP's FreeRTOS `sys_arch.c` with the tick
conversion fixed for debugprobe's 20 kHz tick, optional `-fstack-usage`), `get-version.sh`
(`git describe` stamp for `pico2_w`), `src/main.c`, `src/cdc_uart.c`, `src/tusb_edpt_handler.c`,
`src/usb_descriptors.c`, `src/FreeRTOSConfig.h`, `include/board_config.h`.
`scripts/check_firmware.sh` verifies that the non-W `pico2` image stays byte-identical to the
upstream base commit.

## History and lessons

0.1.0 passed every host test and still died on the first console command on real hardware: the
console ran in the stock UART task with a 1 KiB stack, and `status` needed about 1.4 KiB. The
host simulation has megabytes of stack and could not see it; `-fstack-usage` numbers did. Since
0.2.0 the console has its own 8 KiB task, stack frames are checked by `scripts/check_firmware.sh`,
`status` reports free stack per task, and crashes leave a record that the next boot reports (LED
code, log, `status`) instead of a silent dead probe.

## Relation to upstream, licence

Fork of [raspberrypi/debugprobe](https://github.com/raspberrypi/debugprobe) (base commit
`262f962`, upstream history kept). Upstream's README is in `docs/UPSTREAM_README.md`. MIT
licence, see `LICENSE` (upstream copyright kept); new files carry an MIT SPDX header. CMSIS-DAP
(Apache-2.0) and FreeRTOS (MIT) keep their own licences. See `CHANGELOG.md`.

## Acknowledgements

- [raspberrypi/debugprobe](https://github.com/raspberrypi/debugprobe): the probe firmware this is built on.
- OpenOCD's `cmsis-dap tcp` backend (`src/jtag/drivers/cmsis_dap_tcp.c`): the protocol.
- [bkuschak/cmsis_dap_tcp_esp32](https://github.com/bkuschak/cmsis_dap_tcp_esp32): the ESP32 counterpart.
- pico-sdk, lwIP, the CYW43 driver, FreeRTOS, TinyUSB, Arm CMSIS-DAP.
