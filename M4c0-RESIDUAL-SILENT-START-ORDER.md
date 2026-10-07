# M4c0 — residual budget, silent recovery, START-before-listeners (2026-07-27)

## Diagnosis

1. **Residual hard cap ~1322 B** fills mid Step1 try2 (Android needs ~1283 pure
   text for FDT→Base; recovery adds “unknown device mode” + tags).
2. **Hypothesis “fw=0 lets AutoSDac finish without log abort” is FALSE.**
   Measured REC-S residual len=298: entry/trans only, **no Step1**. Silent
   firmware log disables the recovery AutoSDac path, not just the prints.
3. **START after listeners failed** on clean boot (`START focal32 -5` /
   build_img). **START before listeners works**: `focal32 id=4`, `fingerpr id=5`.
4. Extra SYNC_CFG keys as JSON **bool** when TA expects **JSON_INTEGER** →
   `expect 'JSON_INTEGER' but got 'JSON_BOOL'` (reverted).
5. Clean-boot FDT force still hits SPI: `configure spi mode failed`,
   `init_chip interface hasn't been registered` when probe path is cold.

## Code changes

- `try_force_img_base_calib`: warm re-INIT, residual head+**tail** dump,
  full-SESS ion scan, fw=3 recovery (no fw=0).
- `qsee_fp_init`: **START TAs before** FILE/RPMB/SSD listeners.
- Louder `build_img` / `read_file` errors.
- Config: remove wrong-type extra keys; `f32_config_text[2048]`.

## Clean-boot protocol (measured)

```sh
# 1) START only
insmod qsee_fingerpr.ko do_start=1 probe_level=1
# → START focal32 id=4, fingerpr id=5
rmmod qsee_fingerpr   # TAs stay in TZ

# 2) Full probe (force ids; listeners OK after START)
insmod qsee_fingerpr.ko force_f32_id=4 force_fp_id=5 do_start=0 \
  enable_fs_listener=1 enable_rpmb_listener=1 enable_ssd_listener=1 \
  quiet_send=1 spi_bus_num=14 spi_capture_bps=8000000
```

## Still open

- **newDAC / Base update** not observed (need SPI-warm session like pre-reboot).
- RPMB register still often **-22** on clean boot; SFS serial still GENERIC.
- Residual still truncates mid try2 when SPI/FDT work (pre-reboot M4by/M4bz).

## Next

1. On SPI-warm path (post long session, not cold FDT-fail): verbose recovery
   only + residual tail for newDAC.
2. RPMB: try sb size 0x10000 fallback if 0x6400 → -22.
3. Keep START-before-listeners; never cue finger hold until Base/dac green.
