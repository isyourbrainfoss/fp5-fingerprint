# M4dw — shm_bridge_create on session ion (no finger, 2026-09-06)

GENERIC tzmem does not register SHM-bridge. Android qseecom does
`qtee_shmbridge_register` on the start_app ion. The create SCM is
**exported** on this 7.2 kernel — no flash.

Do **not** call `qcom_scm_shm_bridge_enable` (global; rmtfs reset on
blacklisted SoCs). Only `qcom_scm_shm_bridge_create` on `SESS_BIG`.

Then same idle 112×88 CAP as M4dv.

| dmesg | Meaning |
|-------|---------|
| `M4dw shm_bridge_create … ret=0` then `FRAME RAW` | bridge was the bind |
| `ret=0` then still `array_new` OOM | create is not enough without enable / QSECOM heap |
| `ret!=0` | TZ rejected create without enable — needs kernel SHMBRIDGE mode + boot_b |

No press. GTK Register stays parked.

## Hold result (SSH, no finger, 2026-09-06)

`dumps/pmos/fp-idle-m4dw/`

```
START focal32 ret=0 id=0x15
M4dw shm_bridge_create pa=0xf0600000 sz=0x81000 ret=0 handle=0xe54c8637
*** Base update after INIT_DEV ***
M4dv CAP er=0 ms=4 app_err=0xffffff37 rows=88
ff_sensor_data_array_new[image.c:105]: Out of memory
M4dw shm_bridge_delete handle=0xe54c8637
HEALTH final er=0
```

TZ **accepted** create without `shm_bridge_enable`. CAPTURE still mallocs.
Create-on-session is not the Android QSECOM-heap bind. Next: kernel
`CONFIG_QCOM_TZMEM_MODE_SHMBRIDGE` + flash boot_b when fastboot exists.
Do **not** call `shm_bridge_enable` from OOT (global; rmtfs reset risk).
