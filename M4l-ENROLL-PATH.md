# M4l — Enroll path on pmOS (2026-07-26)

## Goal

Enroll a fingerprint under pmOS so AUTH can unlock a Phosh session.

## What works (still)

| Step | Result |
|------|--------|
| L2 `focaltech_fp_life` + clocks/ICC | OK |
| Nested JSON `SYNC_CFG` (`cfg_style=3`), SPI bus 14 | OK — chip SN present |
| Device TA cmds `INIT_SPI` / `PROBE` / `INIT_DEV` / `INIT` / `HEALTH` | err=0 |
| `SET_ACTIVE_GROUP` (0x2007) | err=`0xfffffffe`, log: **0 template loaded** |
| **`PRE_ENROLL` (0x2000)** | **err=0**, real challenge returned |

### PRE_ENROLL response layout (confirmed)

After successful PRE on shared ion buffer:

```
req+0x00: cmd | 0x80000000          (completed)
req+0x04: plen = 8
req+0x08: err = 0
req+0x0c: flag (sometimes 1)
req+0x10: u64 challenge (LE)         e.g. 0xbc0c8ffa25fcc85f
```

Evidence: `lab/fingerprint/qsee-enroll-clean.txt`, `qsee-enroll-preserve.txt`.

## What fails

| Step | Result |
|------|--------|
| **`ENROLL` (0x2001)** | **SCM −22**, then **all further focal32 APP_SEND → −22** (TA dead) |
| `POST_ENROLL` (0x2002) | −22 after dead TA |
| `AUTHENTICATE` (0x2008) on focal32 | −22 |
| fingerpr high-level cmds | SCM OK, app status `0xffffffff` (wrong TA / framing) |

### ENROLL attempts that all still kill the TA

1. Zero HAT / plen=0  
2. Full HAT (0x45) + gid@0x45 + flag@0x49, plen **0x4a** (HAL constant pool)  
3. HAT with PRE challenge + `authenticator_type` BE = 2 (fingerprint)  
4. Direct ion framing (`cmd@0`, `plen@4`, payload@`0x10`)  
5. **Segmented** framing (`0x1000` wrapper — Android HAL path for non-inline payload)  
6. Soft buffer preserve (no full 0x2000 wipe after PRE)  
7. Work-mode sweep 0–3 + `START_SCANNING` immediately before ENROLL  

After any ENROLL −22, `CHECK_HEALTH` also −22 → app is gone until reboot.

Evidence: `qsee-enroll-try*.txt`, `qsee-enroll-clean.txt`, `qsee-enroll-seg.txt`, `qsee-enroll-wake.txt`.

## HAL / kernel notes

- HAL `ff_trustlet_enroll`: opcode **0x2001**, plen **0x4a**, HAT layout matches our payload.  
- Android **soft-fails** bad HAT (`invalid enrollment challenge` / untrusted forbidden) — does **not** SCM −22.  
- Mainline `qcom_scm_qseecom_call`: **BLOCKED_ON_LISTENER / INCOMPLETE not implemented** (`qcom_scm.c` TODO + WARN_ON).  
- Template store is REE (`ff_template_*.bin` under store path); SVC `GET/SET_TEMPLATE` exist in HAL.  
- Android `userdata` is not mountable from pmOS without FBE keys (template import blocked).

## Config applied (nested JSON)

```json
"trustlet": {
  "enable_trusted_enrollment": false,
  "enable_authenticate_token": false,
  "enable_fingerprint_payment": false,
  "enable_secret_data_backup": false
}
"common": {
  "default_data_home": "/var/lib/fp5-fp",
  "persist_data_home": "/var/lib/fp5-fp"
}
```

`/var/lib/fp5-fp/templates` created on device. SET_GROUP still reports 0 templates.

## Bugs fixed along the way

- **`apay[64]` overflow** when writing plen `0x4a` (74) — fixed with `hat[0x50]` / `apay[128]`.  
- Full req wipe before ENROLL may discard PRE challenge — send path now only overwrites cmd/plen/err and payload slice.  
- Reusing TA ids after `rmmod` without reboot: START −22; force-ids only work if app still alive and ids known. Prefer **clean reboot + do_start=1**.

## Module

- Source: `lab/fingerprint/oot-qsee/qsee_fingerpr.c`  
- Artifact: `artifacts/qsee-modules/qsee_fingerpr.ko` (vermagic 7.1.2)  
- Deploy: probe → L2 → `insmod qsee_fingerpr.ko` (default nested JSON, bus 14)

## Next (priority)

1. **Handle QSEECom blocked-on-listener / incomplete** in send loop (or implement minimal listeners for `FF_CMD_SVC_GET/SET_TEMPLATE`). Kernel explicitly lacks this.  
2. **Low-level capture path**: `CAPTURE_IMAGE` (0x1013, plen 0x24) + IRQ/FDT events — may not need high-level ENROLL.  
3. On Android slot: dump `ff_template_*` from `/data/vendor_de/0/fpdata` (root) and try `SYNC_TEMPLATE` / `UPDATE_TEMPLATE` under pmOS.  
4. Do **not** load `fp_pinmux_se14` / geni experiments (watchdog risk).

## Success bar (unchanged)

Unlock pmOS (Phosh) session with fingerprint.
