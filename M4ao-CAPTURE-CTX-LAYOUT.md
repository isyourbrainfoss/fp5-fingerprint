# M4ao — capture_ctx layout: rows zeroed vs preserved (2026-07-27)

## Live measurement after CAPTURE 0x1013

| Layout | +0x0c | +0x10 | After CAP |
|--------|-------|-------|-----------|
| **0** (old default) | img_off | nbytes | **rows=0**, cols=112, head a5 |
| **1** (new default) | nbytes | img_off | **rows=88**, cols=112, fields stable |

Layout 0: TA zeros `rows` → image pipeline cannot use frame size.  
Layout 1: TA keeps `rows=88` / `cols=112`.

## Decision (M4aq)

| Layout | Soft CAP | rows after CAP | Null risk | Default |
|--------|----------|----------------|-----------|---------|
| 0 | yes | **0** (restore with fix_rows) | low | **yes** |
| 1 | can Null on clean boot force CAP | 88 | high after plen-big | experiment only |

Large plen CAPTURE (0x24+img) → sticky Null on later APP_SEND — **never** use in force path.

## Still open

- HLOS `img_off` often still a5; IMAGE PIXELS from force scan / SRAM (nz 24–2181).  
- `frame raw` TA log / enroll rem under finger.  
- REE pinmux hang (M4al) — TA-only.

## Module / hold

`cap_ctx_layout=0` default. Hold: `/tmp/do-enroll-m4al.sh`.

## Evidence

`fp-m4ao-layout-smoke.log`, `fp-m4aq-smoke.log`, `fp-m4ap-smoke.log` (big-plen Null).
