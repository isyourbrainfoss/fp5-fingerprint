# M4di — pre-INIT FDT soft vs image flag ladder

## Why (M4dh)
Fresh TA after PROBE (vio OK, **no Base**): layout1 **flag=0x240** 28×24 →
`array_new` OOM. Verdict said “heap dead before Base.”

But M4ck: **16×16 flag=0x1** soft CAP worked (FDT path). So maybe heap is
**not** empty — only the **image** CAP path always mallocs and fails.

## Change
After reload PROBE (vio OK), run ladder (HEALTH after each rung):

| Rung | Layout | Dims | Flag | PA |
|------|--------|------|------|-----|
| fdt16 | 1 | 16×16 | **0x1** | 0 |
| fdt28 | 1 | 28×24 | **0x1** | 0 |
| img16 | 1 | 16×16 | **0x240** | 0 |
| img28 | 1 | 28×24 | **0x240** | 0 |
| img28pa | 1 | 28×24 | **0x240** | 1 |

Then INIT_DEV Base + one post-Base img28 probe. Sample hold unchanged.

## Hold expect
- `M4di pre-INIT ladder begin`
- `M4di fdt16 …` / `img16 …` lines
- `M4di ladder summary fdt16=N …`
- `M4di verdict: FDT soft OK, image flag OOM` **or** both OOM / image soft
- SAMPLE banners still work

## Decision
| Result | Next |
|--------|------|
| FDT soft, image OOM | Attack image bind / skip array_new / flag bits — not Base free thrash |
| Both OOM | True TEE pressure pre-Base (listeners / TA size) |
| Image soft/frame | Use that shape under finger for rem |

## Hold result (2026-07-28 go) — decisive

| Rung | Result |
|------|--------|
| fdt16 flag=0x1 | **OOM** |
| fdt28 flag=0x1 | **OOM** |
| img16 flag=0x240 | **OOM** |
| img28 / img28pa | **OOM** |
| summary | all **-1** |
| **verdict** | **`FDT+image both OOM — heap/TA truly tight pre-Base`** |
| Base after INIT | OK |
| SAMPLE 0/1 | REAL DOWN OK, CAP OOM, rem=0 |

### Verdict
Not “image flag only.” On a **fresh** TA after PROBE (vio OK, no Base), **any** layout1 CAP including M4ck-style FDT soft **16×16 flag=0x1** hits `array_new` OOM.

M4ck soft success was a different context (earlier idle path / pre-reload / no dual INIT). Current reload+PROBE world has **no free TEE heap for CAP at all**.

### Next directions
1. **Listener / TZ pressure** — shrink or drop FS/RPMB/SSD SB after START; CAP before listeners  
2. **CAP before PROBE** (after 205/SYNC only) — does PROBE eat the heap?  
3. **No reload path** for CAP attempts — first-cold INIT only, CAP earlier in seq  
4. Android RE: when first CAP runs relative to listeners and heap  
