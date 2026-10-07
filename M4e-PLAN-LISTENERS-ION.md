# M4e plan — listeners, ion/tzmem, fingerpr open (2026-07-26)

## Status after M4d

- **focal32** dialogue works on pmOS (version + SET_KM_KEY log path).
- **fingerpr** APP_START works; every `FF_CMD_TA_*` with Android ion framing still returns **app status `0xffffffff`**.
- Framing is no longer the main unknown (HAL + ion sizes + cmd IDs verified).

## Architecture gap (Android vs mainline)

```
Android (works)
  qseecomd
    ├─ RPMB / FS / time / SSD listeners  (register on /dev/qseecom)
    └─ incomplete-cmd loop
  qseecom-mod.ko
    ├─ secapp-region (global carveout, one-shot REGION_NOTIFICATION)
    ├─ ION/dmabuf shared buffers inside that region
    ├─ LOAD_APP + SEND_CMD ioctls
    └─ process_incomplete_cmd when result INCOMPLETE/BLOCKED
  HAL fingerprint.default.so
    └─ QSEECom_start_app(fingerpr, 0x80040) → exchange_message

Mainline pmOS (lab)
  qcom_scm + qcom_tzmem (dma_alloc_coherent + SHM bridge)
    ├─ APP_START / APP_SEND via SCM  ✓ (focal32 proves path)
    ├─ REGION_NOTIFICATION          ✗ (−22 on all tried layouts)
    └─ incomplete / listener        ✗ (explicit TODO in qcom_scm.c)
  No /dev/qseecom, no qseecomd
```

### Evidence

| Source | Finding |
|--------|---------|
| `qseecom_send_app_region()` (Lineage qseecom.c) | REGION is **global** `secapp-region` resource, not session ion |
| pmOS DT `sec-apps@808ff000` | size **0x1000** only; REGION still −22 |
| `qseecomd` strings | Starts **RPMB, FS, time, SSD, secure UI, GPFS** listeners; aborts all if RPMB/SSD init fails |
| Android `qseecom.c` | `RPMB_SERVICE = 0x2000`, `SSD_SERVICE = 0x3000` (kernel special-cases these) |
| fingerpr.b02 strings | **GPAppLib**, CTAMemorySpace, IMemSpace_map, QIS Listener |
| Android boot log | `__qseecom_reentrancy_process_incomplete_cmd` already active |
| mainline `qcom_tzmem` | SHM-bridge-backed coherent DMA — enough for focal32 SEND |

### Why SEND “works” but status is −1

SCM returns success and the first dword of the response buffer is written to `0xffffffff` (poison `0xee` is overwritten). So:

1. Buffer is TZ-visible (SHM bridge OK).
2. fingerpr’s command handler (or pre-handler) rejects every opcode we try.
3. Likely causes (ordered):
   - **No GP/session setup** that Android does via create + listeners  
   - **Missing RPMB/FS** services for TA internal init  
   - **SPI / device not visible** to TA (`focal_create_spi_instance`)  
   - Unknown remaining wire detail (less likely after ion RE)

## Concrete next experiments

### A. Android oracle (needs human once)

1. Vol Down+Power → FASTBOOT  
2. `fastboot --set-active=a && fastboot reboot`  
3. Root adb:
   - `strace -f -e ioctl,openat -p $(pidof …fingerprint…)` during unlock  
   - `ls /dev/qseecom`; `getprop vendor.sys.listeners.registered`  
   - dmesg: first fingerpr load + incomplete/listener lines  
4. `fastboot --set-active=b` and return to pmOS  

Goal: first non-`ffffffff` fingerpr rsp status and listener set that was up.

### B. Offline / pmOS (no fastboot)

1. **qseecomd RE**: extract listener ID table from `qseecomd` / `libdrmfs` / `librpmb` in vendor dump.  
2. **Minimal listener stubs** on mainline only if we ever observe SCM `result==INCOMPLETE(1)` or `BLOCKED_ON_LISTENER(2)` — so far fingerpr SEND is `result==SUCCESS` with app-level −1.  
3. **SPI L3**: implement real SPI xfer in `focaltech_fp_life` matching Android `FF_CMD_SVC_XFER_SPI_DATA` so TA INIT_SPI has a backend *if* TA reaches out via shared buffer + listener (or via kernel SPI).  
4. **Compare phys**: log `qcom_tzmem` paddr ranges vs Android ion heap (from strace /debug) after oracle.

### C. Do **not** do

- Random REGION with `QCOM_SCM_RW` / wrong owner (caused hard reset once).  
- `qbootctl` on pmOS.  
- Flashing full custom kernels for FP.

## Success criteria for M4e

Any of:

- fingerpr SEND with **status ≠ 0xffffffff** (even a structured error code), or  
- Documented hard proof that **listener N** is required (SCM BLOCKED + ID), or  
- Working SPI chip-id via L2/L3 without fingerpr (parallel ladder rung).

## Files

- Vendor extract: `dumps/android/vendor-qsee-20260726/`  
- Probe: `lab/fingerprint/oot-qsee/qsee_fingerpr.c`  
- Prior: `M4d-FINGERPR-OPEN.md`, `M4c-EXPLORE.md`
