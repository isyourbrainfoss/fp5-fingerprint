# M4bb — skip_capture_path for clean ENROLL (2026-07-27)

## Why

Pre-enroll CAPTURE flag/size sweep causes many `configure spi mode failed`
and leaves chip in RST/ESD before the user even presses.

REE SPI (pinmux / geni / GPIO bitbang) all **reboot** this unit (M4ba).

## Change

`skip_capture_path=1`: FDT arm + one soft CAP only; skip multi-flag CAP and
post-fdt force-image thrash. Image path reserved for finger samples.

## Clean-boot smoke

- ENROLL start er=0, **new fid** generated  
- `chip ready itype=0x0`  
- HEALTH final er=0  
- Warm CAP can still log spi mode fail once; later vio ok  
- rem_seen=0 without finger (expected)  

## Hold

```sh
# /tmp/do-enroll-m4al.sh uses skip_capture_path=1
ssh fp5-pmos 'bash /tmp/do-enroll-m4al.sh'
```
