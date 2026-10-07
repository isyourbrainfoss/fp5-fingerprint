# M4g — SPI bus 14 / SE14: HLOS resources exhausted (2026-07-26)

## Mapping (confirmed both OSes)

| Item | Value |
|------|--------|
| Android `driver.spi_bus_num` | **14** |
| Android pinctrl | `qupv3_se14_spi_*` → **gpio56–59**, function **qup16** |
| Physical SE | wrap1 **SE6** @ `0xa98000` |
| Android DT | **no** `spi@a98000`; only `i2c@a98000` **disabled** (same GENI SE, protocol not HLOS) |
| Android Linux SPI | only **spi0** = Goodix @ `a94000` (touch) |
| pmOS | `spi@a98000` status=disabled; pins in `gpio-reserved-ranges` |

**Conclusion:** Fingerprint SPI is **100% TA/TZ-mediated** on Android. HLOS never binds an SPI master for SE14.

## Android deep oracle

Dump: `dumps/android/fp-spi-deep-20260726-191626/`

- Probe **OK**, chip **0x98FF9024**, no info-level `QSEE_SPI_DEVICE` log
- dmesg: power/reset only; **no** `spiclk` / `ff_set_spiclk` lines
- `/dev/qseecom` + `vendor.sys.listeners.registered=true`
- Sequence: attach TA → “successfully created” → SYNC_CFG → **probe** (no separate INIT_SPI in logcat)

## HLOS resource stack on pmOS (L2 v0.5)

| Resource | Status after enable |
|----------|---------------------|
| VDD gpio60 + reset sequence (end HIGH) | OK |
| `gcc_qupv3_wrap1_s6_clk` (SE) | **ON** |
| wrap1 m/s AHB | **ON** |
| wrap1 **core** + **core_2x** | **ON** (v0.5; were off before) |
| ICC qup-core / qup-config (wrap1) | **50000** (was 0) |
| pinctrl gpio56–59 | **FAIL** reserved (−EINVAL); force TLMM = reboot risk |

## TA results (all resource combos)

| INIT_SPI | PROBE | HEALTH / SYNC |
|----------|-------|----------------|
| **−5** (`0xfffffffb`) | **0xffffff33** | **0** |

Unchanged across: bus 0–15, enable_spidev 0/1, clocks±ICC±core±reset polarity.

APP_SEND returns SCM **SUCCESS** with app-level err (not incomplete/listener).

## Interpretation

We have done everything reasonable on the **HLOS clock / interconnect / power** side that Android appears **not even to log**. Remaining failure is almost certainly:

1. **TZ SPI stack** cannot open SE14 under mainline boot (secure assign / hyp / SMMU / QSEE SPI table), **or**
2. TA needs **CA service callbacks** (`FF_CMD_SVC_IOC_SPI_CLOCK` / `XFER_SPI_DATA`) with a full incomplete loop — but our sends complete with −5, not incomplete, so (1) is more likely for INIT_SPI.

## Artifacts

- L2: `focaltech_fp_life` **v0.5** — clocks + ICC + wrap1 core
- `artifacts/fp-modules/focaltech_fp_life.ko`
- Android: `dumps/android/fp-spi-deep-20260726-191626/`

## Next work (needs design choice)

| Path | Effort | Notes |
|------|--------|--------|
| **A. REE SPI + SVC incomplete handler** | Large | Implement app_send that handles `INCOMPLETE`/`BLOCKED`, service XFER on real pins (pinmux reserved — hard) |
| **B. TZ/hyp SPI assign research** | Unknown | Compare vendor boot vs mainline secure peripheral tables |
| **C. L3 bitbang/GENI chip ID outside TZ** | Medium/risky | Proves wiring; pinmux/MMIO previously rebooted unit |
| **D. Pause SPI; document M4g hard stop** | — | Unlock blocked until SPI open |

## Safety

- Do not `force_spi_pinmux=1` or raw GENI MMIO without serial console.
- Soft slot switch via `lab/reboot-mode-oot/switch-slot.sh` works.
- Avoid `rmmod` while `/dev/focaltech_fp` held open.
