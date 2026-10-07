# M4al — unreserve OK; any pinmux to qup16 hangs (2026-07-27)

## Safe

| Action | Result |
|--------|--------|
| `fp_unreserve_se14.ko` (valid_mask only) | **OK** — gpio56–59 valid=1, no hang |
| TA force CAPTURE / DEAD_PIXEL / SRAM harvest | IMAGE PIXELS (nz up to 1493 / sparse 24) |

## Unsafe (causes hang + self-reboot)

| Action | Result |
|--------|--------|
| `fp_pinmux_se14` pinctrl_select qup16 | hang + reboot (M4aj) |
| `fp_spi_se14` geni bind | hang (M4h) |
| debugfs `pinmux-select` `gpio56 qup16` | hang + reboot (M4al) |

## Conclusion

**Do not touch TLMM pinmux for SE14/qup16 on this unit.** REE SPI frame path
is blocked until a non-hanging pinmux method is found (or Android A leaves
mux intact after dual-boot — untested).

Continue **TA-only** image path. `configure spi mode failed` persists without
pinmux; SN/FDT still work via TZ SPI.

## Module

`lab/fingerprint/oot-fp/fp_unreserve_se14.c` — optional, safe, no pinmux.
