# M4f — focal32 device-command path OPEN on pmOS (2026-07-26)

## Breakthrough

With **ion session framing** (`req_len=0x78000`, `rsp@0x78000/0x8040`) aimed at **`focal32`** (Android’s `trustlet_path`), the TA accepts real device commands and returns structured status — **not** `0xffffffff`.

| Command | Name | `err` @req+8 | Notes |
|---------|------|--------------|--------|
| `0x205` | version base | (log) | Version `v2.5.3.0-cdf0f80-230614@0928` |
| `0x1011` | entry / SET_KM | (log) | keymaster identity empty |
| `0x100d` | **SYNC_CONFIG** | **`0x0` SUCCESS** | Parses our JSON; missing keys → defaults |
| `0x1006` | INIT_SPI | `0xfffffffb` (−5) | Recognized; SPI backend missing |
| `0x100a` | PROBE_DEVICE | `0xffffff33` | Recognized; needs SPI/chip |
| `0x1016` | CHECK_HEALTH | **`0x0` SUCCESS** | |
| `0x1004` | INIT | fail if pre-probe | Assert: chip not ready |

**Response wire format (device TA / ion):**

```
req[0]  = 0x80000000 | cmd   # completion bit (matches Android HAL check)
req[8]  = signed err         # 0 = OK
rsp[0]  = log length
rsp[4+] = ASCII TA logs (focaltech:ta:…)
```

## Critical correction

Android **device** trustlet is **`focal32`** with session **0x80040**, not `fingerpr`.

- `common.trustlet_path` = `…/focal32`
- `fingerpr` is the large algorithm TA (still mostly closed for raw cmds)

Earlier M4d fingerpr-only scans returning `0xffffffff` were probing the **wrong TA** for device open.

## SYNC_CONFIG payload

NUL-terminated text (JSON worked). TA logs:

```text
focaltech:ta:config|no key named 'diagnosis.framework_log_level', use default value.
```

Config **is ingested**.

## Remaining for chip ID / unlock

1. **INIT_SPI / PROBE need SPI** to the FT93xx (Android: bus 14, TA-mediated, `enable_spidev=off`).  
2. Map Android SPI bus 14 → mainline geni SPI + power/reset (L2 already has GPIO 34/35/60).  
3. After probe `err=0`, run INIT / INIT_DEVICE / calibration sequence.  
4. Then fingerpr for match/auth.

## Artifacts

- Source: `lab/fingerprint/oot-qsee/qsee_fingerpr.c` (device-TA path)  
- Module: `artifacts/qsee-modules/qsee_fingerpr.ko`  
- Evidence: `lab/fingerprint/qsee-focal32-device-HIT.txt`, implementer `qsee-f32-reordered.txt`  
- Android oracle: `lab/fingerprint/M4e-ANDROID-ORACLE.md`

## Re-test

```sh
# clean boot pmOS
scp artifacts/qsee-modules/qsee_{probe_fp5,fingerpr}.ko artifacts/fp-modules/focaltech_fp_life.ko fp5-pmos:/tmp/
ssh fp5-pmos 'sudo sh -c "
  mount -o ro /dev/disk/by-partlabel/modem_a /mnt/modem_a
  cp /mnt/modem_a/image/{focal32,fingerpr}* /lib/firmware/qsee/
  insmod /tmp/qsee_probe_fp5.ko force_pdev=1
  insmod /tmp/focaltech_fp_life.ko
  insmod /tmp/qsee_fingerpr.ko
  dmesg | grep -E \"SYNC_CFG|INIT_SPI|PROBE|err=0x0\"
"'
# Expect: SYNC_CFG err=0x0, HEALTH err=0x0, INIT_SPI/PROBE non-ffffffff with cmd name in log
```
