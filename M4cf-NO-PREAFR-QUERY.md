# M4cf — do not QUERY after REAL DOWN before CAP

M4ce: `REAL DOWN touch=1 itype=0x2` then `pre-AFR QUERY` → `itype=0x0 touch=0`
before first CAP → FDT-only residual, rem_seen=0.

Android: finger touch → CAP within ~50ms (no clearing QUERY).

## Fix
Remove pre-AFR QUERY; 12ms settle only then CAP×3.
