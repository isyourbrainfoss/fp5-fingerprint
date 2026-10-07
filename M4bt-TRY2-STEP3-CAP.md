# M4bt — after Step1 try2, Step3 CAP push (2026-07-27)

## Diagnosis (solid)

TRY2 residual len=1322 ends mid-line:
`img calibration->try: 2, AVG DAC_Temp:12` truncated.
Buffer full of FDT+efc before newDAC can log. AutoSDac may still run.

## Fix

1. Log: fw level 2, no native/spi dumps (free budget, keep markers)
2. SCAN before WMODE-5 (M4br)
3. After Step1 window: SPI 8M + triple CAP flag 0x240 (Step3-like)
4. Full TRY2 residual dump when try:2 seen
