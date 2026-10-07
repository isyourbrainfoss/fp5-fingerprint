# M4cp — large-plen CAP + DEAD + SRAM pull (post-OOM heap)

## Why (M4co)
Clean settle + meta plen `0x24` layout1 112×88 → still
`ff_sensor_data_array_new` OOM. Dim/PA/CANCEL exhausted. Residual short Null.

## Change (`try_android_frame_raw`)
1. settle clean (M4co)
2. **SYNC_CALIB** + light **SCAN** (no WMODE)
3. **one large-plen** layout1 CAP `plen=0x24+112*88*2` (in-band ion; HEALTH if sticky)
4. **CHECK_DEAD_PIXEL** `0x1020` once
5. **try_pull_image_ree_ta** (SRAM/XFER)
6. REPORT; **no L0** FDT thrash

## Hold expect
- `android-frame-raw M4cp`
- `afr L1-BIG CAP` — frame / dense nz / or OOM then HEALTH
- `afr DEAD` / `image-pull begin`
- wall still short; rem if any path works
