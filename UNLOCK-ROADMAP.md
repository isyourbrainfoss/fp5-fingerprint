# Goal: unlock the pmOS session with the power-button FP

**Success bar (user):** finger unlocks the Phosh/GNOME (or login) session on postmarketOS — not “IRQ moves”, not Android-only.

**Date started:** 2026-07-26

---

## Time: human vs agent (honest)

| Kind | What it is |
|------|------------|
| **Agent time** | Parallel research, kernel patches, RE of HAL/TA strings, build/load loops, docs — **hours to days of wall-clock** per milestone when the unit is online. This is the “super power.” |
| **Human time** | Reboots, fastboot, “finger on sensor now”, risk calls, installing another OS for oracle — **minutes per session**, not months of sitting. |
| **Calendar / uncertainty** | Not the same as labor. Some steps fail only after a full experiment (TA rejects load, SCM fault). That stretches **calendar** even when agents work hard. |

“Months” in earlier chat meant **calendar uncertainty for full session unlock if TEE/policy fights us** — not “a human must code full-time for months.” With aggressive agents + your dual-boot lab, **early milestones (QSEE version, allowlist, first SCM traffic)** can move in **agent sessions**. **Full greeter unlock** still has unknown blockers (signed TAs, missing Android qseecom ABI on mainline).

Agents compress **implementation and RE**. They do not guarantee TrustZone accepts `fingerpr` from mainline.

---

## Why iodeOS / Ubuntu Touch work

They keep an **Android-class stack**: vendor kernel or blobs + `qseecomd` + HIDL fingerprint + signed TAs `focal32` / `fingerpr`.  
pmOS mainline has **SCM**, config `QCOM_QSEECOM=y`, but at boot:

```text
qcom_scm: qseecom: untested machine, skipping
```

`fairphone,fp5` is **not** on the mainline allowlist (list is mostly WoA laptops). Even if allowlisted, mainline today only wires **`qcom.tz.uefisecapp`** — **not** Android’s `/dev/qseecom` + `QSEECom_start_app("fingerpr")`.

---

## Stack we must rebuild for session unlock

```text
[Phosh / GDM / PAM]  ← session unlock
        ↑
[fprintd or custom biometric daemon]
        ↑
[FocalTech HAL-equivalent userspace]
   ↙              ↘
/dev/focaltech_fp    QSEE load+invoke (fingerpr / focal32)
(L2: we have this)   (missing: Android-style TA loader)
        ↑
   qcom SCM / TZ
```

---

## Milestones (agent-driven; human for device ops)

| # | Milestone | Done when | Risk |
|---|-----------|-----------|------|
| M0 | L2 foothold | `/dev/focaltech_fp` + IRQ34 | **Done** |
| M1 | QSEE reachable | Force pdev + LOOKUP works; **keymaster64 + uefisecapp live** | **Done 2026-07-26** — see `M1-QSEE-RESULTS.md` |
| M2 | Mainline qseecom device up | `qcom_qseecom` + uefisecapp client bound | **Done** (with M1) |
| M3 | TA load | APP_START `fingerpr`→**id 4**, `focal32`→**id 5** | **Done** — `M3-TA-LOAD-RESULTS.md` |
| M4 | APP_SEND | SEND works; framing partially RE’d | **Done** — `M4-SEND-RESULTS.md` |
| M4b | Meaningful TA response | **`focal32` version `v2.5.3.0-cdf0f80-230614@0928`** | **Done** |
| M4c | Command map + fingerpr | `0x205/0x2` info + `0x1011` entry; fingerpr still −1 | **Partial** — `M4c-EXPLORE.md` |
| M4d | fingerpr open (ion+FF_CMD_TA_*) | HAL ion `0x78000/0x8040` + real TA cmds; all still `0xffffffff` | **Hard stop partial** — `M4d-FINGERPR-OPEN.md` |
| M4j | TZ SPI open | nested JSON SYNC; chip SN on pmOS | **Done** — `M4j-QSEE-SPI-OPEN.md` |
| M4k | AUTH path | SET_GROUP 0 templates; PRE_ENROLL later | **Partial** — `M4k-AUTH-PATH.md` |
| M4l | ENROLL | PRE OK; ENROLL SCM −22 kills TA | **Blocked** — `M4l-ENROLL-PATH.md` |
| M4m | CAPTURE without ENROLL | soft Null pointer; TA stays alive | **Partial** — `M4m-CAPTURE-PATH.md` |
| M4n | SFS/listeners | `qsee_sfs_open` → SFS_ERROR_GENERIC; mainline no listeners | **Next** — `M4n-SFS-AND-LISTENERS.md` |
| M4e | Android oracle + seq mirror | Oracle: probe OK on A; pmOS fingerpr still ffffffff | **Partial** — M4e-ANDROID-ORACLE.md |
| M4f | focal32 ion device cmds | SYNC_CFG/HEALTH err=0; SPI blocks PROBE | **Partial** — M4f-FOCAL32-DEVICE-CMDS.md |
| M4g | SPI bus 14 / SE14 HLOS resources | Clocks+ICC+core+reset; INIT_SPI still −5 | **Hard stop partial** — M4g-SPI-SE14.md |
| M4h | REE geni SE14 | un-reserve OK; geni bind hang | **Abort** — M4h-SPI-SE14-REE.md |
| M4i | Android strace + pinmux-only | Android TZ SPI confirmed; pinmux module watchdog reboot | **Abort pinmux** — M4i-ANDROID-STRACE-AND-PINMUX.md |
| M4j | TZ SPI open path | **DONE** nested JSON SYNC → INIT_SPI/PROBE/INIT_DEV err=0; SN read | M4j-QSEE-SPI-OPEN.md |
| M4k | SET_GROUP + template load | **0 templates** all paths; SCAN OK; AUTH framing open | M4k-AUTH-PATH.md |
| M4l | Enroll on pmOS | pending | Next |
| M5 | AUTH + Phosh unlock | Session unlock | Pending |

**Success = M5.**  
We now have a **live FocalTech TA conversation** on pmOS. Remaining work is fingerpr command path + greeter glue.

---

## What we need from you

| When | Need |
|------|------|
| Ongoing | Device online (SSH `fp5-pmos`), reboots/fastboot when asked |
| For M4–M5 | Finger on sensor on cue; enrolled print (Android or whatever path we use) |
| Optional oracle | iodeOS/UT still installable, or keep stock Android A |
| Risk | OK to flash **hybrid DTB / test modules** with stock recovery boot kept; no silent full custom kernel without recovery |
| Product | Confirm greeter: **Phosh** (default pmOS) is the unlock target |

**Not required:** new hardware, months of your coding time.

---

## Immediate next work (agents)

1. Patch / OOT: add `fairphone,fp5` to qseecom allowlist **or** module param to force init after version check.  
2. Capture QSEE version on live unit.  
3. Inventory Android `libQSEEComAPI` / start_app vs mainline `qcom_scm_qseecom_app_*` (gap for fingerpr load).  
4. Keep L2 FP driver; do not regress NFC dual-boot safety.

---

## Dual-boot safety (unchanged)

- No `qbootctl` on pmOS  
- Slot switch: `fastboot --set-active=a|b` only  
- Prefer modules / hybrid DTB over `boot-nfc-full`-class images  
