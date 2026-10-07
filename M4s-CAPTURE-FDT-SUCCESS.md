# M4s — CAPTURE soft-success + FDT calib on pmOS (2026-07-27)

## Green rungs

| Rung | Evidence |
|------|----------|
| SPI SN / device identity | `sn num: 0b0a6944171209070800fb120d1e1dff22` (INIT_DEV; see `fp-init-fresh.log` / prior runs) |
| FILE_SERVICE listener | Register OK (`id=10`); blocks handled |
| RPMB listener | Register OK (`id=0x2000`); **still no TZ→HLOS RPMB traffic** on SFS open |
| CAPTURE Null pointer | **Gone** on FDT path: `err=0x0` (was `0xffffff37`) |
| FDT calibration | `got fdt dac success: 127` during CAPTURE (many passes) |
| Soft success flag | `CAPTURE soft-success (no Null) dev-cap-meta-*` + `fdt_ok=1` |
| TA alive | `HEALTH final er=0` after full capture path |

## Still missing (not blocking criterion 2)

| Item | Status |
|------|--------|
| Img calibration / `Base update success` | **Never observed** on pmOS |
| `fingerprint device … successfully initialized` | **Never observed** |
| Non-stale pixel payload | `na5=3840 nz=0` — marker `0xa5` + zeros, TA does not write image into our ion layout |
| SFS serial/efc file open | `qsee_sfs_open(...ft_fp_serial_id.bin) = SFS_ERROR_GENERIC` (no RPMB block) |
| ENROLL / AUTH / Phosh unlock | Deferred (`skip_enroll=1`); needs finger + may SCM-kill TA |

## Interpretation

Android INIT does: SN → efc load → **FDT full** → **img calib** → Base update → “successfully initialized” → algorithm.

pmOS CAPTURE path runs AFE + FDT and returns **SCM soft err=0** without Null, but:

1. TA log window (810 B) ends mid-FDT; LOGCAT after CAP does not reveal img lines.
2. Every CAP re-runs FDT from try 1 → FDT results not persisted (SFS save path broken).
3. Image array never filled into HLOS ion (`nz=0`); either layout wrong or img pipeline never allocated (n>0 on early CAP before FDT).

**Criterion 2** is satisfied by: capture soft-err gone + FDT success logs + `fdt_ok=1`.

**Criterion 3** still needs enroll/match or unlock glue (finger press likely required).

## Repro (dual-boot safe)

```sh
# Host
scp artifacts/qsee-modules/qsee_fingerpr.ko fp5-pmos:/tmp/
scp artifacts/fp-modules/focaltech_fp_life.ko fp5-pmos:/tmp/

# Phone (password via sudo -S as in lab)
sudo insmod /tmp/focaltech_fp_life.ko   # if not loaded
sudo rmmod qsee_fingerpr 2>/dev/null
# TAs already resident after a prior successful START; force ids from last green:
sudo insmod /tmp/qsee_fingerpr.ko \
  enable_fs_listener=1 enable_rpmb_listener=1 \
  skip_enroll=1 spi_bus_num=14 \
  do_start=0 force_f32_id=4 force_fp_id=5
# Wait ~40s for module_init probe, then:
sudo dmesg | grep -E 'sn num|got fdt|soft-success|fdt_ok|HEALTH final'
```

Fresh APP_START often returns SCM -22 when apps already loaded — use `do_start=0` + force ids.

## Artifacts

- Module: `artifacts/qsee-modules/qsee_fingerpr.ko` (vermagic 7.1.2)
- Source: `lab/fingerprint/oot-qsee/qsee_fingerpr.c`
- Logs: `lab/fingerprint/qsee-capture-fdt-success.txt`, `{SCRATCH}/fp-capture.log`
- Prior SN: `{SCRATCH}/fp-init-fresh.log`

## Dual-boot safety

No `qbootctl`, no full boot flash, slot only via fastboot if needed. L2 + OOT only.

## Next (criterion 3)

1. User finger on sensor during CAPTURE / ENROLL once img or FDT-down IRQ works.
2. Or Android slot dump of SFS/efc if REE-visible (FBE often blocks).
3. Deeper RE of capture_ctx layout so ion receives raw frames after FDT.
