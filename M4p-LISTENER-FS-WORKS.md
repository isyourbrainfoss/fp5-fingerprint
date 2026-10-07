# M4p — FILE_SERVICE listener works end-to-end (2026-07-26)

## Goal

Register and service Android `FILE_SERVICE` (listener id **10**) so TA
`APP_SEND` can complete when TZ blocks on HLOS file proxy.

## Breakthrough

| Item | Status |
|------|--------|
| `REGISTER_LISTENER` VAL/RW/VAL | **OK** (`result=0`) |
| `APP_SEND` incomplete/block loop | **OK** — only `LISTENER_RESPONSE`, never re-send |
| Full probe with listener | **Completes**, phone stays up |
| Chip SN via SPI | **OK** with L2: `0b0a6944171209070800fb120d1e1dff22` |
| PRE_ENROLL challenge | **OK** (e.g. `0x771a64a9e5fdbfa7`) |
| HEALTH final | **er=0**, TA alive |
| CAPTURE | Still soft `Null pointer` (`err=0xffffff37`) |
| SFS `qsee_sfs_open(...fpdata...)` | Still `SFS_ERROR_GENERIC` (needs RPMB, not FILE_SERVICE) |

## Root-cause fixes this session

1. **`send_sep` used mainline `qcom_scm_qseecom_app_send`**, which does **not** resume
   `BLOCKED_ON_LISTENER` / incomplete (`type=0xee02`). That path hard-reset the SoC
   when FILE_SERVICE was registered. Fixed to use `app_send_ex`.
2. **FS response layout**: never clobber cmd@+0; status/result at **+4**
   (libdrmfs + observed qti_fp traffic).
3. **Observed FILE_SERVICE cmds on this unit** (fingerpr / qti_fp):
   - `0x0202` open (path@+4 → fd or `-errno` @+4)
   - `0x020f` mkdir/create
   - `0x021c` close (fd@+4)
4. **L2 required** for VIO/SPI (`focaltech_fp_life.ko`); without it,
   `device id: 0x0000` and probe fails.

## Evidence

- `lab/fingerprint/qsee-fs-traffic1.txt` — first blocked dumps
- `lab/fingerprint/qsee-l2-fs-full.txt` — full probe + SN + PRE + CAPTURE null
- Artifact: `artifacts/qsee-modules/qsee_fingerpr.ko`

## What still blocks unlock

| Gap | Why |
|-----|-----|
| **SFS / RPMB** | focaltech uses `qsee_sfs_open` for `/data/vendor_de/0/fpdata/*`. That is TZ SFS backed by **RPMB listener id 0x2000**, not FILE_SERVICE. |
| **CAPTURE null** | Still `ff_trustlet_capture_image` null / shared-buffer placement; may need correct ion capture_ctx or internal TA malloc path after SFS/init. |
| **ENROLL** | Still deferred (`skip_enroll=1`); historically SCM −22 kills TA. |
| **AUTH / Phosh** | Needs templates + working match path. |

## Safe load recipe

```sh
# After clean pmOS boot:
insmod /tmp/focaltech_fp_life.ko          # L2 clocks/power
insmod /tmp/qsee_fingerpr.ko \
  enable_fs_listener=1 \
  skip_enroll=1 \
  do_start=1                              # or force_f32_id=N force_fp_id=M
```

Module knobs: `fs_dump_only`, `max_sends`, `probe_level`, `fs_root=/var/lib/fp5-fp`.

## Next (priority)

1. **RPMB listener stub / real RPMB** so SFS open succeeds for serial + templates.
2. Refine FILE_SERVICE 0x02xx (read/write payloads once open returns real fds).
3. Revisit CAPTURE ion layout after SFS is green.
4. ENROLL with finger only when TA stays healthy post-CAPTURE.

## Safety

- Soft slot switch only (`fastboot --set-active=a|b`).
- No `qbootctl` on pmOS.
- Never re-`APP_SEND` after incomplete/block — only `LISTENER_RESPONSE`.
