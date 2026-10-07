# M4cb — CAP only after REAL finger DOWN

**Date:** 2026-07-28  
**Why not another identical hold:** force CAP / fake `ta_down` on ESD IRQ ran FDT-only
image path → rem_seen=0 every time.

## Change
- Default `irq_means_down=0`: L2 edge alone does **not** set ta_down.
- After IRQ: poll QES up to `touch_wait_ms` (default 500ms) for `[finger touch]` or itype `0x2`.
- Light QES poll every ~400ms during wait (no esd_clear thrash).
- `REAL DOWN` → AFR CAP; otherwise idle force-image only if `enroll_force_cap`.
- Pre-AFR QUERY + 16ms settle (Android timing).
- `l2_listen_on` each sample.

## Hold params (go script)
`sample_wait_s=15`, `irq_means_down=0`, `touch_wait_ms=500`, `enroll_force_cap=1` (fallback only).

## Success markers
`REAL DOWN`, `finger CAP MINIMAL REAL`, `frame raw` / `AFR rem markers` / `rem_seen=1`.
