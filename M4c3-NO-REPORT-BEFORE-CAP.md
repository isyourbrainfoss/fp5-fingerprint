# M4c3 — no REPORT_EVENT before CAP under finger (2026-07-27)

## Why
M4bg measured: REPORT_EVENT 0x2 before CAP **fakes** finger-down and skips
`ft93xx_img_scan` / `frame raw`. Enroll sample loop still injected REPORT
after AFR failed (M4c1/M4c2 holds: CAP err=0 but residual only FDT, pix~31).

## Code
1. Remove REPORT_EVENT before enroll sample CAP under finger.
2. Remove REPORT from force-image SPI-retry under finger.
3. AFR first CAP dumps residual when no frame raw (FDT-only/ESD diag).
4. ESD clear: SLEEP → FDT_DOWN + soft CAP (M4c2).
5. Sticky ESD after IRQ: wait more, do not CAP yet.

## Hold after reload
```sh
# After reboot (rmmod hangs this module — reboot to refresh):
insmod focaltech_fp_life.ko auto_spiclk=1 spi_se_rate_hz=19200000
echo 1 > /sys/class/focaltech_fp/focaltech_fp/{vdd,spiclk,listen}
# on go:
insmod qsee_fingerpr.ko do_start=1 skip_enroll=0 skip_capture_path=1 \
  enroll_samples=6 sample_wait_s=10 enroll_force_cap=1 quiet_send=1 \
  enable_fs_listener=1 enable_rpmb_listener=1 enable_ssd_listener=1 \
  spi_bus_num=14 spi_capture_bps=8000000 capture_flag=576 force_img_mode5=0
```

## Success
`frame raw` / `ANDROID FRAME RAW` / `rem_seen=1` / AUTH without broken off.
