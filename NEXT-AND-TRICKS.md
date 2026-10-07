# Fingerprint: remaining logs + pmOS tricks

## Extra Android logging (done 2026-07-25)

Under `dumps/android/fp-oracle-extra-20260725-230120/`:

| Item | Status |
|------|--------|
| Enroll + unlock success + fail captures | Done earlier |
| TA **sha256** of `focal32.*` / `fingerpr.*` | Done |
| `focal32.mdt` / `fingerpr.mdt` headers | Done |
| HAL binary + `fingerprint.default.so` | Done |
| `focaltech_fp.ko` + strings | Done |
| ioctl names / SPI-via-TA commands | Done (strings) |
| Full multi‑MB `fingerpr.b02` etc. | Optional (disk); hashes sufficient for identity |

### Optional later (low ROI for “life”, high ROI for RE)
- Pull **full** TA segments (~11 MB) if reverse-engineering offline
- `strace -p` on HAL during one unlock (needs live fail/success session)
- Dump `ff_info` sysfs if exposed after open

## Architecture takeaway (from strings)

Userspace talks to **`/dev/focaltech_fp`** via ioctls (`FF_IOC_INIT_DRIVER`, `FF_IOC_RESET_DEVICE`, …)  
**and** to **QSEE** via `QSEECom_start_app` / `QSEECom_send_cmd` with TAs **`focal32` / `fingerpr`**.

SPI is **not free Linux SPI** for image path in the HAL model:
- `FF_CMD_TA_XFER_SPI_DATA`, `FF_CMD_TA_INIT_SPI`, `driver.enable_spidev`, gpio_ck/mosi/miso pins in config  
- SPI can be kernel spidev **or** TA-mediated; either way **match is in TA**

Kernel driver source path in ko: `drivers/input/finger/focal_finger/ff_core.c`  
GPIOs: irq / reset / vdd request; pinctrl `fpsensor_*`.

## Tricks for “some life” under pmOS

### Tier A — doable without TEE (we started)
1. **Power + reset GPIOs** (VDD 60, RESET 35) + **poll IRQ 34**  
2. DT stub node for documentation  
3. Port minimal **`focaltech_fp` chardev** (ioctl surface only, no TA)  
   - Goal: `/dev/focaltech_fp` open, reset, maybe chip ID if any register is normal-world  
4. If `driver.enable_spidev` path exists: expose **spidev** and probe ID registers  

**Success metric:** IRQ edges on touch after power-up, or SPI ID read — **not** unlock.

### Tier B — hard
5. Load QSEECOM on mainline + start `fingerpr` TA (signature + SCM + vendor policy)  
6. RE ioctl + TA command stream from Android strace  

### Tier C — practical “hack”
7. Keep **fingerprint on Android slot A**; pmOS for daily use  

## What we will NOT get quickly
- fprintd unlock without TA  
- Dropping Android `.ko` on 7.1.2  

## GPIO map (Android oracle)
| Signal | TLMM |
|--------|------|
| IRQ | 34 |
| RESET | 35 |
| VDD | 60 |

Script: `lab/fingerprint/pmos-fp-gpio-poke.py`
