# M4bl — Let AutoSDacUpdate finish (2026-07-27)

## Measurement (M4bk)

```
usMiddleTemp: 863 / 868   # matches Android 864–870
img calibration->try: 1, 2
# never: newDAC / get image dac [success] / Base update
```

WMODE-5 starts DAC search; immediate CAPTURE aborts before success.

## Fix

After WMODE-5: **poll LOGCAT ~1.8s** for `get image dac` / Base update **before** CAP.
Then light SCAN+CAP for Step3 only if still needed.
