#!/usr/bin/env python3
"""Print how long each CI sub-task took and how many cores were busy, from what the job wrote to RUNNER_TEMP."""
import os

tmp = os.environ.get("RUNNER_TEMP", "/tmp")
start = int(os.environ.get("JOB_START_EPOCH", "0") or 0)

timings = os.path.join(tmp, "timings.txt")
if os.path.exists(timings):
    rows = [ln.split() for ln in open(timings) if ln.strip()]
    rows.sort(key=lambda r: -int(r[1].rstrip("s")))
    print("== sub-task wall time ==")
    for r in rows:
        print("  %-18s %6s  %s" % (r[0], r[1], " ".join(r[2:])))

cpu = os.path.join(tmp, "cpu.log")
if os.path.exists(cpu):
    ncpu = os.cpu_count() or 1
    samples = []
    prev = None
    for ln in open(cpu):
        parts = ln.split()
        if len(parts) < 3 or not parts[0].isdigit():
            continue
        ts = int(parts[0])
        if parts[1] == "cpu":
            v = [int(x) for x in parts[2:10]]
            idle, total = v[3] + v[4], sum(v)
            if prev and total > prev[0]:
                samples.append((ts, ncpu * (1.0 - (idle - prev[1]) / (total - prev[0]))))
            prev = (total, idle)
        elif "idle" in ln and "sys," in ln:
            idle = float(ln.split("sys,")[1].split("%")[0])
            samples.append((ts, ncpu * (100.0 - idle) / 100.0))
    buckets = {}
    for ts, busy in samples:
        buckets.setdefault((ts - start) // 10, []).append(busy)
    print("== busy cores of %d, by 10 s from the job's start ==" % ncpu)
    for k in sorted(buckets):
        avg = sum(buckets[k]) / len(buckets[k])
        print("  %4d-%4d s  %4.1f  %s" % (k * 10, k * 10 + 10, avg, "#" * int(round(avg * 10))))
