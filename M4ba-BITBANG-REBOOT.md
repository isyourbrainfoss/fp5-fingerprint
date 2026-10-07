# M4ba — REE bitbang SPI attempt (2026-07-27)

## Goal

Close skeptic gap: REE SPI frame path on bus 14 without geni/pinctrl hang.

## Implementation

`lab/fingerprint/oot-fp/fp_bitbang_se14.c`:
- un-reserve valid_mask for gpio56–59
- `gpiochip_request_own_desc` as GPIO (no qup16 mux)
- bitbang mode-0 chip-id probe with pin permutations

## Result

**Load caused phone hang + self-reboot** (uptime reset to ~2 min; SSH reset).

Same failure class as pinmux/geni: **any HLOS claim of SE14 pads is unsafe** on this unit.

## Conclusion

| Path | Status |
|------|--------|
| pinmux qup16 | hang/reboot |
| geni_spi bind | hang/reboot |
| GPIO bitbang request | hang/reboot |
| unreserve-only | OK |
| TA SPI SN/FDT | OK |
| TA image SPI mode | fail (`configure spi mode failed`) |

Continue **TA-only** image/rem path. Do not load `fp_bitbang_se14` / pinmux / geni.

## Next software (TA-only)

1. `skip_capture_path` enroll — avoid pre-enroll CAP thrash  
2. User hold with clean SPI after init  
3. Force-image under finger only after chip ready  
