# M4ab — TA XFER_SPI / READ_SRAM / synthetic feed (2026-07-27)

## Implemented (agent software)

1. **FF_TA_XFER_SPI (0x1009)** — wlen/rlen + payload in ion; SCM OK, **rdata all 0** (protocol accepts, no useful MISO).
2. **FF_TA_READ_REG (0x1019)** / **READ_SRAM (0x101b)** — SCM OK; SRAM @0x2000+ shows **flat u16=514** (not image; dynamic-range filter rejects).
3. **Synthetic 112×88 frame** in ion (min~200 max~656) — `*** IMAGE PIXELS ***` with real range; SAVE/REPORT accept but **no `enrolling...`** (TA enroll ignores HLOS-only buffers).
4. **REPORT 0x2 before CAP** when finger down.
5. **REE geni_spi (`fp_spi_se14`)** — **hang/reboot** (same class as M4h); do not load until redesigned.

## Live evidence

- `fp-image-pull2.log`: XFER/SRAM path
- `fp-fresh-enroll-synth.log`: START id 4/5, SN, synth IMAGE PIXELS, rem_seen=0 honest, AUTH broken off
- Host: `test_synth_frame.out` PASS dynamic range

## Next (still software)

- Reverse `ft9362_img_scan_start` trigger inside CAPTURE (why never logs `sensor raw data scanning` even with finger).
- Or redesign REE SPI: unreserve + pinmux without blocking attach (avoid of_update/device_attach hang).
