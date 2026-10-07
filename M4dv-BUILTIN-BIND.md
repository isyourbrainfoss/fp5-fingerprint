# M4dv — TA `g_builtin_buffer` bind (no finger, 2026-09-06)

## RE (HAL + TA, no phone tap)

Android HAL (`fingerprint.default.so`) does **not** use
`QSEECom_send_modified_cmd`. Imports are only `start_app` / `send_cmd` /
`shutdown_app`.

`ff_trustlet_client_open_session` calls `QSEECom_start_app(..., sb_length=
0x80040)` (`w3 = 0x40 | 0x8<<16`). `ff_builtin_user_buffer_lock` returns
`g_builtin_buffer->usr` at struct **offset +8**.

`ff_trustlet_client_exchange_message` `QSEECom_send_cmd(handle, usr,
send_len, resp, resp_len)` with `usr == g_builtin_buffer->usr` (asserted).
Message copy is skipped when the caller already built capture_ctx in that
ion. That matches our `sess+0x10` layout.

The **TA** (`focal32.b02`, `ff_ta/solution/trustlet_trans.c`) has its own
`g_builtin_buffer`:

- `trans == (ff_transfer_t *)g_builtin_buffer->ptr`
- `g_builtin_buffer->len >= sizeof(ff_transfer_header_t)`
- `trans->request.dlen <= FF_BUILTIN_MAX_DLEN` (else segmented join + malloc)
- `The response data should be placed in shared memory or another piece of malloc memory.`
- `ff_sensor_data_array_new[image.c:105]`

`fingerpr.b02` is GP `IMemSpace` (matcher TA, not the FT9362 image path).

`fingerpr.b02` (GP, not started on the enroll path): `IMemSpace_map failed`.

pmOS dumps never contain those bind strings. HLOS send buffer is TZ-visible
(logs come back) but the TA image path still `array_new`s — builtin ptr/len
are not the QSEECom start_app mapping.

## No-finger probe

`idle_native_cap=1 skip_enroll=1 skip_capture_path=1 shutdown_before_start=1`
(keep INIT). One 112×88 L1 nopa CAPTURE after INIT. Scan ion for BIND
markers.

| dmesg | Meaning |
|-------|---------|
| `ION-SCAN M4DV-BIND: BIND 'g_builtin...` | TA used builtin (unexpected) |
| `ff_sensor_data_array_new` / OOM ~28ms `rows=88` | bind still missing; CAPTURE mallocs |
| `FRAME RAW` / nz≠0 | bind worked without a finger |

Do not ask for a press. GTK Register stays parked.

## Hold result (SSH, no finger, 2026-09-06)

`dumps/pmos/fp-idle-m4dv/`. SHUTDOWN id 19 `ret=0`, START **id=0x14**, INIT **Base update success**. Then:

```
M4dv idle native 112x88 L1 nopa (no finger)
M4dv CAP er=0 ms=4 app_err=0xffffff37 frame=0 rows=88
M4DV-BIND-RES: ff_sensor_data_array_new[image.c:105]: 'Out of memory'
ION-SCAN M4DV-BIND hits=1  (only array_new — no g_builtin / IMemSpace / shared-memory line)
HEALTH final er=0
```

Native size is img_scan even idle. TA mallocs; builtin ptr was never set. A finger will not change that.

## Kernel vs Android mapping

Live 7.2 tree: `CONFIG_QCOM_TZMEM_MODE_GENERIC=y`, **SHM-bridge off**.
`qcom_tzmem_init_area` is a no-op; APP_SEND is 5-arg phys req/rsp only
(`/home/kb/src/linux-sc7280-7.2/drivers/firmware/qcom/qcom_scm.c`).
Android `qseecom-mod.ko` uses `qtee_shmbridge_register` on the start_app
ion, plus **`QSEECOM_IOCTL_SET_MEM_PARAM_REQ`** after load (not in our
strace; named from CAF qseecom + HAL `QSEECom_get_handle`). That is the
persistent map HAL later calls `g_builtin_buffer->usr`. HAL never uses
`send_modified_cmd` extra fds — not the missing piece.

Android also briefly attaches **app id 1** at `sb_length=0x840`, logs
`TA is successfully created`, then `QSEECom_shutdown_app` id 1. Device
path stays **focal32** on the 0x80040 handle.

Switching tzmem to `CONFIG_QCOM_TZMEM_MODE_SHMBRIDGE` needs a **kernel
rebuild + flash boot_b** (not a Register tap). Do not enable it until
fastboot/USB can flash; sc7180 is blacklisted, sc7280 is not.

**Do not retry REGION_NOTIFICATION.** Every VAL layout was SCM −22;
`QCOM_SCM_RW` hard-reset this SoC once. REGION is qseecom’s global
`secapp-region` carveout, not the 0x80040 session ion. Capture already
`ION HIT`s without it.

Remaining work is that start_app-style ion map into TA `g_builtin_buffer`,
not another CAPTURE flag.
