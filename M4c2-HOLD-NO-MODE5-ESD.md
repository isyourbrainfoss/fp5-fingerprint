# M4c2 — enroll without mode-5 ESD; hold results (2026-07-27)

## Code
- Pre-ENROLL WMODE: **FDT_DOWN** (not mode 5)
- Post-ENROLL: FDT_DOWN + SCAN only (no mode-5 warmup)
- Edge IRQ: ESD clear before assume if no finger_touch

## Hold result (M4c2 + L2 v0.5 + Base)
| Item | Result |
|------|--------|
| Base path | green (from M4c1) |
| CAP under finger | **err=0** (was 0xffffff37) |
| vio | success |
| QES | often `unknown` / sticky **esd=1** after edge |
| finger_touch QES | **never** |
| frame raw | **0** |
| pix | ~31 (idle noise, not ~3k finger frame) |
| rem_seen | **0** |
| AUTH | broken off |

## Diagnosis
1. CAP soft-succeeds with FDT residual only — **img_scan not running**.
2. Without QES `[finger touch]`, TA stays FDT soft path (M4be).
3. Edge ESD clear does not clear sticky esd (qes_ok=0 after 3 tries).
4. User may not have pressed during auto-started holds — coordinate next go.

## Next pure SW
1. Stronger ESD clear / QES idle before sample CAP.
2. Parse residual for FDT-only early-return on enroll CAP.
3. Coordinated hold with real finger_touch QES before CAP.
