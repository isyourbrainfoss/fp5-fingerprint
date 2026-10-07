# M4dj — CAP before PROBE; defer listeners; no reload

## Why (M4di)
On reloaded TA after PROBE, **every** layout1 CAP (FDT soft + image) OOMs
before Base. Either TEE is empty by PROBE time, or reload/listeners poison heap.

## Change
1. **`ta_reload_after_base=0` (default)** — stop SHUTDOWN/START thrash  
2. **`defer_listeners=1` (default)** — FS/RPMB/SSD register **after** probe_device  
3. **`early_cap_stages=1` (default)** — mini pair **fdt16 + img16** at:
   - `post-entry` (205+1011 only)
   - `post-INIT_SPI`
   - `post-PROBE`
4. Then normal INIT_DEV Base + sample hold  

## Hold expect
```
M4dj defer listeners until after device probe
M4dj @post-entry fdt16=N img16=N
M4dj @post-INIT_SPI fdt16=N img16=N
M4dj @post-PROBE fdt16=N img16=N
M4dj @STAGE: CAP soft/frame ***   OR both OOM
Base update …
PRESS sample …
```

| Pattern | Meaning |
|---------|---------|
| soft @entry, OOM @PROBE | PROBE (or SPI) eats heap |
| OOM @entry already | TA load / session / no listeners still dead |
| soft through PROBE | listeners were the eater (deferred) |

## Decision
- Soft at entry → grow that window into under-finger CAP before heavy init  
- OOM at entry → TZ heap / start_app size / mainline QSEE limits  

## Hold result (2026-07-28 go) — decisive

| Stage | fdt16 | img16 | Note |
|-------|-------|-------|------|
| listeners | **deferred** (not registered yet) | | |
| **post-entry** | **OOM** | **OOM** | only 205+1011 done |
| post-INIT_SPI | OOM | OOM | |
| post-PROBE | OOM | OOM | |
| INIT_DEV Base | **OK** (Step1–3 + Base success) | | |
| Sample CAP | OOM | | REAL DOWN OK |
| listeners | registered **after** probe | | |

### Pivot
1. **Not listeners** — CAP OOMs before they exist.  
2. **Not PROBE / Base as first cause** — OOM already at **entry**.  
3. **Paradox:** INIT_DEV **Step1–3 still allocates** and Base succeeds, so TEE is **not** globally dead.  
   CAPTURE’s `array_new` fails even when calib mallocs work.  

→ Next: CAP bind/flags vs calib path; when Android first CAPTUREs; or residual whether CAP OOM is true malloc fail vs bad state labeled OOM.  
