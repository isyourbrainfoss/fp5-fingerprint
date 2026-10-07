# M4cv — preserve Base: no L2 reset after warm Base

## Why (M4cu)
LATCH + REAL DOWN + focal32-only PASS. CAP residual:
```
FDT-BASE success → ******Step1: img calibration try → array_new OOM
```
Android enroll under finger: **no Step1**, frame raw×3 only.

Between INIT Base and CAP we were wiping chip calib:
- `l2_reset_ic(pre-ENROLL)` / `post-ENROLL`
- `esd_clear` try≥1 → L2 reset + VDD + soft CAP
- mid-sample sticky ESD → L2 reset

Android **never** resets IC between Base and transmit_image.

## Change
1. `preserve_img_base=1` (default): when `g_img_base_ok`, skip L2 reset pre/post ENROLL
2. `esd_clear_loop` soft-only when Base warm (ESD_HANDLE + FDT re-arm only)
3. Mid-sample ESD: soft path (same)
4. AFR LATCH: **SYNC_CALIB + SPI 8M** then CAP (no WMODE/SCAN)

## Hold expect
- `M4cv skip L2 reset pre-ENROLL` / `post-ENROLL`
- `M4cv esd_clear soft-only`
- `android-frame-raw M4cv` + `M4cv AFR SYNC_CALIB+SPI8M`
- Prefer residual **without** Step1 + frame raw; else still OOM with/without Step1

## Hold result (2026-07-28)
| Item | Result |
|------|--------|
| skip L2 pre/post ENROLL | **PASS** |
| esd soft-only | **PASS** |
| Base | PASS base=1 |
| REAL DOWN | PASS both (during esdclr) |
| LATCH / CAP | **FAIL** — `clean=0` skipped CAP |

### Regression
Post-IRQ always `esd=1`; soft esdclr leaves sticky ESD. AFR LATCH requires `!hold_esd`:
```
finger CAP FAST REAL touch=1 esd=1 itype=0x2
→ latched=false → settle → skip image (esd=1 touch=0)
```
M4cu had `esd=0` after L2 esdclr so LATCH fired (then OOM).

**Next M4cw:** LATCH on REAL DOWN even if sticky esd; soft ESD_HANDLE once under finger without dropping touch; then CAP.
