# M4ay — soft SPI recover (no CAP-time INIT_SPI) (2026-07-27)

## Regression found

CAP-time `re-SYNC + INIT_SPI + PROBE` mode cycle (M4ax) produced sticky
**`Null pointer`** on all later APP_SEND (TA death). Do **not** re-INIT_SPI
from CAP recover.

## Live QES during image SPI fail

```text
interrupt type: 0x200  (= rst bit)
query event state: [esd]
configure spi mode failed(0).
```

After soft ESD clear: `itype 0x0` (idle). Chip was in reset/ESD while image
SPI mode config ran.

## Fix

`spi_image_recover`: L2 reset + SET_SPI_SPEED + optional VDD + `esd_clear_loop`
only. No SYNC/INIT_SPI on CAP path. PROBE-time mode cycle kept (init only).

## Clean-boot smoke

- SN, `fdt_ok=1`, CAPTURE soft-success, HEALTH 0  
- `NULL=1` (stale INIT_DEV log only, not sticky post-CAP death)  
- Still `configure spi mode failed` on first force CAP; later vio/FDT path  
- No `frame raw` without enroll+finger  

## Hold

`/tmp/do-enroll-m4al.sh` after user **go**.
