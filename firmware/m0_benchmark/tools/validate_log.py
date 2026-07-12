#!/usr/bin/env python3
"""Validate the bounded, key=value M0 evidence emitted by a physical run."""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path

MARKER_RE = re.compile(r"\b(M0_[A-Z_]+)\s+(.+)$")


def parse_records(text: str) -> list[dict[str, str]]:
    records: list[dict[str, str]] = []
    for line in text.splitlines():
        match = MARKER_RE.search(line)
        if not match:
            continue
        record = {"marker": match.group(1)}
        for token in match.group(2).split():
            if "=" in token:
                key, value = token.split("=", 1)
                record[key] = value.rstrip(",")
        records.append(record)
    return records


def validate(records: list[dict[str, str]]) -> list[str]:
    errors: list[str] = []
    required = {"M0_DISPLAY_CONFIG", "M0_PSRAM", "M0_JPEG", "M0_DISPLAY_CASE",
                "M0_SD_READ", "M0_SD", "M0_COMBINED", "M0_HEAP", "M0_COMPLETE"}
    present = {record["marker"] for record in records}
    errors.extend(f"missing marker {marker}" for marker in sorted(required - present))

    combined = next((r for r in records if r["marker"] == "M0_COMBINED"), None)
    if combined:
        fields = ("requested_frames", "completed_frames", "failed_frames", "dropped_frames",
                  "elapsed_us", "achieved_fps", "sd_read_us", "jpeg_decode_us",
                  "lvgl_display_us", "psram_free", "psram_largest", "internal_free")
        for field in fields:
            if field not in combined:
                errors.append(f"M0_COMBINED missing {field}")
        try:
            requested = int(combined["requested_frames"])
            completed = int(combined["completed_frames"])
            failed = int(combined["failed_frames"])
            dropped = int(combined["dropped_frames"])
            if completed + failed > requested:
                errors.append("M0_COMBINED completed+failed exceeds requested")
            if dropped > completed:
                errors.append("M0_COMBINED dropped exceeds completed")
            if float(combined["achieved_fps"]) < 0:
                errors.append("M0_COMBINED achieved_fps is negative")
        except (KeyError, ValueError):
            errors.append("M0_COMBINED contains non-numeric counters")

    sd_passes = {r.get("pass") for r in records if r["marker"] == "M0_SD_READ"}
    if not {"1", "2"}.issubset(sd_passes):
        errors.append("M0_SD_READ requires pass=1 and pass=2")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("log", type=Path)
    parser.add_argument("--json", action="store_true", help="emit parsed records as JSON")
    args = parser.parse_args()
    records = parse_records(args.log.read_text(encoding="utf-8", errors="replace"))
    errors = validate(records)
    if args.json:
        print(json.dumps({"valid": not errors, "errors": errors, "records": records}, indent=2))
    else:
        print("M0_LOG_VALID" if not errors else "M0_LOG_INVALID " + "; ".join(errors))
    return 0 if not errors else 1


if __name__ == "__main__":
    raise SystemExit(main())
