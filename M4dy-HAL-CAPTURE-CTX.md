# M4dy — Android 0x24 capture_ctx (no cols/rows)

HAL RE (`fingerprint.default.so` `ff_trustlet_client_open_session` /
`__svc_capture_image`):

- `g_builtin_buffer->usr = ion_sbuffer+0x10` (ptr at +8)
- `QSEECom_send_cmd(ion, 0x78000, ion+0x78000, 0x8040)` — same split we use
- **0x24 request is not cols/rows/nbytes.** Image header is **after** CAP
  at `usr+0x2c` (cols `+0x30`, rows `+0x34`, nbytes=`cols*rows*2`)

| off | Android field |
|-----|----------------|
| +0x00 | enable_hw_reset |
| +0x0c | burst (`device.burst_acquisition_num`, live 3) |
| +0x10 | 1 |
| +0x14 | frame index 0..2 |
| +0x1c | flag (`0xC0040002` enroll thread; factory `0xE0000240`) |
| +0x20 | 1 factory / 0 event thread |

Lab layout1 (cols/rows/nbytes in the 0x24) is what made TA `array_new` 112×88.

## Hold result (2026-09-06, idle, no finger)

Fresh START id=4, INIT **Base update success**, then HAL overlay CAPTURE:

```
M4dy HAL capture_ctx burst=1 flag=0xE0000240 (no cols/rows)
dev-cap-m4dy-halctx MARK: sensor raw data scanning totally consumed time 21 (0 + 21) ms.
dev-cap-m4dy-halctx MARK: frame raw[0]: avgv = 865, minv = 745, maxv = 921.
M4dy CAP er=0 ms=36 app_err=0xfffffff5
M4DY-ION ion u16[600..1100]=0
M4DY-ION no ADC plane in ion (TA frame stayed TZ-side)
array_new=0  oom=0  HEALTH final er=0
```

Repeatable across three idle runs (`dumps/pmos/fp-idle-m4dy/`, `fp-idle-m4dy2/`,
`fp-idle-m4dy3/`). Android finger frames in `dumps/android/fp-enroll-live-*`
are `avgv ~280–510` with wide min/max; idle 865±20 is empty-sensor scan.

`usr+0x2c` is a **header** (`nbytes=0x28c4`, 36×144), not the ADC plane.
HLOS ion `0x78000` has **zero** u16 in `[600,1100]`. TA reused the INIT
image buffer (no second `array_new`). Copy-out into QSEECom ion is still
missing — that is the leftover `g_builtin_buffer` gap, not another layout.

`0xfffffff5` is a soft app_err (also seen on PROBE); TA still logged
`frame raw[0]`. Factory flag `0xE0000240` skips wait-for-finger.

## Code (this tree)

- `capture_ctx_fill_hal()` / `cap_hal_overlay=1` (default)
- Enroll sample CAPTURE uses HAL overlay (`0xC0040002` under finger,
  `0xE0000240` idle)
- `idle_native_cap=1` skips post-FDT force-image

Do **not** flash `TZMEM_MODE_SHMBRIDGE` for img_scan — HAL overlay unblocked
that. SHMBRIDGE is only if we need the 112×88 plane in HLOS ion.

GTK Register stays parked. A press on the SSH enroll helper (HAL overlay,
`skip_init_dev` off) is the first finger that can progress rem.
