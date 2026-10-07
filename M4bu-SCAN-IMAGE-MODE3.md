# M4bu — work mode 3 = SCAN_IMAGE (2026-07-27)

## Diagnosis

WMODE-5 residual: `unknown device mode` then FDT+Step1 try1–2
truncated (log 1322B). Mode 5 may be SENSOR not image calib.

TA `FF_DEVICE_MODE_*`: WAIT_LEAVE, WAIT_TOUCH, IDLE, … **SCAN_IMAGE**.

## Fix

`FF_WMODE_IMG_CALIB = 3` (was 5). Keep SCAN-before-mode, TRY2 dump,
Step3 CAP push. native_log off, spi_data off.
