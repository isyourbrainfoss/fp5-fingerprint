# M4co — settle clean touch, single L1 CAP

## Why (M4cn residual)
Under-finger image CAP residual:
```
query event state: [esd]
******Step1: img calibration try 1..3
ff_sensor_data_array_new … Out of memory
```
Sample often **REAL DOWN during esdclr** with **esd=1**. Dim/PA ladder exhausted.

## Change
1. **`afr_settle_clean_touch`**: QES + soft ESD_HANDLE until `touch && !esd` (~180ms)
2. **No CANCEL**, no mid ladder, no no-PA
3. **One layout1 112×88** flag `0x240` only when clean
4. One L0 fallback; REPORT; residual on success or first OOM

## Hold expect
- `android-frame-raw M4co`
- `afr settle CLEAN` or `skip L1 — not clean`
- `afr L1 clean CAP` — look for frame raw vs OOM **without** QES esd in residual
