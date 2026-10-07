# M4d — fingerpr open attempts (2026-07-26)

## Goal

Get a **non-`0xffffffff`** response from the `fingerpr` TA (any of INIT / CREATE / version / SET_KM_KEY).

## RE: HAL wire format (fingerprint.default.so)

From `ff_cmd_name` jump table and `ff_trustlet_client_open_session` / `ff_trustlet_client_exchange_message`:

| Item | Value |
|------|--------|
| TA cmd range | `0x1000`–`0x1023` (`FF_CMD_TA_*`) |
| SVC cmd range | `0x3000`+ (`FF_CMD_SVC_*` → `/dev/focaltech_fp`) |
| `0x0205` | `FF_CMD_SVC_GET_VERSION_INFO` style version (plen `2`) — **works on focal32** |
| `0x1011` | **`FF_CMD_TA_SET_KM_KEY`** (not a free “entry” opcode; focal32 log text is side-effect) |
| `0x1004` | `FF_CMD_TA_INIT` (template plen `0`) |
| `0x1001` | `FF_CMD_TA_CREATE` |
| `0x1006` | `FF_CMD_TA_INIT_SPI` |
| `0x100a` | `FF_CMD_TA_PROBE_DEVICE` (plen `1`) |

**Message layout** (staging → ion req):

```
u32 cmd;     // @0
u32 plen;    // @4  payload byte count
// pad to 0x10
u8  payload[plen];  // @0x10
```

**Ion session** (`QSEECom_start_app(..., 0x80040)` for fingerpr):

```
req: offset 0,      length 0x78000
rsp: offset 0x78000, length 0x8040
total 0x80040
```

`QSEECom_send_cmd(handle, sbuffer, 0x78000, sbuffer+0x78000, 0x8040)`.

focal32 is started with smaller session **`0x840`** (req `0x40` / rsp `0x800`).

Open-session order in HAL:

1. start **fingerpr** `0x80040`
2. start **focal32** `0x840`
3. `0x205/0x2` → focal32  
4. on status 0, copy KM material → `0x1011` exchange on fingerpr ion

## Experiments on pmOS (clean reboot)

Module: `artifacts/qsee-modules/qsee_fingerpr.ko`  
Source: `lab/fingerprint/oot-qsee/qsee_fingerpr.c`  
Log: implementer `qsee-fingerpr-clean-run.txt`

| Step | Result |
|------|--------|
| `APP_START focal32` | **OK** id=4 |
| `APP_START fingerpr` | **OK** id=5 |
| focal32 `0x205/0x2` | **OK** `st=0x22a`, version `v2.5.3.0-cdf0f80-230614@0928` |
| focal32 `0x1011` | **OK** `st=0x32a`, “keymaster identity: ''” |
| `REGION` 2-arg `addr,sz` / `sz,addr` / `addr,1M` | **−22** |
| `REGION` 3-arg `app_id,addr,sz` | **−12** (ENOMEM-ish; still fail) |
| fingerpr ion SEND all TA cmds above | **SCM ret=0**, **rsp[0]=0xffffffff** |
| Smaller splits `0x40/0x800`, `0x840`, `0x1000` | same `0xffffffff` |
| Aggressive REGION (RW / TZ_APPS owner) | **avoid** — hard-reset observed once |

Poison pattern `0xee` in rsp is overwritten only in the first dword → TA/OS **does** write a failure status; this is not a silent no-op buffer.

## Interpretation

1. **Command IDs and ion sizes match Android HAL** — framing is no longer the primary unknown.
2. **focal32** accepts simple TZ-mem SEND without ion registration.
3. **fingerpr** always returns application status `0xffffffff` for every tried opcode, including INIT/CREATE/version.
4. Mainline still documents **no incomplete / listener** handling (`qcom_scm.c` TODO + `WARN_ON` for `INCOMPLETE` / `BLOCKED_ON_LISTENER`).
5. fingerpr binary strings reference **QIS Listener**, SPI instance creation, CTAMemorySpace / GP memory objects — heavy HLOS service surface vs focal32.
6. L2 `/dev/focaltech_fp` is power/IRQ only; **no real SPI xfer** (`FF_CMD_SVC_XFER_SPI_DATA` not implemented).

Most likely remaining blockers (ordered):

1. **Missing qseecom listener / incomplete-cmd loop** (qseecomd-class services, QIS).
2. **Session ion never registered** at `start_app` time (Android binds 0x80040 ion into the handle; our REGION_NOTIFY fails).
3. **SPI / probe path** needs kernel bridge + TA INIT_SPI before useful status codes.
4. **Keymaster identity empty** may block SET_KM_KEY handoff (secondary; INIT should not need it).

## Artifacts

- `lab/fingerprint/oot-qsee/qsee_fingerpr.c`
- `artifacts/qsee-modules/qsee_fingerpr.ko` (vermagic 7.1.2, this_module 0x500)
- Evidence: clean-run dmesg in implementer scratch + this file

## Re-test

```sh
# after reboot on pmOS
scp artifacts/qsee-modules/qsee_{probe_fp5,fingerpr}.ko \
    artifacts/fp-modules/focaltech_fp_life.ko fp5-pmos:/tmp/
ssh fp5-pmos 'sudo sh -c "
  mount -o ro /dev/disk/by-partlabel/modem_a /mnt/modem_a
  cp /mnt/modem_a/image/{focal32,fingerpr}* /lib/firmware/qsee/
  insmod /tmp/qsee_probe_fp5.ko force_pdev=1
  insmod /tmp/focaltech_fp_life.ko
  echo 1 > /sys/class/focaltech_fp/focaltech_fp/listen
  insmod /tmp/qsee_fingerpr.ko
  dmesg | grep qsee_fp
"'
```

## Android stack extract (no slot switch)

Pulled from `super` → `vendor_a` via `lpunpack` on pmOS:

`dumps/android/vendor-qsee-20260726/` — `libQSEEComAPI.so`, `qseecomd`, `qseecom-mod.ko`, `focaltech_fp.ko`, init RCs.

From Lineage `qseecom.c` + DT:

| Fact | Detail |
|------|--------|
| `QSEOS_APP_REGION_NOTIFICATION` | **Global** carveout (`platform_get_resource_byname(..., "secapp-region")`), **not** per-session ion |
| pmOS DT | `reserved-memory/sec-apps@808ff000` size **0x1000** — REGION still **−22** |
| Ion buffers | Must lie in registered shared region (`cmd buf … out of shared buffer region`) |
| Incomplete cmds | Android dmesg already shows `__qseecom_reentrancy_process_incomplete_cmd` (app_id=1) |
| Services | `qseecomd` + `vendor.qti.hardware.qseecom@1.0-service` running on slot A |

## Next (M4e)

1. **Android oracle (slot A):** needs **Vol Down+Power → fastboot** once (`fastboot --set-active=a`); strace first fingerpr `send_cmd` + listener list. Soft `reboot bootloader` failed.
2. **Map Android ion heap** that backs fingerpr `0x80040` (which phys range; does mainline `qcom_tzmem` land there?).
3. **Listener / incomplete-cmd** path on mainline (explicit TODO in `qcom_scm.c`).
4. **SPI L3** after messaging works.
