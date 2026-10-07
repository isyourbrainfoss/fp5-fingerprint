# M4aw — raise wrap1 SE6 clock for image SPI (2026-07-27)

## Discovery

Live debugfs before fix:

| Clock | enable | rate |
|-------|--------|------|
| `gcc_qupv3_wrap1_s6_clk` | 1 | **2.048 MHz** |
| core / core_2x | 1 | 0 |

TA SPI is **4 MHz**. With SE at 2.048 MHz, CAPTURE/image path logged:

```text
configure spi mode failed(0).
vio 1.8v fail, try again....
```

FDT/SN still worked (lighter SPI). No TA `frame raw` / img_scan.

## Fix (`focaltech_fp_life.c`)

- Module param `spi_se_rate_hz` (default **19200000**)
- `clk_set_rate(se, …)` before enable; fall back 32M
- Prefer core/core_2x rate hints
- **Never** auto pinctrl_select / TLMM unless `force_spi_pinmux=1` (hang)

## Live after reload

```text
SPI SE rate request=19200000 got=32000000 Hz
SPI_CLK ENABLED ... se_rate=32000000
gcc_qupv3_wrap1_s6_clk rate=32000000
```

Force CAP retry after soft recover: **`vio 1.8v success`** without spi-mode-fail on CAP itself.
Still no `frame raw` without ENROLL+finger (expected for `skip_enroll` smoke).

## Hold

Load L2 with rate, then `/tmp/do-enroll-m4al.sh` after user **go**.

## Clean-boot retest (SE@19.2M before TA open)

Still **15×** `configure spi mode failed` on first force CAP; later CAP **vio success**.
No `frame raw` without enroll+finger. XFER MISO still garbage (not real SPI rdata).
SN/FDT still green → TA internal SPI works for probe path.

Warm-CAP loop added before enroll samples so finger path starts after vio OK.

## Hold

`/tmp/do-enroll-m4al.sh` after user **go** (TAs already resident preferred).
