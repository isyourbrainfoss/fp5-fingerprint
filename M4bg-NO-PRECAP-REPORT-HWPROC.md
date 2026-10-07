# M4bg — no pre-CAP REPORT; hw process config; soft ESD (2026-07-27)

## RE

Android enroll order:
```
QES [finger touch] → frame raw[0..2] (~50ms) → TA "got finger touched" → enrolling
```
Our path injected **REPORT_EVENT 0x2 before CAP**, which can skip real img_scan.
TA also early-returns img_scan on ESD (`power on or ESD revert initial RETURN`).
esd_clear used **WMODE-5** which re-sticks ESD (M4bc hold).

## Changes
1. `try_android_frame_raw` / force-image under finger: **no REPORT before CAP**
2. Nested config: `enable_prev_hw_process` / `enable_post_hw_process` true
3. `esd_clear_loop`: FDT_DOWN only, **no mode-5**

Shared buffer size already REQ_ION=0x78000 (matches Android) — not the gap.
