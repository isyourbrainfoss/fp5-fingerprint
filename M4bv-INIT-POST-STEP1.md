# M4bv — INIT_DEVICE after Step1 try2 (2026-07-27)

## Solid diagnosis (M4br2 TRY2 residual)

TA response log **len=1322** ends mid try2:
```
******Step1...
try: 1, usMiddleTemp: 863
try: 2, AVG DAC_Temp:12   # TRUNCATED
```
No room for newDAC/find-DAC-fail. Preceded by: unknown device mode,
efc 144, full FDT try1-4. AutoSDac code may continue after log fill.

## Change

After Step1 detected: re-INIT_DEVICE while SPI/FDT/efc warm + LOGCAT drain
+ ion scan for Base update / newDAC. Keep SCAN-before-WMODE, mode 3.
