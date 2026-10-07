# M4e — Android oracle: working probe path (2026-07-26)

## Result

On stock Android (slot A, Magisk), FocalTech HAL init **succeeds**:

- `ff_trustlet_probe_device ret = 0`
- Chip id **`0x98FF9024`**, FDT/img calibration OK  
- Trustlet version **`v2.5.3.0-cdf0f80-230614@0928`** (same string as pmOS **focal32** base)

Capture: `dumps/android/fp-qsee-oracle-20260726-151958/`

## Live QSEECom sequence (logcat)

```
QSEECom_get_handle sb_length = 0x80040   → load → APP id = 7   ("TA is attached")
QSEECom_get_handle sb_length = 0x840     → already loaded id=1
  ta:base  TEE environment checking… TA is successfully created
  ta:entry QSEE TA is ready
QSEECom_shutdown_app app_id=1
  configurations is synced to TA
  ff_trustlet_probe_device ret = 0
  fingerprint device is found
  … FDT/img calibration …
  fingerprint device(id=0x98FF9024) is successfully initialized
  TA is successfully initialized
```

## Config that matters

| Item | Value |
|------|--------|
| `common.trustlet_path` | `/vendor/firmware_mnt/image/focal32` |
| `common.keymaster_path` | `…/keymaster64` |
| `driver.enable_spidev` | **off** (SPI inside TA, not userspace spidev) |
| `driver.spi_bus_num` | **14** |
| Session sizes | **0x80040** then **0x840** |

## pmOS after mirroring sequence

Module: `qsee_fingerpr.ko` Android-seq (fingerpr START first → focal32 → base/entry → CREATE/INIT/SYNC/PROBE).

| Step | pmOS |
|------|------|
| START fingerpr / focal32 | OK (ids 4 / 5 this run) |
| focal32 `0x205` / `0x1011` | **OK** (version + entry strings) |
| fingerpr ion CREATE/INIT/SYNC/PROBE | **All `rsp=0xffffffff`** |

So command IDs + ion sizes + order are **not sufficient**. Remaining gap is environmental:

1. **Android qseecom shared-buffer bind at start_app** (ion registered with the app handle) vs our free-standing `qcom_tzmem` SEND buffers  
2. **qseecomd listeners** (RPMB/FS/etc.) — boot already shows incomplete-cmd handling for app_id=1  
3. **SPI bus 14** visibility to the TA (sensor init does real SPI: “set chip vio 1.8v success”)

focal32 still proves mainline SCM SEND works; fingerpr’s app-level reject is consistent with “not the same client session Android created.”

## Next experiments

1. Reverse Android `qseecom` **shared buffer registration** SCM (what binds ion phys to app_id at start).  
2. Minimal **listener** if any fingerpr SEND ever returns SCM `INCOMPLETE`/`BLOCKED` (so far: SUCCESS + app −1).  
3. SPI bus mapping on pmOS (geni SPI ↔ Android bus 14) for L3 parallel path.  
4. Optional: run userspace `libQSEEComAPI` under bionic/chroot if we ever have `/dev/qseecom` (would need Android qseecom.ko — high risk).

## Soft bootloader (related lab win)

Slot A↔B without keys: `lab/reboot-mode-oot/` + `systemctl reboot --reboot-argument=bootloader`.
