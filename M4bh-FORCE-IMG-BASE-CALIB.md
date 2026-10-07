# M4bh — Force img Base update (image buffer bind) (2026-07-27)

## Gap (not flag thrash)

Android INIT_DEVICE always does:

```
FDT calib → Step1 tx_close(nav) → Step2 tx_open(nav) → Step3 raw img
→ Base update success → device successfully initialized
```

TA then has nav base/raw allocated; enroll `frame raw` works.

pmOS INIT_DEV typically logs only **SN + Null** — never `Step1` / `Base update success`.
TA strings: `image buffer = NULL`, `nav base or raw data memory is null`,
`failed to malloc memory for raw`.

Without Base update, **no TA frame raw / rem** even with finger + flag 0x240.

## Software

`try_force_img_base_calib()` after init seq:
- SPI **8M** (Android ~8.03M during Base update)
- WMODE-5 + SCAN + CAP flag **0x240** up to 10×
- DEAD_PIXEL mid-loop (alloc path)
- Stop on `Base update success` / image dac + Step
- Return FDT_DOWN for enroll

Default `spi_capture_bps=8000000`.

## Smoke

```
dmesg | grep -E 'IMG BASE CALIB|Base update|Step1|image buffer'
```
