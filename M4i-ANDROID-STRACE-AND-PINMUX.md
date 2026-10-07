# M4i — Android HAL oracle + pinmux abort (2026-07-26)

## Android (slot A) — what actually probes

| Item | Finding |
|------|---------|
| Service | **`android.hardware.biometrics.fingerprint@2.1-service`** (`vendor.fps_hal`) drives probe; vendor.focaltech 1.0 also runs |
| Chip ID | **`0x98FF9024`** after probe |
| SPI config | `enable_spidev=off`, `spi_bus_num=14`, mode 0, 4 Mbps |
| QSEE handles | `sb_length=0x80040` loads **focal32**; brief second handle `0x840` on app id 1 (keymaster resident) then shutdown |
| Sequence | attach → SYNC config → **probe** (no separate INIT_SPI log line); then full TA calibration @ ~8 MHz SPI |
| Kernel | GPIO power/reset only; **`use_pinctrl:0`**; `ff_set_spiclk` exists but no spiclk lines during successful probe |
| Conclusion | Fingerprint SPI is **100% TZ/TA** on stock. HLOS does not need geni_spi for success on Android |

### Strace tooling

- Stock `/data/local/tmp/strace` is **glibc** → needs `/lib/ld-linux-aarch64.so.1` (fails on Android).
- Working approach: Alpine musl mini-root under `/data/local/tmp/muslroot/` + loader.
- Bind-mount HAL wrap needs SELinux label `u:object_r:hal_fingerprint_default_exec:s0` (+ often `setenforce 0`).
- Dump dir: `dumps/android/fp-strace-20260726-195115/` (+ `NOTES.md`).

## pmOS retest (safe path)

Baseline after soft switch **b**:

| Cmd | err |
|-----|-----|
| SYNC_CFG | **0** (“configurations is synced”) |
| INIT_SPI | **`0xfffffffb` (−5)** |
| PROBE | **`0xffffff33`** |
| HEALTH | **0** |

L2 v0.5: VDD/RESET OK, wrap1 SE6 clocks + ICC OK; **pinctrl gpio56–59 still −EINVAL** (reserved).

## Pinmux experiment — ABORT

`fp_pinmux_se14.ko` (un-reserve gpio56–59 + pinctrl qup16, **no** geni bind):

- SSH died immediately after `insmod`
- USB ping partially alive; unit watchdog-rebooted to clean pmOS (~0 min uptime)

**Do not load** `fp_pinmux_se14` or `fp_spi_se14` until redesigned and serial-console safe.

## Implication

1. Android proves TZ SPI works **without** HLOS pinmux of gpio56–59.
2. Forcing REE pinmux on mainline is **unsafe** on this unit.
3. INIT_SPI −5 is almost certainly **TZ SPI backend / secure SE assignment** under mainline boot, not missing HLOS clocks (already voted) or missing pinctrl (Android also skips HLOS pinmux).

## focal32 TA RE (SPI path)

From concatenated TA image strings (`/lib/firmware/qsee/focal32.*`):

```text
attaching to QSEE_SPI_DEVICE_%u (QUP%u SE %u)...
attaching to SENSOR SPI_%u...
error at %s[%s:%u]: qsee_spi_open(%u) = %d.
error at %s[%s:%u]: un-supported spi device id %u(0x%08x).
ff_spi_init / __open_spi / __ensure_spi
qsee_spi_write / qsee_spi_full_duplex / qsee_spi_close
```

INIT_SPI on pmOS returns **err=−5** with TA logs that only show entry/`message_cb` (no `qsee_spi_open` line in the 298-byte log window). Same chip TA works on Android → **`qsee_spi_open(bus)` fails under mainline HLOS/boot**, not a wrong command opcode.

Likely root cause class: **TZ SPI device table / hyp secure-SE assignment** differs when booting pmOS DT vs Android vendor boot (same signed TAs).

## Next (highest likelihood, safe)

| # | Experiment |
|---|------------|
| 1 | Map `driver.spi_bus_num` → `QSEE_SPI_DEVICE_N (QUP x SE y)` inside focal32; confirm 14 → wrap1 SE6 |
| 2 | Compare **Android vs pmOS** hyp/DT / bootargs for SPI SE ownership (no REE pinmux force) |
| 3 | Optional incomplete/SVC listener for `FF_CMD_SVC_IOC_SPI_CLOCK` (low priority: APP_SEND already completes with −5) |
| 4 | Soft-boot Android only for more logcat — **never** load `fp_pinmux_se14` / `fp_spi_se14` without serial |

## Safety

- Soft slot switch: `lab/reboot-mode-oot/switch-slot.sh a|b`
- Never `qbootctl` on pmOS
- Never load experimental SE14 geni/pinmux modules without serial console
