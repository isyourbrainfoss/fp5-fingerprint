# M4c6 hold — CAP residual: finger status N (2026-07-27)

## Result
FORCE MINIMAL CAP ran; vio=1; esd=0; **frame raw=0**, rem_seen=0.

## Residual (actual CAP window, len=222)
```
set chip vio 1.8v success
cidri: 95 f: 3(0:U 1:S 2:D 3:N) v: 4(...)
chip feature, io contro
```
`f:3` = **N (none)** — TA has no finger-down. MARK only `got fdt dac`;
`capture_ctx rows=0` (FDT soft path).

## Implication
flag 0x240 + vio + Base are **not enough**. `ft93xx_img_scan` / `frame raw`
requires TA finger-down state (Android: itype 0x2 + QES [finger touch] before CAP).
Without IRQ/finger, CAP stays FDT-only forever.

## Next
1. Keep FDT wait-touch armed (no FDT_UP after failed sample) — M4c7.
2. User press during armed wait for itype 0x2.
3. Pure REE SPI image still blocked (TA owns SPI; synth not consumed for rem).
