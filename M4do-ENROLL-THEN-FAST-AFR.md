# M4do — ENROLL first, then fast 0x240×3 (2026-09-05)

## Why
Live Register (`dumps/pmos/fp-enroll-20260905T202648Z/`): real `FINGER_DOWN`,
then **M4dn** 8× ~11s CAP (`flag` 0x240/0x200/0x40/0x41, `rows=0`, FDT dac).
`defer_enroll=1` then **ENROLL-late** minted empty fid `1348893330`.

Android enroll oracle: ENROLL, then `REPORT 0x2` + CAPTURE×3 flag `0x240` in
~50ms → `frame raw`. 16×16 L1 stays (112×88 / climb was Step1 OOM).

## Change
1. `defer_enroll=0` on the Register helper (Android order).
2. AFR: REPORT 0x2, then **0x240×3 only**. Stop if CAP `ms>800` (FDT-slow).
   No 0x41 sweep, no DEAD_PIXEL 11s.

## One press
**FP Fingerprint** → Close if still open → Start → Register → PRESS/LIFT once.

Want in dmesg: `M4do CAP ... ms=` tens not ~11000, and `ANDROID FRAME RAW M4do`
or honest `M4do FDT-slow — stop`. Do not repeat if FDT-slow.
