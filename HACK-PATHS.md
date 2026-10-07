# Fingerprint “hack” paths for FP5 (honest)

## What a finger session on Android buys
- Proves GPIO34 IRQ fires on touch
- logcat shows HAL → QSEE TA command flow (`focal32` / `fingerpr`)
- Confirms enroll UI path without needing a completed enroll

## Realistic hack tiers

### Tier 0 — Impossible short-term (do not expect)
- Drop FocalTech Android `.ko` on mainline 7.1.2 (vermagic/CFI/ABI)
- “Just enable qseecom” on mainline → still need signed TAs + userspace HAL

### Tier 1 — Research / documentation (we can do)
- Full Android finger oracle (this capture)
- DT for GPIOs 34/35/60 on pmOS for completeness
- Catalog TA blob sizes/hashes from `firmware_mnt`

### Tier 2 — Heroic RE (months, maybe never product-quality)
- Reverse `fingerpr`/`focal32` QSEE protocol over `/dev/qseecom` or SCM
- Or find if FT9362 has an undocumented **non-secure image dump** mode (rare for side FP + TEE designs; usually match-in-TA only)
- Run **Android userspace + TZ** in dual-boot (Android works) while using pmOS for daily — not “pmOS fingerprint”, but usable unlock on A

### Tier 3 — Practical “cool” compromise
- **Boot Android for unlock / banking**, pmOS for everything else (slot switch)
- Or lockscreen PIN on pmOS; accept no biometric

## Cool but real: dual-boot biometric
Fingerprint **already works on slot A**. The “hack” with best ROI is a one-button switch:
`fastboot --set-active=a|b` (never qbootctl on pmOS).

## If RE ever starts
1. Finger session logs (TA names, ioctl patterns)
2. Pull `fingerpr.mdt` + `focal32.mdt` (need avb/vendor mount)
3. Strings/disasm for command IDs
4. Compare to other FocalTech QSEE devices (community)

No public mainline FT9362+QSEE port for FP5 is known as of this lab.
