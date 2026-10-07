# M4c1 — L2 v0.5 SE@19.2M → Base update success (2026-07-27)

## Root cause of cold-boot SPI death

Phone had **focaltech_fp_life v0.1** (power/IRQ only, **no spiclk**).
After reboot: `vio 1.8v fail` forever → `init_chip not registered` → no FDT/Step1.

**artifacts/fp-modules/focaltech_fp_life.ko v0.5** has:
- `auto_spiclk=1`
- `spi_se_rate_hz=19200000` (SE14 / wrap1 SE6)
- sysfs `spiclk`

## Smoke (clean boot)

```
insmod focaltech_fp_life.ko auto_spiclk=1 spi_se_rate_hz=19200000
echo 1 > /sys/class/focaltech_fp/focaltech_fp/{vdd,spiclk,listen}
insmod qsee_fingerpr.ko do_start=1 enable_fs_listener=1 \
  enable_rpmb_listener=1 enable_ssd_listener=1 quiet_send=1 \
  spi_bus_num=14 spi_capture_bps=8000000
```

## Result

| Marker | Value |
|--------|--------|
| vio 1.8v | **success** |
| sn num | 0b0a6944171209070800fb120d1e1dff22 |
| FDT | got fdt dac success 127 |
| Step1 | newDAC 122 middle 872; newDAC 126 middle 870 |
| Base | **Base update success** @ ion |
| force calib | **skipped** (already green) |
| CAP no finger | nz mostly 0 / a5; SRAM pull nz≈33 |

## Also shipped

- RPMB register fallback 0x6400 → 0x10000 on SCM -22
- START-before-listeners (M4c0)
- Residual head+tail + full SESS ion scan

## Next

Finger hold: ENROLL / force-image under finger for `frame raw` / rem / AUTH.
Cue **go** only with L2 v0.5 + Base green module loaded.
