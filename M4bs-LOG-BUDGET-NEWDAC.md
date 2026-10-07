# M4bs — TA log budget so newDAC fits (2026-07-27)

## Diagnosis (M4br2 TRY2 residual)

WMODE-5 residual `len=1322` ends mid try2:

```
img calibration->try: 2, AVG DAC_Temp:12   # truncated
```

Buffer already holds: unknown mode, efc 144, FDT try1–4, fdt dac success,
raw diff, base check, Step1, try1, try2 start. **No room for newDAC line.**

So AutoSDac may succeed (or fail) after try2 but the TA stops appending log
when its ~1.3KB response log is full. Android logcat is not size-capped the
same way.

## Fix

Nested config diagnosis:
- framework_log_level 2 (was 3)
- firmware_log_level 1 (was 3)
- enable_trustlet_native_log false
- enable_logcat_spi_data false

Plus M4br order: SCAN before WMODE-5.

## Success criteria

`newDAC` / `get image dac [success]` / `Base update success` in dmesg.
