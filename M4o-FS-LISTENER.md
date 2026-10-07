# M4o — FILE_SERVICE listener attempt (2026-07-26)

## Goal

Register Android `FILE_SERVICE` (listener id **10**) so TA `qsee_sfs_open` /
HLOS file proxy can work, unblocking CAPTURE and eventually ENROLL.

## What we implemented

In `qsee_fingerpr.c`:

| Piece | Status |
|-------|--------|
| `listener_register(FILE_SERVICE=10)` via TZ `owner=50,svc=2,cmd=1` | **SCM −22** |
| Same with `cmd=6` (smcinvoke variant) | **SCM −22** |
| Arginfo with `QCOM_SCM_RW` for sb phys | **Hard-reset SoC** (reverted to all-VAL) |
| Shared buffer 64 KiB tzmem | Allocated |
| `app_send_ex` incomplete/blocked loop | Present |
| `handle_fs_listener` open/read/write/close + path map | Present (unused until register works) |
| Path rewrite `/data/vendor_de/0/fpdata/*` → `/var/lib/fp5-fp/*` | Present |

Module params:

```
enable_fs_listener=0   # default off (register fails)
fs_root=/var/lib/fp5-fp
skip_enroll=1
```

## Evidence

```
REGISTER_LISTENER id=10 pa=0xff200000 len=0x10000 ret=-22
REGISTER_LISTENER(cmd6) ret=-22
listener register failed
… TA path still works: SN present, CAPTURE still Null pointer, HEALTH ok
SFS still: qsee_sfs_open(...ft_fp_serial_id.bin) = SFS_ERROR_GENERIC
```

`lab/fingerprint/qsee-fs-listener2.txt`

## Why register fails

1. Mainline `qcom_scm` exposes QSEECOM APP_SEND/START; **listener register is not
   validated/used in upstream** and this firmware returns **EINVAL (−22)**.
2. Vendor stack uses `/dev/qseecom` + ion + `QSEECOM_IOCTL_REGISTER_LISTENER_REQ`
   and a long incomplete-cmd loop in `drivers/misc/qseecom.c`.
3. pmOS has **`qcom_qseecom` platform device only** — no `/dev/qseecom`, no
   `smcinvoke` node.

So SFS is not just “missing userspace daemon”: **the HLOS cannot register
listeners without vendor qseecom (or a correct smcinvoke binding).**

## Next options (priority)

1. **Port / adapt vendor `qseecom.ko`** for this kernel (or smcinvoke-based
   listener API) so `QSEECom_register_listener` works; then run a tiny
   `libdrmfs`-compatible handler (or our `handle_fs_listener`).
2. **Reverse smcinvoke** on this SoC if present in another firmware path.
3. **Android-slot dump**: pull `libdrmfs.so` + strace `fs_start` for exact
   shared-buffer layout once register works.
4. Interim: keep `skip_enroll=1`; SPI + PRE_ENROLL remain the green path.

## Safety

- Never use `QCOM_SCM_RW` for experimental listener/region args on this unit
  without isolation (watchdog/reset observed).
- Prefer all-VAL SCM args; treat −22 as soft fail.

## Related

- `M4n-SFS-AND-LISTENERS.md` — diagnosis
- `M4m-CAPTURE-PATH.md` — CAPTURE null without SFS
- `M4l-ENROLL-PATH.md` — ENROLL kills TA
