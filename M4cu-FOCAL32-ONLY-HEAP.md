# M4cu — focal32-only TZ heap + PA LATCH CAP

## Why (M4ct)
LATCH + sticky QES **PASS**. CAP residual pure:
```
ff_sensor_data_array_new[image.c:105]: Out of memory
```
base=1, wall ~0.4s, rem=0. M4cm: even **32×32** OOM → TA heap nearly empty
(not Step1 thrash).

Android enroll (same TA binary): **frame raw×3**, **0** array_new OOM.
Android load: long-lived `sb_length=0x80040` on **focal32** only (~3 MB image).
pmOS held **fingerpr** too (~10 MB) forever (`rmmod` never SHUTDOWN).

## Change
1. `app_shutdown` (APP_MGR cmd 2) — reclaim loaded TA
2. `start_fingerpr=0` default: START **focal32 only**; if `force_fp_id` set → SHUTDOWN it
3. Enroll+skip_capture: one SET_GROUP (`/var/lib/fp5-fp`), skip ENUM/AUTH_ID
4. AFR LATCH: **PA@+0x14** (ion plane) ×3; on first OOM one **nopa** retry then stop

## Hold expect
- `M4cu skip START fingerpr` and/or `SHUTDOWN id=… M4cu-no-fingerpr`
- `android-frame-raw M4cu` + `afr LATCH` + `pa=1`
- Prefer: `*** ANDROID FRAME RAW ***` / rem; else still OOM → next is deeper free/re-Base

## Hold result (2026-07-28, CLICK HERE live)
| Item | Result |
|------|--------|
| focal32-only | PASS (`skip START fingerpr`; SHUTDOWN id=5 ret=-22 already gone) |
| Base | PASS base=1 |
| REAL DOWN | PASS both samples (s0 esdclr, s1 sticky touch) |
| LATCH | PASS |
| CAP pa=1 | **FAIL** OOM ms=4 |
| CAP nopa retry | **FAIL** OOM; residual **FDT-BASE → Step1 try → array_new** |
| frame raw / rem | 0 |

**Verdict:** fingerpr unload + PA does **not** fix CAP. Under-finger CAP re-enters **img Step1** (Android does not). Next: block Step1 re-entry when Base warm (SYNC/calib sticky / CAP path that reuses Base buffers).
