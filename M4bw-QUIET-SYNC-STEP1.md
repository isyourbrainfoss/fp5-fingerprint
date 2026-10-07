# M4bw — quiet SYNC_CFG before Step1 (log budget) (2026-07-27)

## Problem

WMODE residual len=1322 ends mid try2; FDT try1–4 + efc fill the buffer
before `newDAC` can be written.

## Fix

After FDT phase succeeds, re-`SYNC_CFG` with:
- `firmware_log_level=1`
- `framework_log_level=2`
- native/spi dumps off

Then SCAN + WMODE-5 so residual is mostly Step1 (room for newDAC / fail reason).

Also: sticky `last_send_fdt_ok` from get fdt dac / fdt base check.
