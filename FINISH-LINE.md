# Fingerprint finish line (honest)

Updated: 2026-09-16. Pin live extra in `STATUS.md` (not chat memory).

## Two different finish lines

| # | Goal | Status now |
|---|------|------------|
| **A** | Sensor life + press visible | **Done** (M0-press-ok; 2026-09-05 `FINGER_DOWN` itype=0x2) |
| **B** | TA up + capture path + HEALTH | **Done** (`HEALTH er=0`, reattach f32=4, START −22) |
| **C** | Enroll template + real frame | **Done in RAM** (2026-09-16 Register rem 19→0, avgv ~300). Persist **unproven** (no RPMB WRITE `0x3`). |
| **D** | AUTH match then Phosh unlock | **Not done** — 2026-09-16 Match press was itype `0x12` (inv) without `FDT_DOWN_DETECT`. Disk extra now arms mode 9. No Phosh unlock. |

B health smoke (this unit, no finger):

- `HEALTH final er=0`
- FDT DAC success, sensor status ok  
- IMAGE PIXELS pulled (synthetic/force path)  
- FS listeners registered (id 10 + 8192)  
- Evidence: `/var/tmp/fp-finish-line-*/dmesg-smoke.txt`

## Commands on the phone

```sh
# Always safe
sudo fp-finish-line prepare
sudo fp-finish-line health

# YOU must press/lift power-button FP when prompted (~1 min)
sudo fp-finish-line enroll
# or
sudo fp-finish-line full

sudo fp-finish-line cleanup
```

App is **FP Fingerprint** → **Start** (L2) → **Register** explains the wall
(no TZ insmod). Close unloads. PIN on pmOS; fingerprint unlock on Android slot A.

## Success criteria for C

```sh
# after enroll
grep -E 'new fid|frame raw|rem_seen=1|ENROLL' /var/tmp/fp-finish-line-*/enroll-hits.txt
```

Want: `new fid (= N)`, `frame raw`, `rem_seen=1`.

## What is *not* the finish line yet

- Phosh “unlock with fingerprint” (fprintd + PAM + stable AUTH)  
- Leaving enroll modules loaded unattended  

## Safety

- Stop/cleanup: listen=0, `rmmod` life + qsee_fingerpr  
- If KEY_POWER wedges: Android slot A, or hard power cycle  
- Root is a 3.3G partition. App Start prunes old `/var/tmp/fp-walkthrough/session-*`. Check `df -h /` before large dumps.  

## Re-install helpers

```sh
# from host repo
scp artifacts/qsee-modules/qsee_fingerpr.ko lab/fingerprint/fp-finish-line.sh fp5-pmos:/tmp/
ssh fp5-pmos 'sudo install -m755 /tmp/fp-finish-line.sh /usr/local/bin/fp-finish-line
  sudo install -m644 /tmp/qsee_fingerpr.ko /usr/local/share/fp5-fp-walkthrough/'
```
