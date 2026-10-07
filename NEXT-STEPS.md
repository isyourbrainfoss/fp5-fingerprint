# Fingerprint — how to keep going

Live pin: **`STATUS.md`** (2026-09-15 extra SHA, persist unproven, Match itype wait).
This file below is historical 2026-09-06 and is **stale** on Register (it is not parked).

## Where we were (2026-09-06, stale)

Use **FP Fingerprint**. Matching kos on `7.2.0-nfc-test+`. Daily: **PIN on pmOS**,
fingerprint unlock on **Android slot A**. Register **does** insmod TZ (see STATUS.md).

| Rung | Status | Notes |
|------|--------|--------|
| L0 Android oracle | Done | dumps under `dumps/android/fp-*` |
| L1 GPIO poke | Done | IRQ34 / RST35 / VDD60 |
| **L2 kernel foothold** | **Done** | `focaltech_fp_life` on 7.2, vermagic match |
| L2.5 press_signal | **Done** | live `FINGER_DOWN` / `touch` |
| Coach app | **Shipped** | Start = L2 only. Register explains the wall. |
| **QSEE health** | **PASS** | `HEALTH er=0` |
| Enroll with finger | **Unblocked for img_scan** | M4dy idle: TA `frame raw[0]` avg 865, **no** `array_new`. Pixels stayed TZ-side. |
| AUTH / Phosh unlock | **Blocked on rem** | Need under-finger HAL CAP + rem; then AUTH |

See **`FINISH-LINE.md`**. Last informative press: `dumps/pmos/fp-enroll-m4ds/`.

**Safety (keep):** no auto enroll; Close → listen-off + `rmmod` life/QSEE; dual-boot Android remains unlock hatch.

## App status

- **Use:** drawer **FP Fingerprint**. **Start** loads L2. **Register** shows
  the TZ wall (no `qsee_fingerpr` insmod). Close unloads.
- Re-install: `./lab/fingerprint/app/install-on-fp5.sh`

## Productive next steps (no full unlock required)

### A — Solidify press / IRQ (high value, low brick risk)

1. Keep walkthrough coach-only; log `irq_count` / `press_signal` per step.  
2. Improve life module if free-run IRQ returns (listen gate must stay quiet=0).  
3. Optional: userspace “wake on press” demo (LED / feedbackd) without QSEE.

### B — L3 SPI / ID (research)

1. SPI electrical / FDT path from Android dumps (`M4*` notes in `lab/fingerprint/`).  
2. Read chip ID without enroll if protocol allows.  
3. Document only — still no fprintd.

### C — QSEE gap (HAL overlay works; ion copy-out still missing)

M4dy idle (no finger, `dumps/pmos/fp-idle-m4dy3/`): HAL 0x24 capture_ctx
(burst/flag, **no** cols/rows) → TA **`frame raw[0]` avgv=865** in 21ms,
`array_new=0`. Layout1 112×88 in the request was the malloc. Ion
`0x78000` has **no** ADC plane (TA frame stayed TZ-side).

Next that actually moves C: SSH enroll helper with this ko (`cap_hal_overlay`
default on, `skip_init_dev` off). One press can test rem. Do not Register
from GTK. Kernel `TZMEM_MODE_SHMBRIDGE` is only for HLOS copy-out, not for
img_scan. Do not OOT `shm_bridge_enable`.

Listeners/SFS are later (templates). Daily: **PIN on pmOS**, FP unlock on
Android slot A.

### D — Parked (do not Register)

Skip-INIT Register **FAILED** before flash (`ESD_HANDLE` SCM -22, dump
`dumps/pmos/fp-enroll-m4du-3/`). Native 112×88 under finger already OOM
(`fp-enroll-m4ds`). Remaining gap is Android `g_builtin_buffer` / QSEECom
session bind — offline RE, not PRESS/LIFT.

SSH enroll helper no longer passes `skip_init_dev` (that was a 2s FAIL
landmine). GTK does not call it.

## Suggested goal text (fingerprint-only, safe)

```
/goal FP5 fingerprint lab (safe): keep M0-press-ok; coach-only defaults; stop path unload;
optional SPI/chip-id probes; no auto enroll; no flash; dual-boot Android unlock hatch.
```

## Do not

- Auto TrustZone enroll in long unattended runs  
- Leave `listen=1` + life module loaded after sessions  
- Claim Phosh unlock before SFS/listener path exists  
