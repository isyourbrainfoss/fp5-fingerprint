# M4m — Capture path without ENROLL (2026-07-26)

## Why

`ENROLL` (0x2001) SCM−22 **kills** focal32 (see M4l). High-level enroll is blocked until QSEECom listener/callback support exists. M4m explores **low-level capture** while keeping the TA alive.

## Default module behaviour

```
insmod qsee_fingerpr.ko skip_enroll=1   # default
```

- Runs device init + SET_GROUP + PRE_ENROLL (safe)
- Work modes 0–4, START_SCANNING
- CAPTURE_IMAGE (0x1013, plen 0x24) trials
- **Does not** send ENROLL / AUTH on focal32
- HEALTH final **err=0** → TA still alive

## Results

| Item | Result |
|------|--------|
| PRE_ENROLL | err=0, challenge returned |
| CAPTURE_IMAGE (zero payload) | SCM **OK**, app err **`0xffffff37`** |
| TA log | `error at ff_trustlet_capture_image[trustlet_core.c:1195]: 'Null pointer'.` |
| TA log (extra) | **`The response data should be placed in shared memory or another piece of malloc memory.`** |
| Image region @ ion+0x1000 | unchanged (`0xa5` pattern) — no pixel write |
| HEALTH after | **er=0** (alive) |

Evidence: `lab/fingerprint/qsee-capture-path.txt`, `qsee-capture-trials.txt`

## Interpretation

1. CAPTURE is a real device-TA command (not SCM reject).
2. Soft failure is a **missing response/image buffer pointer** inside the 0x24-byte request (or trustlet context not bound to shared ion the way Android’s `g_builtin_buffer->usr` is).
3. HAL `ff_trustlet_capture_image(buf)` requires `buf == client shared buffer`; Android’s ion mapping is set up by `QSEECom_start_app` + listener path.
4. Our `qcom_tzmem` ion is shared for APP_SEND but may not populate the same internal “builtin buffer” object the TA uses for capture_ctx.

## Payload trials (so far)

All return Null pointer / no image write:

- zeros (0x24)
- off/len/flag u32s
- REE VA / PA of ion+0x1000
- segmented 0x1000 wrapper
- plen 0

## Next

1. Reverse full `capture_ctx` (0x24) and how Android binds `g_builtin_buffer` at start_app.
2. Implement ion “builtin buffer” registration equivalent (region notify / start_app path Android uses).
3. Parallel: QSEECom **BLOCKED_ON_LISTENER** handling for ENROLL template SVC (`M4l`).
4. Keep `skip_enroll=1` until ENROLL is safe.

## Module

- `lab/fingerprint/oot-qsee/qsee_fingerpr.c` — `skip_enroll` param (default true)
- `artifacts/qsee-modules/qsee_fingerpr.ko`
