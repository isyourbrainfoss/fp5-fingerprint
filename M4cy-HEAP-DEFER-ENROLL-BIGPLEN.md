# M4cy — heap: defer ENROLL, no SYNC under finger, ion big-plen

## Why (M4cx)
Clean path: REAL DOWN + LATCH + SPI8M + no Step1 → still
`array_new` OOM in ~4ms. Base warm; TA alive.

Hypothesis: ENROLL_HL allocates enroll workspace that leaves TEE heap
too tight for frame `array_new`. Android CAP may reuse Base buffers;
meta plen=0x24 may force TEE malloc for the plane.

## Change
1. **`defer_enroll=1` (default)**: FDT arm + sample CAP **before** ENROLL_HL; ENROLL-late after samples
2. AFR: **no SYNC_CALIB** under finger (Android transmit_image does not)
3. AFR: **SPI 8M only**
4. One **big-plen** CAP (`0x24+112×88×2`) with PA into ion, then meta BURST fallback
5. Keep M4cw LATCH / M4cv no-L2 / M4cx no re-SCAN

## Hold expect
- `M4cy defer ENROLL — CAP samples first`
- `android-frame-raw M4cy` + `M4cy AFR SPI8M no-SYNC`
- `afr BIG plen=...` then BURST or `*** FRAME RAW BIG ***`
- If BIG/meta works without ENROLL first → ENROLL was heap eater

## Hold result (2026-07-28)
| Item | Result |
|------|--------|
| defer ENROLL | **PASS** (ENROLL-late after samples) |
| LATCH + SPI8M no-SYNC | PASS |
| REAL DOWN both | PASS |
| BIG plen ion CAP | **FAIL** array_new OOM (same as meta) |
| meta BURST | **FAIL** OOM |
| rem_seen | 0 |

**Verdict:** OOM is **not** from ENROLL heap and **not** from meta-only plen.
`array_new` still runs (and fails) even with full ion plane. Base buffers
likely not reusable; TEE still needs a working alloc that fails on this path.
