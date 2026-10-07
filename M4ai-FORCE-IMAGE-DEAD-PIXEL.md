# M4ai — Force image path: CHECK_DEAD_PIXEL + dual img_off (2026-07-27)

## Software (no user)

1. **`FF_TA_CHECK_DEAD_PIXEL` 0x1020** plen `0x24` — same capture_ctx framing as CAPTURE (HAL pool).
2. **`try_force_image_path()`** after FDT soft-success and on enroll samples when no frame raw:
   - SET_SPI_SPEED 4 MHz
   - WMODE-5 → SCAN → (optional REPORT 0x2) → FDT_DOWN
   - CAPTURE + DEAD_PIXEL over flags `{1,0x41,0x240,0x200}` × img_off `{0x24,0x100}`
   - LOGCAT + XFER/SRAM pull
3. **ESD**: VDD cycle + require positive non-`[esd]` QES (`last_send_qes_ok`).
4. Params: `try_dead_pixel=1`, `dual_img_off=1`.

## Smoke (no finger)

### Round 1 — `fp-m4ai-force-smoke.log`
Post-force RSRAM @0x1000: sparse dynamic (min=1 max=1792); ion CAP still nz=0.

### Round 2 — `fp-m4ai-smoke2.log` (threshold fix + SRAM harvest)

| Marker | Result |
|--------|--------|
| SN + CAP soft-success + fdt_ok | green |
| **force CAPTURE f=1 off=0x24** | **`*** IMAGE PIXELS ***` nz=1493 avg=517 min=514 max=757** |
| **DEAD_PIXEL 0x1020** | same ion IMAGE PIXELS |
| **RSRAM @0x1000** | IMAGE PIXELS nz=24 max=1792 → **copied to ion+0x24** |
| force-image best_nz | **1493** (post-fdt total ~1621) |
| TA `frame raw` / `sensor raw` | **still 0** |
| HEALTH | 0 (TA alive) |

Partial **real** image pipeline without finger: non-flat ion pixels after force CAP + DEAD_PIXEL. Still missing TA enroll markers.

## REE SPI

`spi@a98000` still **disabled** in DT; geni bind hangs (M4h). Existing `spi13` is touchscreen `@a94000` — not FP. Continue TA path; redesign REE only as pinmux-only later.

## Next

1. After force path, **READ_SRAM @0x1000 len=full frame** into ion and claim pixels if max−min>16.
2. User **go** hold with ESD-clear + force-image under real finger (need `frame raw` / rem).
3. Investigate `configure spi mode failed(0)` (present even when SN works).
