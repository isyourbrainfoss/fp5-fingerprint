# M4bo — image_acquisition_mode string (2026-07-27)

## Bug

Config had `"image_acquisition_mode":0` (JSON integer).  
Android: `device.image_acquisition_mode : polling-driven` (**string**).

TA log: `expect 'JSON_STRING' but got 'JSON_INTEGER'` — wrong type may break
img acquisition / AutoSDacUpdate completion (try 1–2 with good middle, no newDAC).

Also `enrolling_overlap_intervals` as `"0x54A0"` string like Android.

## Fix

Nested JSON: `"image_acquisition_mode":"polling-driven"`.
