# M4c4 — Minimal CAP under finger (no WMODE/SCAN) (2026-07-27)

## Root cause (M4c1 hold residual)

On IRQ edge, AFR did:

1. `SET_SPI_SPEED` → QES `[unknow]`, itype `0x0`
2. **`CFG_WORK_MODE FDT_DOWN` → QES `[esd]`** ← induced ESD
3. `START_SCANNING`
4. `CAPTURE flag=0x240` → residual **only FDT DAC** (`got fdt dac success`), `frame raw=0`, `pix~31`

Android enroll (oracle):

```
IRQ → QUERY itype=0x2 QES [finger touch]
    → CAPTURE×3 (~50ms) frame raw[0..2]
    → got finger touched → enrolling rem
    → FDT_UP; on leave → FDT_DOWN
```

No re-WMODE / START_SCANNING / SET_SPI_SPEED between IRQ and frame raw.

## Code

1. **`try_android_frame_raw`**: pure CAP×3 with flags `{0x240,0x41,0x200}` — no WMODE/SCAN/SPI.
2. **`try_force_image_path(ta_down)`**: same — skip prep under finger.
3. **Enroll sample loop under finger**: no `START_SCANNING` / mode-pulse before CAP.
4. **`esd_clear_loop`**: soft `ESD_HANDLE` first; L2 reset try1+; VDD last; less WMODE thrash.

## Hold

```sh
# After reboot (rmmod hangs — reboot to refresh .ko):
insmod focaltech_fp_life.ko auto_spiclk=1 spi_se_rate_hz=19200000
echo 1 > /sys/class/focaltech_fp/focaltech_fp/{vdd,spiclk,listen}
# on go:
insmod qsee_fingerpr.ko do_start=1 skip_enroll=0 skip_capture_path=1 \
  enroll_samples=6 sample_wait_s=10 enroll_force_cap=1 quiet_send=1 \
  enable_fs_listener=1 enable_rpmb_listener=1 enable_ssd_listener=1 \
  spi_bus_num=14 spi_capture_bps=8000000 capture_flag=576 force_img_mode5=0
```

## Success

`QES [finger touch]` then `frame raw` / `ANDROID FRAME RAW` / `rem_seen=1` / AUTH without broken off.
AFR residual must **not** show WMODE-induced `[esd]` before first CAP.
