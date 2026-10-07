#!/bin/sh
# Optional human hold (NOT required for pure-SW goal). 10s windows.
# Usage: on phone after clean START: sh fp-hold-short.sh
export SAMPLE_WAIT=10 ENROLL_SAMPLES=6
exec sh /tmp/fp-clean-load.sh
