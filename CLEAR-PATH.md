# Clear path — Fairphone 5 fingerprint on pmOS

Twelve lab reviews (2026-08-25). Not a Phosh unlock promise.

**Sensor:** FT9362 in the **power button**. Match is **TrustZone** (`focal32` + `fingerpr`), not libfprint.

## What “working” means (pick one)

| Name | Meaning | Status |
|------|---------|--------|
| **Daily** | Unlock the phone you carry | **PIN on pmOS. FP on Android slot A.** |
| **Lab C** | Real enroll frames | `new fid` **once** + `frame raw` + `rem_seen=1` |
| **Lab D** | TA AUTH match | AUTH **without** `broken off` |
| **Phosh unlock** | fprintd + PAM + greeter | **Not this year** unless D is proven first |

Do not call press_signal or HEALTH “fingerprint working.”

## Consensus

1. L2 life + IRQ 34 + `press_signal` **work** (on **7.1.2**).
2. QSEE health without a finger **works** (`HEALTH er=0`, FDT, listeners).
3. HLOS `IMAGE PIXELS` is **synthetic/force**, not Android `frame raw`.
4. libfprint / mailing a `focaltech,fp` binding **cannot** identify fingers.
5. `spi0.0` is **Goodix touch**. FP SPI is **TA / SE14**. Pinmux 56–59 **reboots**.
6. Catcrafts FP6: same TEE wall. Do not copy their image.
7. Live kernel **`7.2.0-nfc-test+`**: 7.1.2 kos **will not load**. Rebuild OOT, **do not flash**.
8. `new fid` without rem is **not** a template. AUTH stays `broken off`.

## Three lanes (run in parallel, never mix 3 into unattended)

```
Human at power button? ──yes──► Lane 3: one supervised enroll + AUTH + cleanup
                     └──no───► Lane 2: health + cleanup only
Always ──► Lane 1: PIN on pmOS, FP on Android A
```

### Lane 1 — daily (honest product)

PIN/password on slot B. Fingerprint enroll/match on **slot A**. Hatch: host `fastboot --set-active=a` or 30s power. Never `qbootctl`. Never leave `listen=1`.

### Lane 2 — away (no finger)

```sh
sudo fp-finish-line prepare
sudo fp-finish-line health    # skip_enroll=1
sudo fp-finish-line cleanup
```

Want: `HEALTH final er=0`. Optional: coach press_signal / haptic tick **without** QSEE. Optional: rebuild kos for `7.2.0-nfc-test+`.

**Forbidden away:** `FP_RUN_ENROLL=1`, `fp-finish-line enroll`, pinmux/bitbang SE14, second APP_START when TA resident (−22).

### Lane 3 — you at the button (~5–10 min)

1. Confirm Android A still boots.
2. Coach TAP/HOLD (`M0-press-ok`). No register.
3. **FP Fingerprint Lab → Try register** (`enroll_force_cap=0`, `irq_means_down=1`). Short press/lift ×8, not one hold.
4. Same session AUTH.
5. **cleanup immediately** (`listen=0`, `rmmod` life + qsee).

Success grep:

```sh
grep -E 'new fid|frame raw|rem_seen=1|broken off' /var/tmp/fp-finish-line-*/enroll-hits.txt
```

Want `frame raw` + `rem_seen=1` + AUTH without `broken off`. Then **stop**. Do not start fprintd.

If `KEY_POWER` wedges: cleanup; else Android A / 30s hold.

## After a real template (only if lane 3 PASSes)

Still lab: persist SFS/RPMB, wrap AUTH in a daemon, *then* PAM. Distro fprintd is idle until AUTH match is real. Mainline will not grow a FocalTech QSEE driver.

## Do not

- Auto enroll / unattended `Try register`
- Flash to “fix” vermagic
- Pinmux gpio56–59 / bind `a98000.spi`
- Mail a fingerprint DT that claims identify
- Redefine Phosh unlock as this week’s finish line
