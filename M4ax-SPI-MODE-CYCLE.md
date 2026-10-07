# M4ax — SPI mode cycle at CAP + probe (2026-07-27)

## Finding

`configure spi mode failed(0)` is **not** the same as `chip_id is shifted`
(SHIFT count was 0 on clean boot). Nearby TA strings: host omit interrupt /
abnormality interrupt flag; also chip_id-shifted retry path exists separately.

When CAP later reaches **spi_fail=0** + **vio success** + **got fdt dac success**,
ion image head is still **a5** (no img fill). CAPTURE is **FDT-only** without
finger — frame raw requires enroll+finger + working image SPI.

## Software

1. SE rate L2 (M4aw)  
2. On PROBE spi fail: re-SYNC cycling `device.spi_mode` 0..3 + INIT_SPI + PROBE  
3. On CAP spi fail: once-per-load CAP-time mode cycle in `spi_image_recover`  
4. Pre-sample warm CAP ×3  
5. Hold: ESD clear, IRQ=down, triple CAP, 4M  

Live: `*** SPI mode cycle OK mode=0 ***` and `*** CAP-time SPI mode OK mode=0 ***`
after re-SYNC; SN/fdt_ok/HEALTH still green; no frame raw without finger.

## Hold

`/tmp/do-enroll-m4al.sh` after user **go**.
