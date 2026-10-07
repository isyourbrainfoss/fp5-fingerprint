# M4bq — Step1 reached; chase newDAC (2026-07-27)

## M4bp result

- Full `get fdt dac [success]`
- **`******Step1: clibration-->tx close(nav)...`** first time on pmOS
- AutoSDac try 1–2 middle 863–868, **no newDAC**
- SFS serial still GENERIC (no RPMB block)

## M4bq

1. Sticky base/dac/step flags (were cleared by SCAN/LOGCAT after Step1)
2. Log abnormal/fail/skip-step2/ESD DAC fails
3. ESD_HANDLE + VDD cycle before Step1
4. Prefer SPI 4M; hold WMODE-5 without mid-SLEEP restart
