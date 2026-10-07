# M4ci — SPI speed under REAL DOWN; no toxic skipcap CAP

## Evidence (M4ch hold)

| Marker | Value |
|--------|-------|
| REAL DOWN | yes (itype 0x2, touch=1) both samples |
| CAP ms | **~620–650** each (Android img ~12–18 ms) |
| residual | **cidri f:3=N** |
| frame / rem | 0 |
| skipcap CAP | **vio 1.8v fail** + **configure spi mode failed** |

Dropping inter-CAP LOGCAT (M4ch) did not fix path: each CAPTURE is still FDT-DAC.

## Fix

1. **skip_capture_path**: no soft CAP (flag=1) — only PWR+WMODE+SCAN+SPI speed + L2 vdd/spiclk. Avoids vio/spi-fail poison before ENROLL.
2. **AFR under REAL DOWN**: `SET_SPI_SPEED` only (no WMODE/reset) at `spi_capture_bps` before burst.
3. Default **`spi_capture_bps=4000000`** (Android default when unset).
4. If CAP0 still FDT-slow (`ms>200`): continue rest @ **8M**; if still no frame raw, one ALT CAP **flag=0x41**.

## Hold

```sh
# after deploy — wait for user "go"
# look for: SPI-SPEED-afr, afr CAP ms=, FDT-slow, ALT, frame raw / rem_seen
```
