#!/usr/bin/env python3
"""Best-effort pmOS life signs for FT9362 power-button FP.
Powers VDD (60), pulses RESET (35), watches IRQ (34) edges.
Does NOT talk SPI/TA — only proves pins are controllable.
Run as root on pmOS with py3-libgpiod.
"""
from __future__ import annotations
import os, sys, time
try:
    import gpiod
    from gpiod.line import Direction, Value, Edge
except ImportError:
    print("need: apk add py3-libgpiod", file=sys.stderr)
    sys.exit(2)

IRQ, RESET, VDD = 34, 35, 60

def find_tlmm():
    for name in sorted(os.listdir("/dev")):
        if not name.startswith("gpiochip"):
            continue
        path = f"/dev/{name}"
        with gpiod.Chip(path) as chip:
            info = chip.get_info()
            if "tlmm" in (info.label or "").lower() or info.num_lines >= 176:
                return path
    raise SystemExit("tlmm not found")

def main():
    if os.geteuid() != 0:
        print("need root", file=sys.stderr)
        return 2
    tlmm = find_tlmm()
    print("tlmm", tlmm)
    # Request: VDD high, RESET pulse, IRQ input with both edges
    config = {
        VDD: gpiod.LineSettings(direction=Direction.OUTPUT, output_value=Value.ACTIVE),
        RESET: gpiod.LineSettings(direction=Direction.OUTPUT, output_value=Value.ACTIVE),
        IRQ: gpiod.LineSettings(direction=Direction.INPUT, edge_detection=Edge.BOTH),
    }
    with gpiod.request_lines(tlmm, consumer="fp-poke", config=config) as req:
        print("VDD=1 (on), RESET assert high briefly then low/high pulse")
        time.sleep(0.05)
        # active-high reset: assert then release (guess — try both polarities)
        for name, seq in [
            ("pulse_high_then_low", [(Value.ACTIVE, 0.02), (Value.INACTIVE, 0.1)]),
            ("pulse_low_then_high", [(Value.INACTIVE, 0.02), (Value.ACTIVE, 0.1)]),
        ]:
            print("reset sequence", name)
            for val, delay in seq:
                req.set_value(RESET, val)
                time.sleep(delay)
            print("  irq level", req.get_value(IRQ))
            # wait for edges 2s
            t0 = time.time()
            edges = 0
            while time.time() - t0 < 2.0:
                for event in req.read_edge_events(timeout=0.2):
                    edges += 1
                    print("  EDGE", event.event_type, "line", event.line_offset, "irq_now", req.get_value(IRQ))
            print("  edges_in_2s", edges)
        print("leave VDD on, hold RESET inactive (low), watch 5s — press sensor")
        req.set_value(RESET, Value.INACTIVE)
        t0 = time.time()
        edges = 0
        while time.time() - t0 < 5.0:
            for event in req.read_edge_events(timeout=0.25):
                edges += 1
                print("  EDGE", event.event_type, "irq", req.get_value(IRQ))
        print("press_watch_edges", edges)
        print("final irq", req.get_value(IRQ), "vdd on")
    return 0

if __name__ == "__main__":
    sys.exit(main())
