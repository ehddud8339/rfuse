#!/usr/bin/env python3
"""Merge RFUSE workload latency logs into one readable report.

Input file name pattern:
  <prefix>-<workload>-lat.txt
"""

from __future__ import annotations

import argparse
import math
import re
import statistics
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Tuple

FILE_RE = re.compile(r"^(.+)-([^-]+)-lat\.txt$")
KV_RE = re.compile(r"([A-Za-z0-9_]+)=([^\s]+)")
TS_RE = re.compile(r"\bts=(\d+)\b")
SRC_RE = re.compile(r"^\[([A-Za-z])\]\s+")

STAGE_PAIRS: List[Tuple[str, str, str]] = [
    ("req_init", "enqueue", "req_init_to_enqueue"),
    ("enqueue", "dequeue", "enqueue_to_dequeue"),
    ("dequeue", "io_submit", "dequeue_to_io_submit"),
    ("io_submit", "io_complete", "io_submit_to_io_complete"),
    ("io_complete", "reply_signal_send", "io_complete_to_reply_send"),
    ("reply_signal_send", "reply_signal_recv", "reply_send_to_reply_recv"),
    ("reply_signal_recv", "reply_handle", "reply_recv_to_reply_handle"),
    ("reply_handle", "reply_handle_done", "reply_handle_to_done"),
]

STAGE_ALIAS = {
    "io_start": "io_submit",
    "io_done": "io_complete",
    "reply_notify_enter": "reply_signal_send",
    "reply_notify_ioctl": "reply_signal_recv",
    "request_end_enter": "reply_handle",
    "request_end_done": "reply_handle_done",
}


@dataclass
class Event:
    ts: int
    workload: str
    src: str
    stage: str
    req_key: Optional[str]
    line: str


def percentile(sorted_values: List[float], pct: float) -> float:
    if not sorted_values:
        return 0.0
    if len(sorted_values) == 1:
        return sorted_values[0]
    rank = pct * (len(sorted_values) - 1)
    low = math.floor(rank)
    high = math.ceil(rank)
    if low == high:
        return sorted_values[low]
    frac = rank - low
    return sorted_values[low] * (1.0 - frac) + sorted_values[high] * frac


def parse_event(line: str, workload: str) -> Optional[Event]:
    ts_m = TS_RE.search(line)
    stage_m = re.search(r"\bstage=([^\s]+)", line)
    if not ts_m or not stage_m:
        return None

    kv = dict(KV_RE.findall(line))
    req_key = None
    for key in ("unique", "req", "riq"):
        value = kv.get(key)
        if value and value != "0":
            req_key = f"{key}:{value}"
            break
    if req_key and "opcode" in kv:
        req_key = f"{req_key}|opcode:{kv['opcode']}"

    src_m = SRC_RE.match(line)
    src = src_m.group(1) if src_m else "?"
    stage = STAGE_ALIAS.get(stage_m.group(1), stage_m.group(1))
    return Event(
        ts=int(ts_m.group(1)),
        workload=workload,
        src=src,
        stage=stage,
        req_key=req_key,
        line=line.rstrip("\n"),
    )


def collect_events(input_dir: Path, output_name: str) -> Tuple[List[Event], List[Tuple[str, str]]]:
    events: List[Event] = []
    matched_files: List[Tuple[str, str]] = []
    for path in sorted(input_dir.glob("*-*-lat.txt")):
        if path.name == output_name:
            continue
        fm = FILE_RE.match(path.name)
        if not fm:
            continue
        workload = fm.group(2)
        matched_files.append((path.name, workload))
        with path.open("r", encoding="utf-8", errors="replace") as f:
            for line in f:
                ev = parse_event(line, workload)
                if ev is not None:
                    events.append(ev)
    events.sort(key=lambda e: e.ts)
    return events, matched_files


def build_latency_stats(events: Iterable[Event]) -> Dict[str, Dict[str, List[int]]]:
    by_req: Dict[Tuple[str, str], Dict[str, int]] = {}
    for ev in events:
        if ev.req_key is None:
            continue
        k = (ev.workload, ev.req_key)
        stages = by_req.setdefault(k, {})
        stages.setdefault(ev.stage, ev.ts)

    out: Dict[str, Dict[str, List[int]]] = {}
    for (workload, _), stages in by_req.items():
        w = out.setdefault(workload, {})
        for start, end, label in STAGE_PAIRS:
            if start in stages and end in stages and stages[end] >= stages[start]:
                w.setdefault(label, []).append(stages[end] - stages[start])
    return out


def format_stats(values_ns: List[int]) -> str:
    if not values_ns:
        return "count=0"
    values_us = sorted(v / 1000.0 for v in values_ns)
    avg = statistics.fmean(values_us)
    p50 = percentile(values_us, 0.50)
    p95 = percentile(values_us, 0.95)
    mx = values_us[-1]
    return f"count={len(values_us):6d} avg={avg:10.3f}us p50={p50:10.3f}us p95={p95:10.3f}us max={mx:10.3f}us"


def write_report(output_path: Path, events: List[Event], files: List[Tuple[str, str]]) -> None:
    stats = build_latency_stats(events)
    first_ts = events[0].ts if events else 0
    last_ts = events[-1].ts if events else 0
    span_us = (last_ts - first_ts) / 1000.0 if events else 0.0

    with output_path.open("w", encoding="utf-8") as out:
        out.write("# RFUSE Workload Latency Merge Report\n")
        out.write(f"# files={len(files)} events={len(events)} span={span_us:.3f}us\n\n")

        out.write("## Input Files\n")
        for name, workload in files:
            out.write(f"- workload={workload:>6s} file={name}\n")
        out.write("\n")

        out.write("## Stage Latency Summary (per workload)\n")
        for workload in sorted(stats):
            out.write(f"\n[workload={workload}]\n")
            for _, _, label in STAGE_PAIRS:
                out.write(f"{label:28s} {format_stats(stats[workload].get(label, []))}\n")

        out.write("\n## Merged Timeline (sorted by ts)\n")
        for ev in events:
            rel_us = (ev.ts - first_ts) / 1000.0 if first_ts else 0.0
            out.write(
                f"ts={ev.ts} rel={rel_us:12.3f}us workload={ev.workload:>6s} src={ev.src} "
                f"stage={ev.stage:22s} req={ev.req_key or '-'} | {ev.line}\n"
            )


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Merge <prefix>-<workload>-lat.txt files and generate readable report."
    )
    parser.add_argument("output_filename", help="Output file name (no '/').")
    parser.add_argument(
        "--input-dir",
        default=str(Path.home() / "ehddud8339.storage/logs/RFUSE"),
        help="Directory containing workload log files.",
    )
    args = parser.parse_args()

    if "/" in args.output_filename:
        raise SystemExit("output_filename must be a plain file name (no '/').")

    input_dir = Path(args.input_dir).expanduser()
    input_dir.mkdir(parents=True, exist_ok=True)
    output_path = input_dir / args.output_filename

    events, files = collect_events(input_dir, args.output_filename)
    if not files:
        raise SystemExit(f"No workload logs found in {input_dir} (pattern: *-*-lat.txt)")

    write_report(output_path, events, files)
    print(output_path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
