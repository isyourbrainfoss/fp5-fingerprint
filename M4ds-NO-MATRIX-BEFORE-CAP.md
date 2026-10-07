# M4ds — no CAP matrix before native 112×88 (2026-09-05)

## Why
M4dr live: cue + real DOWN + one 112×88 CAP → **28ms**, `rows=88`,
`ff_sensor_data_array_new` **OOM** / Null pointer. Native size *is* the
image path (not 11s FDT). TA still `array_new` instead of using ion.

Default `early_cap_stages=1` runs **m4dk_matrix at post-entry and post-Base**
on the enroll path (8 diagnostic CAPs). Those OOMs empty TEE heap before
the finger CAP.

## Change
`early_cap_stages=0` `heap_probe=0` `idle_layout1_cap=0` on Register.
Keep M4dr: one 112×88 nopa CAPTURE on DOWN.

## One press
Hands off until **one flash**, hold ~2s.
| Result | Meaning |
|--------|---------|
| `FRAME RAW M4dr` | heap was the matrix |
| OOM ~28ms again | INIT/Base already ate the heap — stop Register loops |

## Hold result (2026-09-05 `fp-enroll-m4ds` / `20260905T214728Z`)
Cue + REAL DOWN PASS. Matrix gone (no m4dk). INIT_DEV Step1–3 still ran on
reattach (`START focal32 -22`). CAP **ms=28 rows=88** `array_new` OOM.
Empty fid **1750352060**, AUTH `broken off`. Same as M4dr — matrix was not
the leftover heap. Next: **M4du** (SHUTDOWN then skip INIT), not more AFR.
