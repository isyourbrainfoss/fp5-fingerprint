# M4cd — stop power-cycling FP during sample wait

**Date:** 2026-07-28  
**Root cause:** `l2_listen_on()` wrote sysfs `listen=1` every sample; L2
`listen_store` always ran `ff_power_cycle()` (VDD+RESET). That re-stuck TA
`[esd]` and produced itype=0x0 IRQ storms — never itype=0x2 / finger_touch.

## Fix
1. **qsee `l2_listen_on`**: ioctl SPI/IRQ only; write listen=1 only if currently 0.
2. **L2 `listen_store`**: if IRQ already enabled, skip power cycle.
3. **Pre-hold arming**: drop VIO latch thrash; strong `esd_clear` (3) + FDT only.
4. **Per-sample**: no listen/VDD reassert; SPI clock vote only.
5. **Mid-wait ESD**: escalate to try=2 (L2 reset) without listen power cycle.

## Hold
Still 2×8s, REAL DOWN required for AFR CAP (M4cb/M4cc debounce retained).
