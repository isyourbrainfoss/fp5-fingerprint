# M4w — Enroll ESD recovery + img calibration on pmOS (2026-07-27)

## Findings (live fp5-pmos)

1. **HLOS IRQ is quiet during TA FDT enroll**  
   After TA is in FDT_DOWN_DETECT, `irq_count` barely moves without a press.  
   Gating CAP on IRQ edge caused 8× `skip CAP`. **Fix:** `enroll_force_cap=1`
   (default) — wait optional IRQ then always CAPTURE.

2. **ENROLL start triggers ESD**  
   TA log: `may be is esd, need to reset ic` / `query event state: [esd]`.  
   First enroll CAPs then show `set chip vio 1.8v fail` + `configure spi mode failed`.

3. **Do not re-INIT_SPI/PROBE after ENROLL**  
   That path regressed to CAP `0xffffff37` and AUTH/HEALTH **SCM −22** (TA dead).

4. **Recovery that works**  
   - L2 IC reset via `/dev/focaltech_fp` ioctls (`l2_reset_ic` after ENROLL)  
   - Warm-up CAPTURE(s) until `vio 1.8v success` (`enroll warmup`)  
   - Skip pre-ENROLL SYNC/PWR by default (`enroll_resync=0`)

5. **First pmOS img calibration lines** (post-warmup WMODE 5):
   ```
   img calibration->try: 1, AVG DAC_Temp:120, ...
   img calibration->try: 2, AVG DAC_Temp:121, ...
   ```
   Previously never observed on pmOS (only FDT). Still no
   `successfully initialized` / `Base update success` / `frame raw` without finger.

6. **Without finger:** ENROLL once + force CAP + HEALTH er=0; AUTH still
   `enrollment is unexpected broken off` / `rem_seen=0` — criterion 5.

## Module knobs

| Param | Default | Role |
|-------|---------|------|
| `enroll_force_cap` | true | CAP without IRQ edge |
| `sample_wait_s` | 8 | wait window per sample |
| `enroll_resync` | false | avoid SYNC that breaks VIO |
| (internal) | | post-ENROLL `l2_reset_ic` + warm-up CAPs |

## Repro

```sh
scp artifacts/qsee-modules/qsee_fingerpr.ko lab/fingerprint/run-enroll-hold-finger.sh fp5-pmos:/tmp/
ssh fp5-pmos 'sudo sh /tmp/run-enroll-hold-finger.sh'
# Hold power-button FP on each *** PRESS/LIFT finger sample N ***
dmesg | grep -E 'img calib|frame raw|enrolling|rem_seen|vio 1.8v success|warmup|l2_reset'
```

## Artifacts

- Source: `lab/fingerprint/oot-qsee/qsee_fingerpr.c`
- Module: `artifacts/qsee-modules/qsee_fingerpr.ko` vermagic 7.1.2
- Logs: `{SCRATCH}/fp-warmup.out`, `fp-esd-reset-full.log`
