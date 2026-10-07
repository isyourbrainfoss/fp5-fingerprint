# M4q — RPMB register + SFS still generic (2026-07-26)

## Summary

| Item | Result |
|------|--------|
| `REGISTER_LISTENER` **RPMB=0x2000** | **OK** (isolated probe_level=2) |
| FILE_SERVICE + RPMB together | **OK**, full probe completes |
| RPMB **blocked traffic** during probe | **None** (no lid=0x2000 hits) |
| `qsee_sfs_open(.../fpdata/...)` | Still **SFS_ERROR_GENERIC** |
| Chip SN (with fresh L2) | **OK** |
| CAPTURE | Still soft null `0xffffff37` |
| CONFIG_RPMB on pmOS | **not set**; UFS `rpmb_rw_size=0x40`, `/dev/bsg/ufs-bsg0` present |

## Interpretation

1. **SFS does not call the HLOS RPMB listener** on this path (or fails before block).
   Android qseecomd runs `rpmb_init_service` in the **parent** before forking
   listener threads. Registration alone is not enough — SFS likely needs prior
   RPMB **provision / key material** that only exists after real UFS RPMB IO.

2. **FILE_SERVICE** continues to work for qti_fp HLOS files (`0x0202` open,
   `0x020e` read-by-fd, `0x020f` create, `0x021c` close).

3. **CAPTURE null** is independent of the missing RPMB block traffic so far;
   still `ff_trustlet_capture_image` null / shared-buffer placement.

## Kernel gap

```
# CONFIG_RPMB is not set
```

Real RPMB frames need either:
- enable `CONFIG_RPMB` + reboot, or
- UFS BSG RPMB protocol via `/dev/bsg/ufs-bsg0` (librpmb path) in-kernel.

## Module knobs

```
enable_fs_listener=1
enable_rpmb_listener=1   # registers id=0x2000; dump-only respond for now
```

## CAPTURE follow-up (same session)

Added after INIT: `SYNC_CALIB` (0x100f), `SYNC_STATS` (0x100e).

| Cmd | Result |
|-----|--------|
| SYNC_STATS | **OK** — “statistical data is synced to TA” |
| SYNC_CALIB | err=0 (missing image_processing_* keys → defaults) |
| CAPTURE 0x1013 | Still null `0xffffff37` |
| 0x1017 (REPORT_EVENT, plen 0x2e0) | soft **err=0** (log may show stale capture null) |

CAPTURE null is still TA-internal (`trustlet_core.c:1195`), not fixed by FILE_SERVICE/RPMB register. Android does full FDT/img calibration over SPI during init (see logcat); we still skip that path (`efc data not exist`).

## Next options

1. Port minimal **UFS RPMB read/write** using BSG (or enable CONFIG_RPMB).
2. RE `rpmb_init_service` SCM side effects — maybe a one-shot TZ init cmd.
3. Deeper RE of HAL `0x537d0` capture path (multi-step, not bare 0x1013).
4. Trigger Android-like **FDT/img calibration** sequence if command IDs map.
5. Optional: seed Android SFS blob from slot A if keys become accessible.

## Evidence

- `lab/fingerprint/qsee-l2-rpmb2.txt`
- `lab/fingerprint/M4p-LISTENER-FS-WORKS.md`
- `dumps/android/vendor-qsee-20260726/lib64/librpmb.so`
