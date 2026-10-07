# M4t — ENROLL alive + fid generation (no TA kill) (2026-07-27)

## Breakthrough

With L2 listen + FDT soft-success + finger-wait on L2 IRQ, **ENROLL 0x2001 no longer SCM-kills focal32**.

| Step | Result |
|------|--------|
| FDT CAPTURE | `fdt_ok=1`, `err=0` |
| Finger wait | `L2 IRQ rose … finger` / `fingerwait done seen=1` |
| PRE_ENROLL | challenge non-zero |
| ENROLL samples 0–7 | **each `er=0 err=0x0`**, `new fid (= …) generated` |
| POST_ENROLL | `er=0` |
| AUTH | `er=0 err=0x0` |
| HEALTH final | `er=0` (TA alive) |

Evidence: `{SCRATCH}/fp-enroll-or-unlock.log`, `lab/fingerprint/qsee-enroll-finger-run.txt`.

## Update — enroll CAPTURE soft-success (mode 5 + power)

After forcing power-mode 1 + work-mode 5 before samples:

| cap-enroll-0..7 | **all `er=0 err=0x0`** (Null / 0xffffff37 **gone**) |
| ENROLL samples 0..7 | all `er=0 err=0x0` + `new fid` |
| AUTH | `er=0` but `enrollment is unexpected broken off` |

## Gap (why not full match yet)

1. Each ENROLL sample still generates a **new** fid (not rem countdown on one enrollment).
2. Mid-ENROLL TA log: `configure spi mode failed(0)`, intermittent `set chip vio 1.8v fail`.
3. AUTH: `g_enrollImage is empty`, `tplNodeList is empty` — no finished template.
4. Finger often `release` / `gesture leave` at sample time (need held finger / type 0x2).

So criterion **3a (match)** is **not** met yet; criterion **2** and ENROLL **protocol** path are green.

## Repro

```sh
# L2 listen
echo 1 | sudo tee /sys/class/focaltech_fp/focaltech_fp/listen
echo 1 | sudo tee /sys/class/focaltech_fp/focaltech_fp/vdd
# Touch sensor during wait (or rely on ambient IRQ edges)
sudo rmmod qsee_fingerpr 2>/dev/null
sudo insmod /tmp/qsee_fingerpr.ko \
  enable_fs_listener=1 enable_rpmb_listener=1 \
  skip_enroll=0 enroll_needs_finger=1 wait_finger_s=25 \
  spi_bus_num=14 do_start=0 force_f32_id=4 force_fp_id=5
sudo dmesg | grep -E 'fdt_ok|fingerwait|ENROLL sample|new fid|AUTH er|HEALTH final'
```

## Next

1. Hold finger **down** during ENROLL (not just IRQ edge); gate samples on interrupt type 0x2.
2. Fix image array n>0 so enroll CAPTURE supplies raw frames (img calib / dims / mode).
3. AUTH match once templates non-empty; then Phosh unlock glue.

## Related

- M4s — CAPTURE FDT success  
- BLOCKER-ENROLL-AUTH.md — earlier finger requirement (partially lifted for protocol ENROLL)
