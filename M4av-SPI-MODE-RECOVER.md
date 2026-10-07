# M4av — configure spi mode failed blocks frame raw (2026-07-27)

## Root cause (live)

During CAPTURE / force-image the TA log shows:

```text
configure spi mode failed(0).
```

paired with either `vio 1.8v fail` or later `vio 1.8v success`.

TA strings place this in `ft9362_config_spi_mode` next to:

- `host omit or miss interrupt to handle`
- chip id / image path

**Without successful chip SPI mode config, `ft93xx_img_scan_*` / `frame raw[%d]` never run.**  
FDT/SN can still soft-succeed (different path). HLOS IMAGE PIXELS from SRAM/force is not enroll rem.

Android: `enable_spidev=off`, `spi_default_bps=4000000`, capture_bps often unset (4M).  
`frame raw` is logged from **TA** (`frame raw[%d]: avgv=...` in TA binary), not REE kernel.

## Software (M4av)

1. `spi_image_recover()` — soft reset+4M first; VDD only if still fail; longer settle  
2. Force-image: on CAP `last_send_spi_mode_fail`, recover + one CAP retry  
3. Hold uses `spi_capture_bps=4000000` (was 8M)  
4. `spi_img_recover=1` default  

Smoke: SN, fdt_ok, recover path fires, HEALTH 0; still no frame raw without finger+ENROLL (expected for `skip_enroll`).

## Not done

- REE pinmux/geni still **hard hang** — do not load  
- rem_seen under finger still needs cued hold with M4av + M4au (ESD, IRQ=down, triple CAP)

## Hold

`/tmp/do-enroll-m4al.sh` after user **go**.
