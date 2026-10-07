# M4dr — one native 112×88 CAPTURE (2026-09-05)

## Why
M4dq live: skip QFINGER, immediate 16×16 `0x240` after real DOWN → still
**11s FDT**, `cidri f:N`, no `img_scan`. 16×16 AFR is exhausted.

Android: same itype 0x2 → `frame raw[0..2]` in ~37ms at **112×88**.

## Change
On latched DOWN: **one** `CAPTURE_IMAGE` plen `0x24`, **112×88**,
`nbytes=0x4d00`, layout1, **no PA@+0x14**. No flag sweep, no 16×16.

## One press
Hands off until **one flash**, then hold ~2s.
Read dmesg `afr M4dr CAP ... ms=`:
| ms | Meaning |
|----|---------|
| tens + `FRAME RAW M4dr` | native bind works |
| ~4 + OOM/`0xffffff37` | TA still `array_new` |
| ~11000 + `f:N` | ctx still not an image request |

Do not repeat Register if it is OOM or FDT-slow.
