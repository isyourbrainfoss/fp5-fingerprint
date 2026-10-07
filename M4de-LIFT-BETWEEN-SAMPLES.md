# M4de — require LIFT between enroll samples

## Why (M4dd hold UX)
Sample 0 REAL DOWN (itype=0x2) → CAP ~0.4s → sample 1 PRESS → **~0.8s later**
REAL DOWN with **itype=0x0 touch=1** (sticky touch / bounce). User never got a
real second press window.

## Change
1. Before sample ≥1: poll QES until **lift clear** (~320ms stable no-touch /
   no itype DOWN), up to `sample_wait_s`
2. REAL DOWN requires **`itype & 0x2`** — sticky `touch=1` alone is ignored
3. Log: `LIFT finger — wait clear`, `lift clear (stable)`, `touch sticky no itype`

## Hold expect
- Sample 0 PRESS → press → CAP
- `LIFT finger — wait clear before sample 1` (banner waits)
- Only after lift: SAMPLE 2 PRESS → intentional re-press
- No auto-fire sample 1 with itype=0x0

## Hold result (2026-07-28 re-go)
| Item | Result |
|------|--------|
| Sample 0 REAL DOWN | **PASS** itype=0x2 |
| LIFT wait | **PASS** `lift clear (stable)` ~0.9s |
| Sample 1 sticky touch | **ignored** (`touch sticky no itype`) |
| Sample 1 REAL DOWN | **PASS** itype=0x2 (~2.7s after PRESS — real re-press) |
| CAP both | **FAIL** OOM `array_new` ms=4 |
| rem_seen | 0 |

UX fix works. Image path still Base-heap OOM (M4dd).
