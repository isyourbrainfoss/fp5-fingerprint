# M4dk — layout0 vs layout1 matrix (entry + post-Base)

## Why (M4dj paradox)
- CAP OOM at **post-entry** (no listeners, no PROBE, no Base)
- INIT_DEV **Step1–3 still mallocs** and Base succeeds  
→ not “TEE has zero bytes”; CAPTURE path differs from calib path.

M4s/M4ck: **layout0** often soft-success (FDT); **layout1** image OOMs.

## Change
1. At **post-entry** and **post-Base**: matrix  
   L0×fdt(0x1), L0×img(0x240), L1×fdt, L1×img (16×16)  
2. Drop SPI/PROBE thrash pairs (M4dj already OOM everywhere layout1)  
3. If any soft/frame: remember layout+flag for under-finger AFR  
4. Keep defer_listeners; no TA reload  

## Hold expect
```
M4dk matrix @post-entry …
M4dk @post-entry L0fdt=N L0img=N L1fdt=N L1img=N
M4dk @post-entry verdict: …
*** Base update after INIT_DEV ***
M4dk matrix @post-Base …
M4dk prefer L? flag=0x? for AFR   (if soft found)
```

| Result | Meaning |
|--------|---------|
| L0 soft, L1 OOM | form gate — use layout0 under finger |
| all OOM entry, soft post-Base | need Base state for CAP |
| all OOM both | CAPTURE broken independent of form (next: residual RE) |

## Hold result (2026-07-28 go) — **BREAKTHROUGH**

| Stage | L0fdt | L0img | L1fdt | L1img |
|-------|-------|-------|-------|-------|
| **post-entry** | OOM | OOM | OOM | OOM |
| **post-Base** | **soft 0** | **soft 0** | **soft 0** | **soft 0** |

- Prefer: `L0 flag=0x1` (first soft in order)  
- Under finger AFR: **no OOM** — `app_err=0` ms~1s vio=1 rows=0 frame=0 (FDT soft path)  
- rem_seen=0 (no image frame raw yet)

### Verdict
1. CAPTURE is **not** permanently OOM — needs **Base first**.  
2. post-entry OOM = device not ready / wrong state, not “heap empty forever.”  
3. post-Base: **all forms soft** including **L1+0x240 16×16** — image form is open.  
4. Under finger we picked **0x1 FDT** first → soft, no frame. Next: force **L1+0x240** (full 112×88) under finger after Base.

### Next (M4dl)
AFR after Base: **layout1 flag=0x240** full frame (not prefer FDT 0x1); post-Base matrix already proved L1img soft at 16×16.
