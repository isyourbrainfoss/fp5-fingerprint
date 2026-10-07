# M4cr — warm base / less pre-CAP heap thrash

## Why (M4cq + Android enroll)
Android under finger: **frame raw×3**, no Step1 re-log, no OOM.  
pmOS clean CAP: residual **FDT-BASE → Step1 try → array_new OOM**.

Hypothesis: INIT Base is not left sticky, or force-calib / multi-LOGCAT
before samples leaves TA unable to allocate for under-finger CAP.

## Change
1. INIT_DEV: **one** LOGCAT drain (was ×4)
2. Enroll+skip_capture: **never** `try_force_img_base_calib` after INIT  
   (SYNC_CALIB only when base already seen)
3. skipcap: no second force base
4. AFR: settle → **FDT_DOWN + SCAN** → meta CAP ×3 pa=0; stop on OOM

## Hold expect
- `android-frame-raw M4cr` + `base=1` if INIT Base stuck
- `afr postarm` then `BURST0`
- Look for frame raw **without** Step1 in residual, or still OOM
