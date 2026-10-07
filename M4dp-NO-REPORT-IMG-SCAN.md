# M4dp — no REPORT before CAP (2026-09-05)

## Why
M4do live: ENROLL-first + real `FINGER_DOWN`, then **REPORT_EVENT 0x2** +
0x240 CAP → **11s FDT** (`got fdt dac`, `rows=0`). That is the M4c3
anti-pattern: a synthetic REPORT fakes DOWN and the TA takes
`normal fdt mode RETURN` instead of `ft93xx_img_scan` (~37ms on Android).

Android: IRQ itype 0x2 / QES `[finger touch]` → CAPTURE×3. No REPORT.

## Change
AFR under a latched real DOWN: SPI 8M, **no re-WMODE**, **no REPORT**,
0x240×3 16×16 L1. LOGCAT residual if FDT-slow. `defer_enroll=0` stays.

## One press
Close **FP Fingerprint** → Start → Register → PRESS/LIFT **once**.
Want `M4dp CAP ... ms=` tens + `ANDROID FRAME RAW M4dp`, or honest
`M4dp FDT-slow` + `FDT-ONLY early-return` in dmesg.
