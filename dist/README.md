# Prebuilt firmware

| File | sha256 | Built from | Hardware status |
|---|---|---|---|
| `debugprobe_on_pico2_w.uf2` | `e2e8ded160663193059e6e5804de28387341643b7bdd9d8313a895993d4d96bb` | tag `v0.2.0` (tree `d455caf`), pico-sdk 2.3.1, arm-none-eabi-gcc 16.2.0 | **tested** on one Pico 2 W, see the README |

Notes:

- This build reports its version as `f604a7e` in `status`: it was built from an uncommitted
  tree whose parent was the first fork commit (hash before a later author rewrite; that
  commit is now tag `v0.1.0`). Builds from later commits stamp `git describe` output
  instead (e.g. `v0.2.0-5-gabc1234`).
- Rebuilding tag `v0.2.0` in the same directory reproduces this file except for the
  version stamp; images built elsewhere differ in embedded build paths.
- Only for the **Pico 2 W** (RP2350 + CYW43439). Not for the Pico W, Pico 2 or the
  Debug Probe.
- The stock firmware (to go back): `debugprobe_on_pico2.uf2` from
  <https://github.com/raspberrypi/debugprobe/releases>.
