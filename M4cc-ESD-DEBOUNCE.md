# M4cc — ESD IRQ storm debounce

**Date:** 2026-07-28  
**Problem:** M4cb hold saw continuous L2 IRQs with TA `itype=0x0 esd=1 touch=0`.
Each edge ran 500ms of QUERY → 8s window never got REAL DOWN.

## Fix
- `irq_debounce_ms=250` (default): process at most one edge per 250ms; absorb storm.
- Sticky ESD → **one** `esd_clear` + FDT re-arm + `l2_listen_on` mid-wait.
- `esd_touch_wait_ms=60`: short QES poll when esd (vs full 500ms).
- Pre-sample: always one ESD clear + FDT arm (even if `sample_esd_clear=0`).
- No late-window fake DOWN on ESD.

## Hold still: 2×8s, REAL DOWN for AFR CAP
