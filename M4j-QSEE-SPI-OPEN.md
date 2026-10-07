# M4j — SPI OPEN on pmOS (2026-07-26) ✅

## Result: TZ SPI works; chip serial read on postmarketOS

With **nested JSON** `SYNC_CFG` (not flat dotted keys), the focal32 TA opens SPI and
talks to the FT93xx:

| Command | err | Notes |
|---------|-----|--------|
| SYNC_CFG | **0** | Nested config installs |
| **INIT_SPI** | **0** | was −5 forever with flat config |
| **PROBE** | **0** | SPI register/SRAM reads |
| **INIT_DEVICE** | **0** | foundry **nexchip**, SN **`0b0a6944171209070800fb120d1e1dff22`** |
| INIT | **0** | |
| CHIP_PARAM_INIT | **0** | |
| HEALTH / check_alive | **0** | open-short data `0x4` |

Evidence: `lab/fingerprint/qsee-SPI-WORKS-nested-json.txt`

### How to reproduce

```sh
# clean pmOS boot, TAs in /lib/firmware/qsee/
insmod qsee_probe_fp5.ko force_pdev=1
insmod focaltech_fp_life.ko
insmod qsee_fingerpr.ko   # cfg_style=3 default (nested JSON), spi_bus_num=14
dmesg | grep -E 'INIT_SPI|INIT_DEV|sn num|err=0x0'
```

## Root cause of prior INIT_SPI −5

Flat dotted JSON:

```json
{"driver.spi_bus_num":14, ...}
```

TA logged “configurations is synced” but **did not install** keys. SPI then:

```text
no key named 'driver.spi_bus_num', use default value.
attaching to QSEE_SPI_DEVICE_1 (QUP0 SE 0)...
qsee_spi_open(0) = -1   # I/O error → app err -5
```

Nested JSON works:

```json
{"driver":{"spi_bus_num":14,"enable_spidev":false,...},
 "device":{"spi_default_bps":4000000,...}}
```

## Bus map (confirmed by success path)

`QSEE_SPI_DEVICE_N (QUP N/8, SE N%8)` → bus **14** = wrap1 SE6 @ `a98000`.

## Not a hyp/pinmux problem

REE pinmux force / geni bind were unnecessary and unsafe. Once the TA got the
correct bus number, **qsee_spi_open under mainline HLOS worked**.

## Remaining for session unlock

1. **SFS paths** still Android (`/data/vendor_de/0/fpdata/...`) — need pmOS storage or redirect.
2. **fingerpr** algorithm TA + AUTH / enroll templates.
3. **IRQ / listen** path for finger-down (L2 already has IRQ34).
4. Phosh / PAM / fprintd (or custom) glue.

L3 electrical identity is met (serial + SPI probe + health). Full greeter unlock is next.
