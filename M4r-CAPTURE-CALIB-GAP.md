# M4r — CAPTURE blocked by missing FDT/img calibration (2026-07-26)

## Finding

Android init log (successful enroll path) shows, **during device init**:

1. `last time saved efc data: 128`
2. **FDT calibration** (DAC search → success)
3. **Img calibration** steps 1–3 + `Base update success`
4. `fingerprint device(id=0x98FF9024) is successfully initialized`
5. `initializing the algorithm module...`
6. `TA is successfully initialized`

pmOS `INIT_DEV` / `INIT` with working SPI SN shows:

1. SN OK  
2. `qsee_sfs_open(...ft_fp_serial_id.bin) = SFS_ERROR_GENERIC`  
3. `efc data not exist, skip`  
4. `cidri` + chip feature…  
5. **No FDT / img / algorithm / “successfully initialized”** lines in any TA log window  
6. CAPTURE always `ff_trustlet_capture_image … Null pointer` (`err=0xffffff37`)

## Interpretation

CAPTURE’s null pointer is almost certainly an **uninitialized image pipeline** inside the TA
(missing calibration bases / buffers that Android builds during init), not just wrong
0x24 capture_ctx padding.

EFC + serial sync are **SFS-backed**. Without SFS/RPMB, efc is missing and the
Android-style FDT/img calibration chain may never run (or runs but never logs in the
810-byte response window). We never observe FDT strings on pmOS.

## Tried this session

| Experiment | Result |
|------------|--------|
| SYNC_CALIB / SYNC_STATS | Stats sync OK; no FDT |
| CHIP before INIT, DEAD_PIXEL, FP_IS_TYPE | Soft OK; no calib |
| Probe ids 0x98FF9024 / 0x9024 / 0x9391 / 0x9362 | No new init success line |
| Larger log dump (req+rsp) | Still no `fdt` / `algorithm module` |
| Mount Android userdata for efc | **Fails** (encrypted FBE) |

## Blockers to unlock

```
SFS/RPMB → efc + serial + templates
        → FDT/img calibration
        → CAPTURE buffers non-null
        → ENROLL/AUTH
        → Phosh
```

FILE_SERVICE alone is not sufficient for the focaltech fpdata/SFS path.

## Next options (ordered)

1. **UFS RPMB real IO** (`/dev/bsg/ufs-bsg0`, `rpmb_rw_size=0x40`) so SFS can open.  
2. Boot Android, dump efc/serial via rooted shell to `/sdcard`, pull to pmOS  
   (if files are REE-visible; may still be TEE-wrapped).  
3. Deeper TA RE: which branch skips FDT when efc missing — force-recalib cmd.  
4. Optional: enable `CONFIG_RPMB` in pmOS kernel (reboot).

## Related

- `M4p-LISTENER-FS-WORKS.md` — FILE_SERVICE live  
- `M4q-RPMB-SFS.md` — RPMB register without SFS traffic  
- `dumps/android/fp-strace-20260726-195115/logcat-qsee-seq.txt` — Android calib oracle  
- `dumps/android/fp-enroll-first-20260725-224510/SUMMARY.md` — enroll capture path  
