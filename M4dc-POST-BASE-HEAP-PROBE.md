# M4dc — post-Base idle CAP heap probe

## Why
Under-finger layout1 CAP OOMs even at 28×24. Soft free failed.
Need to know **when** heap dies:

| Probe right after Base | Meaning |
|------------------------|---------|
| OOM at 28×24 | Base/INIT already filled TEE heap → cut INIT footprint / reuse Base |
| soft err=0 no frame | Heap free post-Base → arm/wait path eats it |
| frame raw idle | Image path works without finger → under-finger bind issue |

## Change
After Base SLEEP + HEALTH, **idle** layout1 CAP probe:
1. 28×24 flag `0x240` PA
2. If not OOM, 112×88
3. LOGCAT residual on OOM; HEALTH recover
4. SLEEP again before FDT arm

Under-finger AFR unchanged (M4db LATCH direct CAP).

## Hold result (2026-07-28, go completed)

### Timeline
| Step | Result |
|------|--------|
| Base after INIT_DEV | **OK** (`Base update success @0x7934a`, base_ok=1) |
| M4dc idle 28×24 CAP | **OOM** `app_err=0xffffff37` `ff_sensor_data_array_new[image.c:105]` |
| 112×88 probe | **skipped** (dead at 28×24) |
| Sample 0 REAL DOWN | touch=1 → LATCH CAP → **BURST0 OOM** rows=88 |
| Sample 1 REAL DOWN | touch=1 → LATCH CAP → **BURST0 OOM** rows=88 |
| rem_seen | **0** |
| ENROLL-late | fid generated, no image |
| AUTH | er=0 rem_seen=0 broken off / touch |

### Key logs
```
M4dc probe 28x24 er=0 app_err=0xffffff37 frame=0 rows=24
M4DC-OOM-RES: error at ff_sensor_data_array_new[image.c:105]: 'Out of memory'.
M4dc probe OOM at 28x24 — heap dead post-Base
sample 0 REAL DOWN … afr BURST0 CAP … app_err=0xffffff37
sample 1 REAL DOWN … afr BURST0 CAP … app_err=0xffffff37
enroll sample loop done rem_seen=0
AUTH er=0 err=0x0 rem_seen=0
```

### Decision (criterion met for when-heap-dies)
**Heap is already empty for `array_new` immediately after Base**, before FDT arm / wait / finger.

Not: dual-TA-only, ENROLL-only, meta-vs-big-plen, “only full frame”, soft-free thrash.

**Next SW target:** INIT/Base footprint — thinner INIT path, free Base working buffers after sticky Base, or **reuse Base raw buffer** for CAP without a second `array_new`.

## Hold expect (reference)
- `M4dc post-Base heap probe begin`
- `M4dc probe 28x24 …` OOM **or** soft/no-frame
- Then SAMPLE banners + under-finger CAP as before
