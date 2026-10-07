# M4az — wait_chip_ready before image CAP (2026-07-27)

## Finding

Image SPI `configure spi mode failed` coincides with chip interrupt bits:

| itype | meaning |
|-------|---------|
| 0x200 | **rst** |
| 0x400 | **esd** |
| 0x10 | **inv** (fake interrupt) |

Even after `chip ready itype=0x0`, first force CAP can still log spi mode fail
(host omit interrupt / GENI timing). Without finger, CAP remains **FDT-only**
(a5 head). REE pinmux still hard hang; gpio56–59 absent from gpio debugfs when
reserved.

## Software

1. Track `last_send_itype` from TA log  
2. `wait_chip_ready()` — QEV until idle/finger, not inv/rst/esd  
3. Call before force-image and pre-sample warm CAP  
4. Soft `spi_image_recover` only (no CAP INIT_SPI)  
5. Reject itype 0x10 inv as not ready  

## Smoke

Clean boot: SN, `chip ready itype=0x0`, `fdt_ok=1`, HEALTH 0, IMAGE PIXELS.  
Still spi mode fail on first force CAP; no frame raw without finger.

## Hold

`/tmp/do-enroll-m4al.sh` after user **go** — need finger for rem.
