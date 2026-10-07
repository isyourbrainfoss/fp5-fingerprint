# M4n — SFS / listeners are the real gap (2026-07-26)

## Decision

After M4l (ENROLL kills TA) and M4m (CAPTURE soft-null), the best next investment is
**QSEE filesystem (SFS) + listener support**, not more CAPTURE payload guessing.

## Evidence

### 1. SFS open fails under pmOS

On clean boot with working SPI (chip SN present):

```
focaltech:ta:file
error at ff_file_open[file.c:82]:
  qsee_sfs_open('/data/vendor_de/0/fpdata/ft_fp_serial_id.bin', ..) = 'SFS_ERROR_GENERIC'
error at ff_file_open[file.c:83]: 'I/O error'
error at __module_serial_sync[device.c:136]: 'Null pointer'
```

Evidence: `lab/fingerprint/qsee-create-capture.txt` (INIT_DEV logs).

Android provides SFS via **qseecomd FS listener** (`fs_start` / “file system services”).
pmOS mainline `qcom_scm` has **no listener implementation**
(`BLOCKED_ON_LISTENER` only WARN_ON in `qcom_scm.c`).

### 2. CAPTURE soft-fails with null (TA stays alive)

```
error at ff_trustlet_capture_image[trustlet_core.c:1195]: 'Null pointer'
The response data should be placed in shared memory or another piece of malloc memory.
err=0xffffff37
HEALTH final er=0
```

Tried: zero/flag/dims/large in-band image/phys/VA/segmented. No pixel write.
Android ion layout confirmed: `usr = ion+0x10`, plen `0x24` for capture_ctx.

### 3. ENROLL still SCM −22 (kills TA)

Independent of HAT / segmented / wake. Not a soft reject like Android’s
“invalid enrollment challenge”.

### 4. CREATE

`try_create=1`: soft `err=0xffffff35`, **does not** always poison later cmds
(INIT_SPI/INIT_DEV still SN-ok on this run). Still no CAPTURE fix.

### 5. FP HAL does not register listeners

`fingerprint.default.so` imports only `QSEECom_start_app` / `send_cmd` / `shutdown_app`.
Listeners come from **qseecomd** (RPMB, FS, SSD, …).

## Interpretation

| Layer | Status on pmOS |
|-------|----------------|
| SPI / device probe / SN | **Works** |
| High-level PRE_ENROLL | **Works** (challenge) |
| SET_GROUP | Works, 0 templates |
| CAPTURE / ENROLL / AUTH | Need **SFS + listeners** (and/or full ion session semantics) |
| Mainline SCM | APP_SEND only; no listener resume |

CAPTURE null and SFS failures are the same class of problem: TA expects REE
services (file store, response buffer placement) that Android’s qseecom stack
provides and mainline does not.

## Plan (M4n → unlock)

1. **Instrument APP_SEND** to surface `result=2 BLOCKED_ON_LISTENER` + listener id  
   (`resp_type=0xEE02`) — in `qsee_fingerpr` via `app_send_ex` (done).
2. **Minimal FS listener** for paths under `/var/lib/fp5-fp` and Android
   `/data/vendor_de/0/fpdata` (open/read/write/close), registered the way
   vendor `qseecom_register_listener` / qseecomd `fs_start` do.
3. Re-test CAPTURE → then ENROLL with finger on sensor.
4. Template store on REE; AUTH; Phosh unlock glue.

Optional parallel: dump templates from rooted Android slot and import if SFS
keys become visible once FS listener is up.

## Module knobs

```
skip_enroll=1     # default — do not kill TA
try_create=0|1    # optional CREATE soft-err experiment
```

## Artifacts

- `lab/fingerprint/M4l-ENROLL-PATH.md`
- `lab/fingerprint/M4m-CAPTURE-PATH.md`
- `lab/fingerprint/qsee-create-capture.txt` (SFS errors + SN)
- `artifacts/qsee-modules/qsee_fingerpr.ko`
