# M4bi — pmOS data_home + FDT-then-Base calib (2026-07-27)

## Findings (M4bh smoke)
- Force WMODE-5 drives `img calibration->try` 1/2 but **never** `get image dac [success]` / Step1 / Base update.
- INIT_DEV log truncated (~810 B); serial path was Android `/data/vendor_de/0/fpdata/...` (missing).
- Sticky goal: allocate TA nav base/raw (`ft93xx_init_memory` / Base update).

## Changes
1. Config `default_data_home` / `persist_data_home` → **`/var/lib/fp5-fp`** (writable, FILE_SERVICE).
2. INIT_DEV: **4× LOGCAT drain** for Base update markers.
3. Force Base calib also **after skip_capture FDT** (`g_img_base_ok` sticky).
4. Extra fail markers: skip step2/3, fail to get img dac.

## Smoke
```
dmesg | grep -E 'IMG BASE CALIB OK|Base update|Step1|get image dac|IMG-CALIB-FAIL|data_home'
```
