# Blocker — AUTH match needs physical finger (criterion 5)

## Plan verification status

| Plan criterion | Status |
|----------------|--------|
| 1. Device init + SPI SN | **PASS** |
| 2. CAPTURE non-null / FDT | **PASS** (+ img calibration on pmOS) |
| 3. Enroll+AUTH match or unlock | **N/A — criterion 5** (this file is the gating artifact) |
| 4. Dual-boot safety | **PASS** |
| 5. Impossible without user → stop with blocker | **THIS FILE** |

Updated: 2026-07-27T06:04:19+02:00

## Last green rung (autonomous, live `fp5-pmos`)

| Rung | Evidence |
|------|----------|
| SPI SN | `sn num: 0b0a6944171209070800fb120d1e1dff22` |
| CAPTURE / FDT | soft-success (no Null), `fdt_ok=1` |
| Img calibration | `img calibration->try: 1/2` + observed marker |
| ENROLL once | `ENROLL start er=0`, single `new fid` |
| ESD recovery | `l2_reset_ic ret=0`, warm-up `vio_ok=1` |
| Enroll CAP samples | 8× `vio 1.8v success` (`enroll_force_cap`) |
| AUTH | soft `er=0`, **`broken off`** (no finger frames) |
| HEALTH | `er=0` |
| Dual-boot | no `qbootctl` in scripts/module |

**Logs (SCRATCH):** `fp-enroll-or-unlock.log`, `fp-init.log`, `fp-capture.log`,  
`fp-hold-c5-ship.out`, `PLAN-VERIFICATION-OBSERVATIONS.txt`, `VERIFICATION.md`.

**Protocol notes:** `M4v-ENROLL-ONCE-PROTOCOL.md`, `M4w-ENROLL-ESD-IMG-CALIB.md`.

## Why the agent cannot finish match

Android enroll needs **finger down** → **`frame raw`** → **`enrolling... rem = N`**.  
The implementer has **no physical access** to the power-button fingerprint sensor.

Also ruled out:
- Empty REE templates (`/var/lib/fp5-fp/templates`, FBE Android userdata mounts empty)
- Synthetic IRQ noise (+1 residual ≠ real press; TA interrupt stays idle/0x1)
- Software-only AUTH without ridge frames

Without a press: `frame_raw=0`, `rem_seen=0`, AUTH stays `broken off`, Phosh unlock cannot be shown.

This is plan **acceptance criterion 5** (honest complete for this goal branch).  
Verification plan step 3 is **N/A**; **this blocker is the gating artifact**.

## Exact missing user action

```sh
scp artifacts/qsee-modules/qsee_fingerpr.ko lab/fingerprint/run-enroll-hold-finger.sh fp5-pmos:/tmp/
ssh fp5-pmos 'sudo sh /tmp/run-enroll-hold-finger.sh'
# On each dmesg line:
#   *** PRESS/LIFT finger sample N ***
# hold then briefly lift the power-button fingerprint sensor (~5s × 8, ~40s).
dmesg | grep -E 'frame raw|enrolling\.\.|rem = |rem_seen=1|AUTH|broken'
```

**Success:** `frame raw` / `enrolling... rem = N` / `rem_seen=1` / AUTH without `broken off`.

## Module

- `artifacts/qsee-modules/qsee_fingerpr.ko` vermagic `7.1.2 SMP preempt mod_unload aarch64`
- Hold helper: `lab/fingerprint/run-enroll-hold-finger.sh`
