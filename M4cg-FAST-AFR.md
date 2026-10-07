# M4cg — FAST AFR under REAL DOWN (0x240×3 only)

M4cf: REAL DOWN itype=0x2 then multi-flag CAP (~6s) stayed FDT-only.

## Fix
- `try_android_frame_raw`: only flag **0x240** × `sample_triple_cap` (3), 12ms gaps
- After CAP×3: **REPORT_EVENT 0x2** once
- No force-image thrash under real finger; skip secondary enroll CAP loop

Android: 3 frames in ~50ms then enrolling.
