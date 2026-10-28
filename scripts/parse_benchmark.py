#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
# SPDX-License-Identifier: Apache-2.0

"""Validate raw thread benchmark logs and emit machine-readable measurements."""

import argparse
import csv
import json
from pathlib import Path
from statistics import median

METRICS = (
    "mutex_uncontended",
    "mutex_first_use",
    "mutex_contended",
    "condition_round_trip",
    "thread_create_join",
    "call_once_ready",
)
SAMPLE_COUNT = 9


def parse_log(path):
    metadata = {}
    samples = {metric: {} for metric in METRICS}
    summaries = {}
    complete = False
    for line in path.read_text(errors="replace").splitlines():
        # Console runners may prefix non-CSV startup lines; benchmark rows themselves are raw CSV.
        if "BENCH_ERROR," in line or "CPP_BENCH_FAILED" in line:
            raise ValueError(f"{path}: benchmark reported failure: {line}")
        if line.strip() == "CPP_BENCH_DONE":
            complete = True
        fields = next(csv.reader([line.strip()]))
        if not fields:
            continue
        if fields[0] == "META":
            if len(fields) != 3 or fields[1] in metadata:
                raise ValueError(f"{path}: malformed or duplicate metadata row: {line}")
            try:
                value = int(fields[2])
            except ValueError:
                value = fields[2]
            metadata[fields[1]] = value
        elif fields[0] == "BENCH_RAW":
            if len(fields) != 6 or fields[1] not in samples:
                raise ValueError(f"{path}: malformed raw sample: {line}")
            metric, index, ticks, operations, hz = fields[1], *map(int, fields[2:])
            if index in samples[metric] or ticks < 0 or operations <= 0 or hz <= 0:
                raise ValueError(f"{path}: invalid or duplicate raw sample: {line}")
            samples[metric][index] = (ticks, operations, hz)
        elif fields[0] == "BENCH_SUMMARY":
            if len(fields) != 7 or fields[1] not in samples or fields[1] in summaries:
                raise ValueError(f"{path}: malformed or duplicate summary: {line}")
            summaries[fields[1]] = tuple(map(int, fields[2:]))
    if not complete:
        raise ValueError(f"{path}: missing successful completion marker")
    if metadata.get("main_priority") != metadata.get("worker_priority"):
        raise ValueError(f"{path}: main and worker priorities differ")
    if metadata.get("measured_batches") != SAMPLE_COUNT or metadata.get("warmup_batches") != 1:
        raise ValueError(f"{path}: unexpected warmup/sample count")
    measurements = []
    for metric, indexed in samples.items():
        if set(indexed) != set(range(SAMPLE_COUNT)):
            raise ValueError(f"{path}: {metric} does not contain exactly nine indexed samples")
        raw_ticks = [indexed[index][0] for index in range(SAMPLE_COUNT)]
        operation_counts = {value[1] for value in indexed.values()}
        frequencies = {value[2] for value in indexed.values()}
        if len(operation_counts) != 1 or frequencies != {metadata.get("counter_hz")}:
            raise ValueError(f"{path}: {metric} changes operation count or timer frequency")
        operations = operation_counts.pop()
        hz = frequencies.pop()
        minimum_ticks = min(raw_ticks)
        median_ticks = int(median(raw_ticks))
        # Nearest-rank p90 for nine samples is the maximum; retain raw data for inspection.
        p90_ticks = sorted(raw_ticks)[(SAMPLE_COUNT * 9 + 9) // 10 - 1]
        ns_per_operation = median_ticks * 1_000_000_000 // hz // operations
        expected = (SAMPLE_COUNT, minimum_ticks, median_ticks, p90_ticks, ns_per_operation)
        if summaries.get(metric) != expected:
            raise ValueError(f"{path}: {metric} summary does not match raw samples")
        measurements.append(
            {
                "metric": metric,
                "operations_per_batch": operations,
                "raw_counter_ticks": raw_ticks,
                "minimum_ticks": minimum_ticks,
                "median_ticks": median_ticks,
                "p90_nearest_rank_ticks": p90_ticks,
                "median_ns_per_operation": ns_per_operation,
            }
        )
    return {"log": str(path), "metadata": metadata, "measurements": measurements}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("logs", nargs="+", type=Path)
    parser.add_argument("-o", "--output", type=Path)
    args = parser.parse_args()
    try:
        runs = [parse_log(path) for path in args.logs]
    except (OSError, ValueError) as error:
        parser.error(str(error))
    result = {
        "measurement": (
            "Elapsed platform-timer ticks under QEMU are simulator observations, "
            "not retired CPU cycles or hardware performance guarantees."
        ),
        "runs": runs,
    }
    rendered = json.dumps(result, indent=2) + "\n"
    if args.output:
        args.output.write_text(rendered)
    else:
        print(rendered, end="")


if __name__ == "__main__":
    main()
