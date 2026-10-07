# M4 results — APP_SEND reaches loaded `fingerpr` (2026-07-26)

## Headline

**Mainline can SEND to the loaded fingerprint TA.**  
SCM returns success; the app answers with application-level status words. Command framing is not fully reversed yet (most probes return `0xffffffff` status or empty payload).

## Proven pipeline (pmOS, no Android boot required)

```
qsee_probe_fp5  →  qcom_qseecom pdev
        ↓
APP_START fingerpr  →  app_id = 4
APP_START focal32   →  app_id = 5
        ↓
qcom_scm_qseecom_app_send(app_id, req, rsp)  →  ret=0
```

TAs sourced from **modem_a** (`/mnt/modem_a/image/`, Android `firmware_mnt`).

## SEND probe summary (`qsee_ta_talk`)

| Observation | Detail |
|-------------|--------|
| SCM layer | Always `ret=0` for valid app_id 4 after START |
| cmd @ offset 0 | Response often starts with `ff ff ff ff` (−1) → TA error status |
| cmd=0 empty | Response all zeros |
| Layout status@0 + cmd@4 (`qsee_cmd_scan`) | First word becomes `0`, rest untouched (`0xee` fill) — partial / wrong frame |
| Interesting SEND count | **58** logged for fingerpr in one sweep |

Interpretation: we are **inside the TA**, not blocked by SCM. Need correct **message header** (and likely CREATE→INIT→PROBE order plus kernel SPI/IRQ sideband).

## HAL command names (for next RE)

Sequential string table order in `fps_hal.bin` (candidate enum):

| ID | Name |
|----|------|
| 0 | FF_CMD_TA_SEGMENTED_TRANS |
| 1 | FF_CMD_TA_CREATE |
| 4 | FF_CMD_TA_INIT |
| 10 | FF_CMD_TA_PROBE_DEVICE |
| 22 | FF_CMD_TA_CHECK_HEALTH |
| 43 | FF_CMD_TA_AUTNENTICATE |
| 44 | FF_CMD_SVC_GET_VERSION_INFO |

Authenticates on Android after enroll; templates live in TZ (`FF_CMD_TA_SYNC_TEMPLATE` / SVC template ops).

## Operational notes

1. **Reboot between full START sessions** if START returns `-22` / `result=0x…fed3` (−301): TA still resident.
2. **Name LOOKUP** of `"fingerpr"` still fails after START; use **numeric app_id** from START `data`.
3. **kCFI:** use `no_sanitize("kcfi")` wrapper around kallsyms `qcom_scm_qseecom_call` only (not whole module).
4. Soft-reboot clears DIE taint if a prior CFI oops occurred.

## Artifacts

| Path | Role |
|------|------|
| `artifacts/qsee-modules/qsee_probe_fp5.ko` | Force QSEECOM pdev |
| `artifacts/qsee-modules/qsee_ta_load.ko` | START only |
| `artifacts/qsee-modules/qsee_ta_talk.ko` | START + multi-format SEND |
| `artifacts/qsee-modules/qsee_cmd_scan.ko` | START + cmd 0..80 scan |
| `lab/fingerprint/oot-qsee/` | sources |
| `dumps/android/fp-tas-full-20260726/` | TA blobs |

## Re-test (clean boot)

```sh
scp artifacts/qsee-modules/*.ko fp5-pmos:/tmp/
ssh fp5-pmos 'sudo sh -c "
  mount -o ro /dev/disk/by-partlabel/modem_a /mnt/modem_a
  mkdir -p /lib/firmware/qsee
  cp /mnt/modem_a/image/fingerpr* /mnt/modem_a/image/focal32* /lib/firmware/qsee/
  insmod /tmp/qsee_probe_fp5.ko force_pdev=1
  insmod /tmp/qsee_ta_talk.ko ta_dir=/lib/firmware/qsee
  dmesg | grep -E \"START|SEND ok|finished\"
"'
```

## RE note from `fingerprint.default.so` (capstone)

`QSEECom_send_cmd` call site uses a **64-byte request** and **0x800-byte response**, with an 8-byte prefix:

```text
le32 0x00000205
le32 0x00000002
```

loaded from `.rodata` and stored at the start of the ion buffer; response begins at **+0x40** in the same allocation on Android. Lab probe `qsee_hdr_probe` sent that prefix via mainline SEND → still **status `0xffffffff`**. So either:

- more of the 64-byte slot must be filled by prior ion/session setup, or  
- `start_app`’s large shared buffer (`movk … #8,lsl#16` → sizes like `0x80040`) must be the same memory used for SEND (session-bound), not a fresh tzmem alloc.

## Next (M4b / M5 path)

1. Port Android **session buffer** layout (start_app buflen + ion offsets `0x78000` / `0x8040` seen in SO).
2. Sequence **CREATE → INIT → PROBE** with L2 `/dev/focaltech_fp` powered + IRQ listen.
3. **AUTHENTICATE** + template path.
4. fprintd/PAM → Phosh unlock.

## Roadmap

| Milestone | Status |
|-----------|--------|
| M0 L2 chardev/IRQ | Done |
| M1 QSEE LOOKUP | Done |
| M3 APP_START fingerpr/focal32 | Done |
| **M4 APP_SEND reaches TA** | **Done (framing TBD)** |
| M4b meaningful TA success status | In progress |
| M5 session unlock | Pending |
