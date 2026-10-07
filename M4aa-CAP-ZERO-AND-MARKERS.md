# M4aa — CAP variants + richer TA markers (2026-07-27)

## Changes

- Quiet MARK scan also flags: `sensor raw data`, `got image ready`, `Null pointer`, `image buffer`, `shared memory`.
- Enroll sample CAP: filled 112×88 `capture_ctx` (zero-ctx measured → Null), light `START_SCANNING` before CAP, no re-WMODE when `ta_down`, REPORT/SAVE + ion marker scan.
- Confirmed **not** to use: PA-in-ctx (SCM-kill), cols/rows=0 auto (vio/spi mode fail), pure zero ctx (Null).

## Smoke

SN + CAP soft-success + fdt + HEALTH ok; enroll rem still 0 without/with finger (see BLOCKER-IMAGE-REM.md).
