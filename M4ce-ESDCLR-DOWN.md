# M4ce — CAP if itype 0x2 appears during esd_clear

M4cd hold: mid-wait esd_clear briefly showed **itype=0x2** while user pressed,
then continued clear/WMODE and lost DOWN → rem_seen=0.

## Fix
- `esd_clear_loop` returns true if DOWN seen; aborts clear immediately.
- Mid-wait: if clear returns DOWN → REAL DOWN + AFR CAP.
- Fresh QUERY after clear as backup.
