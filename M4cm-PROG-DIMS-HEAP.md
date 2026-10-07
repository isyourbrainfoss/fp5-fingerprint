# M4cm — REAL DOWN progressive layout1 dims (TA-heap)

## Why
M4cl: under REAL DOWN, layout1 full **112×88** enters image path then
`ff_sensor_data_array_new[image.c:105]: Out of memory` → Null `0xffffff37`.
HEALTH clears sticky; L0 fallback residual **f:N**. rem=0.

M4ck proved layout1 **form** works idle at **16×16** (no OOM).

## Change (`try_android_frame_raw`)
1. **HEALTH pre-free** before any image CAP
2. **Progressive layout1** flag `0x240`: 16×16 → 32×32 → 56×44 → 112×44 → 112×88  
   HEALTH after OOM; climb while soft no-frame
3. **No inter-step LOGCAT** (residual dump once on first OOM or on success) — M4cl LOGCAT flooded dmesg
4. One **L0** full CAP if still no frame; REPORT once

## Hold expect
- `android-frame-raw M4cm`
- `afr L1 progN` with cols/rows; OOM only above some size
- Possible `*** ANDROID FRAME RAW L1 WxH ***` if any rung fits
- wall still ≪ 13s; rem if frame/enrolling
