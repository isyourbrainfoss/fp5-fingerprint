# M4ct — sticky QES parse + CAP on latched REAL DOWN

## Why (M4cs)
```
settle CLEAN itype=0x2 touch=1
precap QUERY → touch=0 itype=0x0 QES unknow
CAP residual MARK finger touch then QES_LAST unknow + Step1 OOM
```
Last-line QES rule discarded mid-buffer finger touch. Extra QUERY after sample REAL DOWN clears latched state.

## Change
1. Residual parse: sticky **finger_touch** / **itype 0x2** over trailing **unknow**
2. Leave/esd last still win when exclusive
3. AFR: if sample already REAL DOWN → **skip settle + precap**, CAP immediately

## Hold expect
- `afr LATCH skip settle (sample REAL DOWN …)`
- `QES sticky finger_touch` on QUERY paths
- BURST residual: look for finger_touch sticky or still OOM
