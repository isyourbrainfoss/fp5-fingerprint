# M4v — Android-style one-ENROLL + CAPTURE samples (2026-07-27)

## Change

Previous path called `ENROLL 0x2001` **per sample** → TA logged a **new fid every
time** and never `enrolling... rem = N`. That cannot finish a template.

Android logcat oracle (`dumps/android/fp-enroll-first-*`):

1. `fingerprint_pre_enroll` → challenge  
2. `fingerprint_enroll` **once** → `new fid` once → `FDT_DOWN_DETECT`  
3. Loop: IRQ type `0x2` → `frame raw` / `transmit_image ~37ms` → `enrolling... rem--`  
4. Finger up → next down until rem complete  
5. `post_enroll` / AUTH match  

pmOS module now:

- `PRE_ENROLL` → **one** `ENROLL-start` (segmented HAT plen `0x4a`)  
- Then **CAPTURE-only** loop (`enroll_samples`, default 8)  
- Marks `SAMPLE_MARK` / `rem_seen` if TA logs `rem =` / `enrolling` / `frame raw`  
- `POST_ENROLL` + `AUTH`  

## Evidence (no held finger)

From `{SCRATCH}/fp-enroll-short.log` / `qsee-enroll-once-protocol.txt`:

| Marker | Result |
|--------|--------|
| `ENROLL start er=0` | pass |
| `new fid (= 1377415170)` | **once** |
| `cap-enroll-N er=0` (N=0..2) | pass |
| `ta_down=0` | no IRQ type 0x2 / finger touch |
| `rem_seen=0` | no `frame raw` / rem |
| AUTH | `enrollment is unexpected broken off` |
| `HEALTH final er=0` | TA alive |

During CAP: soft `set chip vio 1.8v fail` + `configure spi mode failed(0)` then FDT
retry — no img path / no `frame raw`.

## Module params

```
skip_enroll=0 enroll_needs_finger=1 wait_finger_s=40 enroll_samples=8
spi_bus_num=14 do_start=0 force_f32_id=4 force_fp_id=5
enable_fs_listener=1 enable_rpmb_listener=1
```

## Still blocked (criterion 5)

User must **hold finger** for the wait window so TA reports interrupt type `0x2`
and (with stable SPI) `frame raw` → `enrolling... rem`. Agent cannot press the
sensor.

Also open: full img calib / “successfully initialized” never seen on pmOS;
REE `geni_spi` pinmux still reserved (M4h).

## Artifacts

- Source: `lab/fingerprint/oot-qsee/qsee_fingerpr.c`  
- KO: `artifacts/qsee-modules/qsee_fingerpr.ko`  
- Script: `lab/fingerprint/run-enroll-hold-finger.sh`  
- Log: `lab/fingerprint/qsee-enroll-once-protocol.txt`  

## Update: skip empty CAP + REPORT_EVENT

Without IRQ/TA DOWN, samples log `no IRQ/DOWN — skip CAP` (avoids empty enroll frames).
After CAP, send REPORT_EVENT 0x1017 plen 0x2e0 (Android constant pool).
