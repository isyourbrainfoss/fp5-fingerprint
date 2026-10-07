# M4c5 — VIO latch + fast IRQ QUERY (2026-07-27)

## M4c4 hold result
- MINIMAL CAP under finger (no WMODE) — **worked** (esd=0 at CAP).
- Still `frame raw=0`, `rem_seen=0`, AUTH broken.
- AFR residual: **`set chip vio 1.8v fail`** on first CAP; later CAPs vio success but still FDT path.
- Never QES `[finger touch]` / itype `0x2` — only assume from IRQ edge.
- User may not have been mid-press during hold (samples 0–4 lost to dmesg ring).

## Code
1. Before sample loop: **mode 7 (vio_1p8) → FDT_DOWN → SCAN → soft CAP latch → re-arm FDT**.
2. IRQ poll **40ms** (was 150ms); log itype/touch immediately after QUERY.
3. Treat **itype 0x2** as ta_down (with finger_touch).

## Hold
Reboot (rmmod hangs). L2 v0.5 + M4c5 ko. On go: 6× press/lift power-button FP ~1s.
Look for: `pre-sample VIO latch vio=1`, `post-IRQ itype=0x2 touch=1`, `ANDROID FRAME RAW`, `rem_seen=1`.
