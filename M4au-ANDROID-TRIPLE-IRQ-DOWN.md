# M4au — Android triple CAP + IRQ-as-down + ESD mid-wait (2026-07-27)

## Why

M4as hold: `ta_down=0`, `rem_seen=0`, sticky QES `[esd]`, AUTH `broken off`.
Android enroll (`dumps/android/fp-enroll-first-*/focaltech-events.txt`):

1. IRQ `itype 0x2` / finger touch  
2. **frame raw[0..2]** (~50–70 ms)  
3. `ta:core: enrolling...` + HAL `transmit_image` ~37 ms  
4. `FDT_UP_DETECT` → leave → `FDT_DOWN_DETECT` for next press  

Pure TA CAPTURE does not log REE `ft93xx:img: frame raw`; rem needs real img scan
under finger. REE pinmux still hard-blocked (hang). Stay on TA path.

## Code (`qsee_fingerpr.c`)

| Change | Default |
|--------|---------|
| `force_img_mode5=0` | no mode-5 prime (was re-sticking ESD) |
| `sample_esd_clear=1` | ESD clear pre-sample + mid-wait + post-sample |
| `irq_means_down=1` | L2 IRQ edge ⇒ `ta_down` (do not require QES parse) |
| `irq_edge_delta=1` | was +2 (missed single-IRQ presses) |
| `sample_triple_cap=3` | three CAPTUREs under finger (Android frames) |
| FDT_UP→FDT_DOWN | after **every** sample (not only `ta_down`) |
| QUERY_FINGER | with QEV during poll / IRQ edge |

## Smoke (no finger)

- SN, `fdt_ok=1`, IMAGE PIXELS nz≥24, HEALTH final er=0  
- Scratch: `{SCRATCH}/fp-m4au-smoke.log` → also `fp-init.log` / `fp-capture.log`

## Hold

```sh
# after user says go:
ssh fp5-pmos 'bash /tmp/do-enroll-m4al.sh'
# look for: IRQ, TA DOWN, ta_down=1, PRE-FORCE, frame raw / enrolling / rem_seen=1
```

## Still open

- Criterion 3: rem + AUTH match under finger  
- HLOS IMAGE PIXELS ≠ TA enroll rem without finger img_scan  
