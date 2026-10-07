# M4bd — finger-touch wait, no mid-press reset (2026-07-27)

## M4bc hold result
- ENROLL + samples with **TA DOWN** (IRQ edge `assume=1`, **qes=0**)
- **IMAGE PIXELS** nz≈3719 under finger, `vio=1`, `spi_fail=0` at force begin
- **rem_seen=0**, no TA/REE `frame raw`, AUTH **broken off**
- Sticky **[esd]** through every sample; `esd_clear` never cleared QES
- `wait_chip_ready` / `esd_clear` ran **during** force-image under finger → l2_reset mid-press

## Android oracle (enroll)
1. IRQ itype 0x2 → QES **`[finger touch]`**
2. `ft93xx:img: frame raw[0..2]` (~50ms) avgv~330
3. TA `got finger touched` + `enrolling... rem`
4. No IC reset mid-touch

## Code (M4bd)
1. Under finger: **skip** `wait_chip_ready` / heavy SPI rec / **dead-pixel a5**
2. On IRQ edge: poll QES ~320ms for **`finger touch`** before CAP
3. Drop synthetic feed under finger
4. Keep M4bc: force-image only when ta_down; no idle CAP thrash

## Hold
```sh
sh /tmp/do-enroll-m4bc-body.sh
# look for: finger touch QES | frame raw | rem_seen=1 | enrolling
```
