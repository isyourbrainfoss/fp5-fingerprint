# M4u — Enroll protocol complete path, no template match (2026-07-27)

## What works
- FDT CAP soft-success (`fdt_ok=1`)
- Finger detect via L2 IRQ
- 8× enroll CAP `err=0` + 8× ENROLL `er=0` + POST_ENROLL + AUTH soft `er=0`
- TA stays alive (`HEALTH final er=0`)
- No SCM -22 kill on ENROLL

## What fails for unlock
- Soft SPI/vio noise: `configure spi mode failed(0)`, `set chip vio 1.8v fail` (often with FDT still progressing)
- **New fid every ENROLL sample** — no Android-style rem countdown on one enrollment
- AUTH: **`enrollment is unexpected broken off`** / empty enroll image / empty template list
- No AUTH match / no Phosh unlock

## Interpretation
Protocol glue for enroll/AUTH is live on pmOS. Missing piece is a **held finger producing accepted image samples** into the algorithm (plus stable SPI during those samples). IRQ edge alone is not enough.

**Follow-up M4v:** re-calling ENROLL per sample caused multi-fid; fixed to Android-style **one ENROLL + CAPTURE samples**. Still no rem without held finger / frame raw.

## User action
Hold finger on rear sensor for the entire ~30s enroll window after:
`qsee_fp: waiting up to Ns for finger`

Evidence: `{SCRATCH}/fp-enroll-or-unlock.log`, `lab/fingerprint/qsee-enroll-or-unlock.log`
