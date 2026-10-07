# Clear path: Fairphone 5 fingerprint on postmarketOS

Two different finishes — do not mix them.

| Finish | Meaning | Effort |
|--------|---------|--------|
| **A. Sensor life** | Power + IRQ + presses visible; lab can measure | **Near** (OOT module) |
| **B. Session unlock** | Finger unlocks Phosh | **Hard** (QSEE TAs + match) |

---

## A — Sensor itself (HW + SW life)

### Hardware (fixed on this phone)

| Pin / piece | Value | Source |
|-------------|--------|--------|
| Sensor | FocalTech **FT9362** in power button | Android oracle / repair PDF |
| IRQ | TLMM **GPIO 34** edge | Android `msmgpio 34 → focaltech_fp` |
| RESET | TLMM **GPIO 35** | Android DT |
| VDD | TLMM **GPIO 60** | Android DT |
| SPI | Often **TA-mediated** (not free REE spidev) | RE notes |

No extra board rework. Dual-boot keeps Android slot A for oracle.

### Software ladder (life)

```text
1. Kernel OOT: focaltech_fp_life.ko
   - gpiod on 34/35/60, /dev/focaltech_fp, request_irq(34)
2. listen=1  (sysfs)  — edges often gated until enabled (Android-like)
3. Press power-button FP → /proc/interrupts + irq_count move
4. Walkthrough app logs markers + IRQ series for correlation
```

**On this lab image the module is already installed:**

```text
/lib/modules/$(uname -r)/extra/focaltech_fp_life.ko
```

### One-shot (manual)

```sh
sudo /usr/local/share/fp5-fp-walkthrough/fp-walkthrough-prepare.sh
# expect: FP_PREPARE_OK, msmgpio 34 … focaltech_fp
grep focal /proc/interrupts
echo 1 | sudo tee /sys/class/focaltech_fp/*/listen
# press power button sensor; watch:
cat /sys/class/focaltech_fp/*/irq_count
```

### App path (preferred)

Open **FP Fingerprint Lab** → **Start**.  
The app runs `fp-walkthrough-prepare.sh` (sudo -n) then starts the coach + logger.

Success for **A**: session `irq-series.txt` shows **non-zero** `focaltech_fp` counts that rise on **TAP NOW** / **HOLD**, not only goodix (touchscreen).

### What A does *not* give

- Enroll / match / unlock  
- Ridge image frames (those need QSEE `focal32` / `fingerpr`)

---

## B — Working reader (unlock Phosh)

```text
[Phosh / PAM / fprintd]
        ↑
HAL-equivalent userspace (FocalTech protocol)
   ↙                    ↘
/dev/focaltech_fp          QSEE TAs: fingerpr + focal32
(A: we have this)          capture + templates + match
        ↑
   qcom SCM / TrustZone
```

| Milestone | Status (lab) |
|-----------|----------------|
| L2 chardev + IRQ | Done when prepare loads module |
| QSEE allowlist / TA load | Largely done (see `UNLOCK-ROADMAP.md`) |
| SPI SN / CAPTURE soft path | Partial / green rungs exist |
| ENROLL + AUTH with real frames | **Blocked** — needs finger-on-cue + SFS/listener gaps |
| fprintd / greeter glue | Not started until AUTH works |

Details: `UNLOCK-ROADMAP.md`, `BLOCKER-FT9362-QSEE.md`, `BLOCKER-ENROLL-AUTH.md`.

There is **no** mainline libfprint backend for this TZ path. iodeOS-class stacks keep Android HAL + `qseecomd`.

### Practical order toward B

1. Prove **A** every boot (prepare + walkthrough IRQ deltas).  
2. With L2 up, run enroll-hold / TA capture scripts **while** pressing on coach cues.  
3. Chase `frame raw` / `rem_seen` in dmesg (not empty AUTH).  
4. Only then wire Phosh unlock.

---

## Install / ship checklist

| Piece | Path |
|-------|------|
| Life module | `/lib/modules/$(uname -r)/extra/focaltech_fp_life.ko` |
| Prepare | `/usr/local/share/fp5-fp-walkthrough/fp-walkthrough-prepare.sh` |
| App | `/usr/local/bin/fp-walkthrough` |
| Sudoers (lab) | `/etc/sudoers.d/fp-walkthrough` — logger + prepare NOPASSWD |

Host reinstall:

```sh
./lab/fingerprint/walkthrough/install-on-fp5.sh
```

---

## Honesty

- **A** is the correct near-term goal of the walkthrough app.  
- **B** is a multi-milestone TEE project; empty IRQ series before prepare was a **missing driver**, not proof the sensor is dead.
