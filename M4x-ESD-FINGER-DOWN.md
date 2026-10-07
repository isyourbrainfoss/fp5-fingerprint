# M4x — Finger-down seen; ESD blocks enroll samples (2026-07-27)

## Disk
Root was 100% full (journal ~645MB). Vacuumed to **~600MB free**. Modules stay on `/tmp` (tmpfs).

## Progress with user finger present
- **Live finger-down once:** `FINGER_DOWN itype=0x2` (sample 6 of hold run #3).
- CAP after that: vio success, `ta_down=1`, but **no** `frame raw` / `enrolling... rem`.
- Android oracle: **REE** `ft93xx:img: frame raw` then `transmit_image` then TA `enrolling...`.
  Pure TA `CAPTURE 0x1013` does not emit `frame raw` logs.

## ESD
- After capture path / ENROLL, TA `query event state: [esd]` (latest, not stale).
- ENROLL logs `may be is esd, need to reset ic`.
- Mid-loop QEV polling worsened sticky ESD; quiet hold (IRQ-only wait) avoids spam.
- `l2_reset_ic` + warm CAP restores **vio** but event state often stays `[esd]`.
- Pre-ENROLL + post-ENROLL L2 reset + VDD cycle added.

## Repro (user)
```sh
scp artifacts/qsee-modules/qsee_fingerpr.ko lab/fingerprint/run-enroll-hold-finger.sh fp5-pmos:/tmp/
ssh fp5-pmos 'sudo sh /tmp/run-enroll-hold-finger.sh'
# Hold power-button FP firmly during *** HOLD FINGER NOW ***
```

## Next
1. Clear ESD so FDT reports `[finger touch]` reliably under hold.
2. Bridge REE image path or find TA cmd that yields `got finger touched` + rem countdown.
3. Then AUTH match / Phosh unlock glue.
