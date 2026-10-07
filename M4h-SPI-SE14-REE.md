# M4h — REE SPI SE14 bring-up attempt (2026-07-26)

## Goal

Enable mainline `geni_spi` on `spi@a98000` so pinmux (gpio56–59 / qup16) is applied
via pinctrl and either (a) TZ SPI can open, or (b) REE SPI can talk to the chip.

## Blocker discovered

**`gpio-reserved-ranges = <32 2>, <56 4>`** on TLMM causes:

```text
msm_pinmux_request() → gpiochip_line_is_valid(56) == false → -EINVAL
geni_spi a98000.spi: Error applying setting (pinctrl)
```

So **neither GPIO consumers nor SPI pinctrl** can claim pins 56–59 until un-reserved.

## What worked

1. **Un-reserve at runtime** via `gpiochip_query_valid_mask` + `set_bit(56..59)`  
   → pins report `valid=1`.
2. DT status of `spi@a98000` set to **okay** (device node appears as `a98000.spi`).
3. Device has supplier links (gcc, interconnect, pinctrl, rpmhpd).

## What failed / crashed

| Action | Result |
|--------|--------|
| `of_update_property` (bad prop alloc) | **kernel oops** in `of_find_property` |
| `echo a98000.spi > geni_spi/bind` after partial state | **SSH hang** (waiting_for_supplier / probe stall) |
| SPI client chip-id patterns | never reached (no spi_master) |

## Module

`lab/fingerprint/oot-fp/fp_spi_se14.c` — experimental; **do not load until cleaned up**.

## Clean next sequence (after power cycle)

1. Boot pmOS clean.
2. Load only a **minimal** module that:
   - un-reserves 56–59
   - does **not** corrupt OF properties if status already okay
   - sets status okay only if disabled
   - `device_attach` with timeout / async, **not** blocking sysfs bind from shell
3. Confirm `geni_spi` binds → new `/sys/class/spi_master/spiN`
4. SPI transfer chip-id experiments
5. Re-run `qsee_fingerpr` with pinmux applied (and/or `enable_spidev=1`)

## Safety

- Avoid `geni_spi/bind` write if `waiting_for_supplier` is present.
- Prefer power cycle after any OOPS (tainted **D**).
- Keep `force_spi_pinmux` / raw TLMM off.
