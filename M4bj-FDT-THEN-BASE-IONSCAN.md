# M4bj — FDT-then-Base + full ion marker scan (2026-07-27)

## INIT_DEV root cause (full log)

```
qsee_sfs_open('/data/vendor_de/0/fpdata/ft_fp_serial_id.bin') = SFS_ERROR_GENERIC
__module_serial_sync: 'Null pointer'
efc data not exist, skip
cidri / chip feature...
```

SFS is **RPMB-backed** (not FILE_SERVICE). Serial Null is logged but init continues
past efc. LOGCAT ~810 B truncates so FDT/img may have run invisibly.

## Fix

1. `scan_ion_img_markers()` — scan full `REQ_ION` for Base update / Step1 / dac /
   malloc fail / host omit / usMiddleTemp (not just 810 B LOGCAT).
2. Force calib **phase A**: FDT CAP until ready (Android order).
3. **Phase B**: WMODE-5 + flag 0x240 CAP @8M with longer SPI settle.
4. Scan ion after each phase; SYNC_CALIB after.

## Smoke

```
dmesg | grep -E 'ION-SCAN|IMG BASE CALIB OK|Base update|Step1|usMiddleTemp|FAIL'
```
