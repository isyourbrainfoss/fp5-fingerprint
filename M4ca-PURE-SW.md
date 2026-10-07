# M4ca — pure-SW image/rem advance (no user finger)

**Date:** 2026-07-28  
**Goal:** IRQ arm + clean load + image-path SW without finger presses.

## Shipped

| Item | Detail |
|------|--------|
| L2 IRQ | Rising-edge, rate-limited 1/s; listen 0→1 deltas proven |
| `read_file` | EINTR/partial + reopen on n=0 short read |
| `paint_cap_a5` | Module param; `0` for idle force-image without A5 mask |
| Residual | Logs finger f: when cidri-style text present |
| Load script | `fp-clean-load.sh` — `/tmp/qsee-ta`, START then force-ids |
| Artifacts | `artifacts/qsee-modules/qsee_fingerpr.ko` |

## Evidence locations

Prefer implementer scratch from the goal run:
- `fp-irq-arm.log`
- `fp-clean-load.log`
- `fp-img-bind.log`

## Results summary

1. **IRQ:** before 0 / after listen 11 / after2 22 (sysfs + `/proc/interrupts`).
2. **Load:** START focal32 id=4 fingerpr id=5; Base update; sample wait tw advances ~25/s.
3. **Image:** force-image idle `paint_cap_a5=0`; best_nz=33; IMAGE PIXELS min/max span 1–1792; frame raw still 0.

## Gap

rem / AUTH / frame raw under finger remain open for a future human hold; not blocked for pure SW criterion-5.
