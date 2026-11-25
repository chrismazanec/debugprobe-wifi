# AGENTS.md: brief for coding agents working in this repo

## Scope and goal

debugprobe-wifi is Raspberry Pi's debugprobe firmware plus a WiFi transport for the
**Pico 2 W**: CMSIS-DAP over TCP using the framing of OpenOCD's `cmsis-dap backend tcp`,
a pyOCD probe plugin, and a provisioning script. Goals, in this order: USB DAP must keep
working exactly like the stock probe in every case (including when WiFi fails); the network
side must never be able to corrupt the probe; non-W builds stay identical to upstream.

## Repo map

| Path | What |
|---|---|
| `src/net/` | everything new on the firmware side (see README "How it differs from upstream") |
| `src/main.c`, `src/cdc_uart.c`, `src/tusb_edpt_handler.c`, `src/usb_descriptors.c`, `src/FreeRTOSConfig.h`, `include/board_config.h`, `CMakeLists.txt`, `get-version.sh` | upstream files with hooks under `PROBE_NET_WIFI` / the `pico2_w` CMake branch |
| `include/board_pico2_w_config.h`, `src/lwipopts.h` | board pins, lwIP options |
| `host/wifiprobe-provision.py` | provisioning / diagnostics over the USB serial port |
| `host/pyocd-daptcp/` | pyOCD plugin (`-u daptcp:host[:port]`) |
| `test/host/` | host test suite (`run_host_tests.sh`), simulator, unit tests |
| `scripts/check_firmware.sh` | firmware build, stack-usage limit, upstream identity |
| `scripts/uf2info.py` | UF2 family / range / image-definition check |
| `dist/` | the hardware-tested UF2 + SHA256SUMS + provenance note |
| `docs/UPSTREAM_README.md` | upstream's README |

## Build

```sh
git submodule update --init --recursive
mkdir build-pico2w && cd build-pico2w
cmake -G Ninja -DPICO_BOARD=pico2_w -DPICO_SDK_PATH=/path/to/pico-sdk .. && ninja
```

## Checks before every commit

1. `test/host/run_host_tests.sh`: 0 failures. Set `OPENOCD=` (OpenOCD git master built with
   `--enable-cmsis-dap-tcp`) and have `uv` available so the OpenOCD / pyOCD stages run instead of
   being skipped; report the pass / fail / skip counts.
2. For any firmware change: `PICO_SDK_PATH=... scripts/check_firmware.sh`: W build without
   warnings in fork files, largest stack frame below the limit, `pico2` image byte-identical
   to upstream.
3. Add a test for every new rule (unit test in `test/host/unit.c`, protocol test in
   `test/host/test_server.py`, or a stage in `run_host_tests.sh`).

## Rules that bite

- **Stack sizes.** 0.1.0 died on hardware because the config console ran in the stock UART task
  (1 KiB stack) and `status` needed ~1.4 KiB: stack overflow hook, whole firmware stopped, USB
  DAP dead. Measure with `-fstack-usage` (`scripts/check_firmware.sh`), add up the call chain,
  and compare with the task's stack (words x 4 bytes). The host simulation cannot catch this.
  Keep big buffers static. `status` shows the free stack of the WiFi tasks: watch it.
- **Never block or do real work in TinyUSB callbacks** (`tud_*_cb`): they run in the 1 KiB TUD
  task. Set a flag and let another task do the work (see `netcon_enter`).
- **Priorities.** Every WiFi-side task, including the CYW43 async context (created in
  `net_task.c`, not the SDK default +4) and lwIP's tcpip thread, runs at `tskIDLE_PRIORITY + 1`,
  below the USB task (+2) and the UART task (+3). Nothing on the network side may starve USB.
- **PIO.** The SWD state machine uses pio0 SM0 without claiming it. The CYW43 driver claims a
  free PIO at init, so `cyw43_bringup()` parks pio0 first. Keep that, and keep new PIO users off
  pio0.
- **Tick.** debugprobe runs FreeRTOS at 20 kHz, so `portTICK_PERIOD_MS` is 0. lwIP's FreeRTOS port
  divides by it; `CMakeLists.txt` builds a patched copy (`generated/lwip_sys_arch_fixed.c`) and
  fails the configure step if the SDK file changes. Never use `portTICK_PERIOD_MS`; use
  `pdMS_TO_TICKS`.
- **Flash.** Settings live in the last 4 KiB sector (`netcfg_t`, magic + version + CRC, fits one
  page, `netcfg_flash.c`). Writes go through `flash_safe_execute` and wait while the CYW43 firmware
  is being loaded. Never compile credentials, SSIDs or test networks into the firmware, the
  tests or the docs.
- **Network command filter** (`dap_tcp_request_ok`): DAP.c trusts its input. Every command allowed
  over TCP must have a proven bound inside `DAP_TCP_BUF_SIZE`; batch / vendor / JTAG / SWO commands
  stay refused. Changing it needs a unit test with the worst case.
- **Non-W builds** must stay byte-identical to upstream (`scripts/check_firmware.sh` part 3).
  Hooks go under `#ifdef PROBE_NET_WIFI` or the `pico2_w` CMake branch.
- **Diagnostics.** Watchdog scratch registers 0..3 hold the crash record (4..7 belong to the
  SDK / bootrom). `_exit`, `isr_hardfault`, the stack-overflow and malloc-failed hooks end in
  `diag_fault()`. Two crashes in a row = safe mode (no WiFi). Keep the LED codes, the log and the
  README table in sync.

## Hardware testing protocol

Every reflash needs a human to press BOOTSEL, so each hardware run must give a verdict:

- Change as little as possible per run; say up front what result confirms or refutes what.
- Give the human exact commands (copy-pasteable, for their OS) and the list of observations to
  send back: the LED pattern during the first 10 s and afterwards; `pyocd list`; full output of
  `wifiprobe-provision.py --raw-log`, `--status` and `--log`; the exact tool command and its full
  output; wall times.
- Prepare the fallback: what to run if it fails (e.g. `--raw-log` after a replug shows the
  previous boot's crash reason and stage).
- Put the UF2 to test somewhere stable with its sha256, and record which commit it was built from.

## Reporting

- Always separate **tested on hardware**, **tested on the host** and **untested**; give numbers
  (counts, sizes, times, checksums) and say how they were measured.
- Update the README status table, the performance table and `CHANGELOG.md` with every hardware
  result; never mark something tested that was only simulated.
- `dist/` holds only hardware-tested images; a rebuilt image is untested until a run says otherwise.

## Repo etiquette

- Do not push and do not create remote repositories; commits stay local unless the owner says
  otherwise.
- No personal data anywhere (names, real SSIDs, real IP addresses, host names, e-mail addresses):
  use placeholders such as `192.168.1.50`, `myprobe.local`, `MyNetwork`.
- Commit messages: imperative subject line, a body that says what and why with the numbers;
  no attribution or "generated by" lines.
- New files get the MIT SPDX header used in `src/net/`.
