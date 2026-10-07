# M3 results — QSEE APP_START loads `fingerpr` on pmOS (2026-07-26)

## Headline

**TrustZone accepted a mainline-driven APP_START for the Android fingerprint TA `fingerpr`.**

This is the first time the FP5 lab has loaded a biometric QSEE app from postmarketOS.

## Evidence (live slot B, kernel 7.1.2)

```
qsee_ta_load: built fingerpr image total=10818258 mdt_len=7312
qsee_ta_load: private tzmem pool initial=12582912 max=16777216
qsee_ta_load: LOOKUP fingerpr pre-start: -2 (expected -ENOENT)
qsee_ta_load: APP_START fingerpr mdt=7312 img=10818258 pa=0xff200000
qsee_ta_load: APP_START scm ret=0 result=0x0 type=0xee01 data=0x4
qsee_ta_load: APP_START success, data/app_id=0x4
qsee_ta_load: POST LOOKUP fingerpr -> -2 (may still need listeners)
```

| Field | Meaning |
|-------|---------|
| scm ret=0 | Mainline `qcom_scm_qseecom_call` succeeded |
| result=0x0 | `QSEECOM_RESULT_SUCCESS` |
| type=0xee01 | `QSEECOM_SCM_RES_APP_ID` |
| data=0x4 | **Assigned app_id = 4** |

### Also `focal32` (same session)

```
qsee_ta_load: APP_START focal32 mdt=7108 img=3036093 pa=0xff200000
qsee_ta_load: APP_START scm ret=0 result=0x0 type=0xee01 data=0x5
qsee_ta_load: APP_START success, data/app_id=0x5
```

Both Android fingerprint TAs load from mainline. Name LOOKUP still sticky; use numeric app_id for SEND.

## How we got here

1. **M1:** Force `qcom_qseecom` pdev; LOOKUP works for `keymaster64` / `uefisecapp`.
2. **TAs:** Full `fingerpr`/`focal32` `.mdt`+`.b00–.b07` from **modem_a** (`/vendor/firmware_mnt` on Android) — no slot switch required.
3. **Loader:** OOT `qsee_ta_load.ko` builds contiguous image, TZ mem pool, SCM  
   `owner=QSEE_OS(50), svc=APP_MGR(1), cmd=APP_START(1)`  
   args: `mdt_len, img_len, phys`.
4. **kCFI:** kallsyms → static `qcom_scm_qseecom_call`; invoke via `no_sanitize("kcfi")` helper.

Artifacts:

- `artifacts/qsee-modules/qsee_ta_load.ko`, `qsee_probe_fp5.ko`
- `lab/fingerprint/oot-qsee/`
- `dumps/android/fp-tas-full-20260726/` (full TA blobs, hashes match oracle)

## Open issue (next, still solvable)

**POST LOOKUP by name `"fingerpr"` still returns -ENOENT** even though START returned app_id 4.

Likely causes (Android does these after START):

1. App needs **listener / incomplete-cmd** handling for full registration  
2. Embedded app name differs slightly (try other strings / list apps)  
3. SEND by **numeric app_id=4** works without name LOOKUP  

Next experiments: `APP_SEND` to id 4 with HAL-shaped buffers; load `focal32`; capture incomplete/listener path if result flips.

## Re-test

```sh
# on pmOS after modules + modem firmware copy
insmod qsee_probe_fp5.ko force_pdev=1
insmod qsee_ta_load.ko ta_dir=/lib/firmware/qsee ta_name=fingerpr
dmesg | grep qsee_ta_load
# expect: APP_START success, data/app_id=0x4
```

## Safety

- Read-only mount of `modem_a` for TA copy  
- No `qbootctl`, no custom full kernel flash  
- Soft reboots only when clearing DIE taint from early CFI experiments  

## Roadmap status

| Milestone | Status |
|-----------|--------|
| M0 L2 `/dev/focaltech_fp` | Done |
| M1 QSEE LOOKUP / uefisecapp | Done |
| M2 qseecom pdev | Done |
| **M3 TA load (APP_START)** | **Partial PASS — START ok, name LOOKUP sticky** |
| M4 Auth / match | Next |
| M5 Session unlock | Goal |
