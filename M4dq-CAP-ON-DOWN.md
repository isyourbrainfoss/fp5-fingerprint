# M4dq — CAPTURE on the QEV DOWN (2026-09-05)

## Why
M4dp residual at CAP: `cidri f: 3 (N)` while HLOS had `itype=0x2`.
`send_ion` **zeroes** `last_send_itype`, so **QFINGER-s** after QEV-s dropped
the DOWN bit and extra **SET_SPI_SPEED** ran before CAPTURE. TA AFE had no
finger → 11s FDT calib instead of `img_scan`.

## Change
1. If QEV-s is itype 0x2: **skip QFINGER**, CAPTURE next.
2. AFR: **no SET_SPI_SPEED**, no REPORT, no re-WMODE. 0x240×3 immediately.

## One press
Close **FP Fingerprint** → Start → Register → PRESS/LIFT **once**.
Want `REAL DOWN QEV-s … skip QFINGER`, `M4dq CAP ms=` tens, `f:2 (D)`,
or honest `M4dq FDT-slow` with `f:` still N.
