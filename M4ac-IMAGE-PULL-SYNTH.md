# M4ac — TA SPI pull + synthetic ion frames (2026-07-27)

## Agent-implemented (not criterion-5 user blocker)

| Path | Result |
|------|--------|
| `FF_TA_XFER_SPI` 0x1009 | SCM OK; rdata **zeros** |
| `FF_TA_READ_SRAM` 0x101b | SCM OK; flat **u16=514** (rejected by dynamic-range) |
| Synthetic 112×88 in ion | **`IMAGE PIXELS` avg~428 min~200 max~656** |
| SAVE/REPORT/large CAP with synth | err=0, **no `enrolling...`** |
| REE `fp_spi_se14` geni bind | **hang/reboot** (avoid) |
| Leave FDT → WMODE-5 before CAP | no rem |

## Conclusion so far

HLOS **can** place a real-looking frame in shared ion. TA enroll/rem **does not** consume that buffer; it needs internal `ft9362_img_scan_*` (never logs `sensor raw data scanning` on pmOS). Next: reverse when CAPTURE calls `img_scan_start` vs early-return (`last mode is fdt mode RETURN`).

## Evidence

- Scratch: `fp-fresh-enroll-synth.log`, `fp-synth-big.log`, `fp-image-pull2.log`, `test_synth_frame.out`
- Module: `artifacts/qsee-modules/qsee_fingerpr.ko`
