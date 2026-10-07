# Slash-goal text — Fairphone 5 fingerprint “life” on postmarketOS

Paste everything in the fenced block below after `/goal` (optionally add `--budget …`).

---

```
Fairphone 5 dual-boot lab (repo /home/kb/repos/grok_build/fp5): advance fingerprint from “documented QSEE dead end” to measurable hardware/software life on postmarketOS (slot B). Full Android-style unlock is NOT required for goal completion. “Somewhat working” means verifiable progress up the ladder below, with evidence on disk.

## Context (do not re-discover from zero)
- Sensor: FocalTech FT9362 (power-button), DT name focalfp_ft9362, compatible focaltech,fp
- GPIOs (TLMM): IRQ=34, RESET=35, VDD=60
- Android: /dev/focaltech_fp, HAL vendor.focaltech…fingerprint@1.0-service, TAs focal32 + fingerpr under /vendor/firmware_mnt/image/
- Enroll/unlock/fail captures: dumps/android/fp-enroll-first-*, fp-unlock-*, fp-unlock-fail-*, fp-oracle-extra-*
- Lab notes: lab/fingerprint/BLOCKER-FT9362-QSEE.md, NEXT-AND-TRICKS.md, HACK-PATHS.md, pmos-fp-gpio-poke.py
- Dual-boot safety: NEVER qbootctl on pmOS. Slot switch only via fastboot --set-active=a|b. Prefer hybrid boot_b for NFC; do not flash boot-nfc-full.img. Rootfs on SD. SSH host: fp5-pmos (WiFi). Magisk su on Android when on slot A.
- Architecture: userspace ioctl(/dev/focaltech_fp) + QSEECom TAs; SPI may be kernel and/or TA-mediated (FF_CMD_*_SPI_*). Match/enroll is TA-bound; image capture appears partially in HAL logs.

## Definition of “somewhat working” (accept ANY complete ladder rung as partial success; goal COMPLETE only if L2 or higher is met with evidence)

L0 — Oracle complete (already largely done): Android enroll + unlock success + unlock fail logs; GPIO map; HAL/ko strings; TA hashes. Refresh only if missing.

L1 — Pin life on pmOS: scripted control of VDD/RESET; document IRQ 34 level/edges with and without reset sequences; report whether passive finger press changes IRQ without a driver. Evidence: lab/fingerprint/pmos-gpio-*.txt logs + short README update.

L2 — Kernel foothold (MINIMUM for goal COMPLETE): mainline-compatible platform driver (out-of-tree module OK, clang/CFI/BTF match phone 7.1.2 if loading on device) that:
  (a) binds a DT node for FT9362 with gpios 34/35/60,
  (b) creates /dev/focaltech_fp or equivalent chardev,
  (c) implements reset + power + IRQ registration,
  (d) exposes sysfs or debug logs showing probe success and IRQ counts when userspace enables “listen” (ioctl stub OK),
  (e) loads on live pmOS without breaking boot.
Evidence: dmesg probe lines, /proc/interrupts focal/gpio34 activity after an enable path, module path under artifacts/ or lab/fingerprint/, DT snippet, and a one-shot enable script. Optional: document that finger press increments IRQ only after enable (session-gated, as on Android).

L3 — Electrical identity (stretch): SPI or register probe that reads a chip ID / non-zero status without QSEE, OR prove SPI is TA-only with evidence from Android strace/ioctl trace. Evidence file required either way.

L4 — Stretch / not required for COMPLETE: any path toward templates or match (QSEECOM on mainline, TA load, or documented hard stop with exact missing kernel/SCM pieces). Prefer documenting the hard stop over claiming unlock.

## Non-goals
- Do not claim fprintd unlock without a working match path.
- Do not brick: no qbootctl on pmOS; no careless flash of boot_a or userdata; do not flash full custom kernels without recovery image ready.
- Do not spend the whole budget re-running NFC/audio unless blocked on SSH/boot.

## Working method
1. Prefer pmOS SSH; use Android only if new oracle is needed; return to slot B when done.
2. Small measurable commits/artifacts under lab/fingerprint/ and artifacts/; update BLOCKER or NEXT-AND-TRICKS with results.
3. If device needs fastboot, ask the user once for Vol Down+Power; do not spin forever.
4. When claiming COMPLETE, cite concrete paths and commands a verifier can re-run (insmod, dmesg grep, cat /proc/interrupts, optional finger press).

## Acceptance for COMPLETE
- L2 criteria (a)–(e) all true with reproducible evidence on this dual-boot unit, OR L3 chip-ID success with the same bar.
- STATUS-PMOS.md or lab/fingerprint/README.md updated with “FP life: L2 (or L3)” and how to test.
- Honest remaining gap to full unlock stated in one paragraph (QSEE TA).

## If blocked
Pause with: exact error, last successful rung (L0–L4), files written, and the single next experiment. Prefer shipping L2 over infinite L4 research.
```

---

## Why this goal is shaped this way

| Design choice | Reason |
|---------------|--------|
| Ladder L0–L4 | “Somewhat working” is ambiguous; the ladder is verifiable |
| COMPLETE at **L2** | Achievable kernel foothold, not fantasy fprintd |
| L3/L4 stretch | Room to push without failing the goal |
| Explicit non-goals + brick rules | Protects dual-boot / NFC work |
| Evidence on disk | `/goal` verification needs re-runnable claims |

## Short one-liner variant

If the UI prefers a short objective:

```
/goal FP5 pmOS fingerprint life: ship OOT platform driver + DT (IRQ34/RST35/VDD60) creating /dev node, reset/power/IRQ, load on live unit; prove IRQ activity when listen enabled. Full QSEE unlock out of scope. Lab: lab/fingerprint/, dumps/android/fp-*. See lab/fingerprint/GOAL-fp-life.md for full ladder L0–L4 and safety (no qbootctl).
```
