# M4aj — pinmux-only REE path hangs (2026-07-27)

## Attempt

Load `fp_pinmux_se14.ko` (un-reserve gpio56–59 + pinctrl map to `qup16`).
**Does not** create `geni_spi` / `of_platform` device.

## Result

- `insmod` → **SSH hang** then phone **self-rebooted** (~40s).
- Do **not** load `fp_pinmux_se14` or `fp_spi_se14` on this unit.
- Pins remain UNCLAIMED; `spi@a98000` remains `disabled`.

## Implication

REE SPI frame path is not safe without a redesign (e.g. deferred TLMM
write with known-good sequence, or only after Android A-slot leaves
pinmux). Continue **TA-only** force CAPTURE / DEAD_PIXEL / SRAM harvest
(M4ai) which already yields non-flat IMAGE PIXELS without pinmux.

## Evidence

Scratch: pinmux load attempt timed out; phone uptime reset to 0 min.
