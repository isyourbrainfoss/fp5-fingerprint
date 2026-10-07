# Fingerprint blocker — FocalTech FT9362 / QSEE on Fairphone 5

## Goal status

**Unlock / enroll / identify:** not usable on postmarketOS mainline without proprietary TEE integration.
Documented hard blocker with Android oracle (2026-07-25 slot A Magisk dump).

**Kernel foothold (L2, 2026-07-25):** separate from unlock — OOT `focaltech_fp_life` provides `/dev/focaltech_fp`, power/reset, IRQ 34 on live pmOS. See `lab/fingerprint/README.md` (“FP life: L2”). This does **not** remove the QSEE blocker for match.

## Android oracle (live, this unit)

| Fact | Evidence |
|------|----------|
| Sensor | DT node `/soc/focalfp_ft9362`, compatible `focaltech,fp` |
| Chip | **FT9362** (node name + Fairphone repair PDF power-button FP) |
| Kernel | `focaltech_fp.ko` vermagic 5.4.274-qgki; `/dev/focaltech_fp` |
| IRQ | **TLMM GPIO 34** Edge (`msmgpio 34` → `focaltech_fp`) |
| Reset GPIO | **TLMM 35** (`focaltech_fp,reset-gpio`) |
| VDD GPIO | **TLMM 60** (`focaltech_fp,vdd-gpio`) |
| SPI note | `spi0.0` @ `a94000` is **Goodix touch** (`goodix,brl-a`), **not** FP |
| HAL | `vendor.focaltech.hardware.biometrics.fingerprint@1.0-service` |
| QSEE | `qseecomd` + `vendor.qti.hardware.qseecom@1.0-service` running |
| TEE apps | `focal32.mdt` + `fingerpr.mdt` (and .b00–.b07) in `/vendor/firmware_mnt/image/` |
| Enrolled prints | **0** on dump session (`FingerprintProvider` prints count 0) |
| Physical | Repair PDF: scanner **integrated in power button** |

Dump tree: `dumps/android/fp-oracle-20260725/`

## Live pmOS probe

| Check | Result |
|-------|--------|
| fprintd | No sensor |
| SPI | Only Goodix touch on mainline path |
| DT FT9362 | Absent |
| qseecom | `untested machine, skipping` |
| Android ko | Cannot load on 7.1.2 |

## Why mainline cannot “just enable” it

1. No FT9362 DT / GPIO power sequencing in mainline board DTS.
2. No mainline driver that loads QSEE TAs `focal32` / `fingerpr` for match.
3. Stock pmOS skips QSEECOM on this board.
4. libfprint has no backend for this TZ path.

SPI electrical probe of GPIOs is possible later but **does not unlock enroll/match**.

## Conclusion

Fingerprint remains **Broken / blocked by TrustZone (QSEE TAs)**.
NFC and media paths are the viable short-term wins.

## Enroll capture (2026-07-25)

First fingerprint enrolled under logging. IRQ 34 active only during enroll. HAL `ft93xx` captures raw frames then `invoke_ta`. See `dumps/android/fp-enroll-first-20260725-224510/SUMMARY.md`.
