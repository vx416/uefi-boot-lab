from __future__ import annotations

import argparse
import os
from pathlib import Path
from typing import Sequence

from .collector import collect_linux_view
from .models import PlatformReport
from .utils import repo_root, utc_timestamp
from .writer import generate_report


def default_artifact_dir() -> Path:
    return repo_root() / "artifacts" / "linux-view" / utc_timestamp()


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Collect Linux OS view and generate platform diagnostics reports.")
    subparsers = parser.add_subparsers(dest="command", required=True)

    collect_parser = subparsers.add_parser("collect", help="Collect Phase 2 Linux OS view artifacts.")
    collect_parser.add_argument("--out", type=Path, default=None, help="Artifact output directory.")

    report_parser = subparsers.add_parser("report", help="Generate Phase 3 report from Linux artifacts.")
    report_parser.add_argument("--artifacts", type=Path, required=True, help="Linux artifact directory.")
    report_parser.add_argument("--out", type=Path, default=None, help="Report output directory.")
    report_parser.add_argument("--firmware-json", type=Path, default=None, help="Phase 1 firmware-view.json path.")
    report_parser.add_argument("--expected-platform", type=Path, default=None, help="Expected platform YAML path.")
    report_parser.add_argument("--fail-on-error", action="store_true", help="Exit non-zero when any expected check fails.")

    all_parser = subparsers.add_parser("all", help="Collect Linux view and generate report.")
    all_parser.add_argument("--out", type=Path, default=None, help="Artifact output directory.")
    all_parser.add_argument("--report-out", type=Path, default=None, help="Report output directory.")
    all_parser.add_argument("--firmware-json", type=Path, default=None, help="Phase 1 firmware-view.json path.")
    all_parser.add_argument("--expected-platform", type=Path, default=None, help="Expected platform YAML path.")
    all_parser.add_argument("--fail-on-error", action="store_true", help="Exit non-zero when any expected check fails.")

    return parser.parse_args(argv)


def report_has_failures(report: PlatformReport) -> bool:
    return any(check.status == "FAIL" for check in report.diagnostics.expected_checks)


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv if argv is not None else os.sys.argv[1:])

    if args.command == "collect":
        artifact_dir = args.out or default_artifact_dir()
        collect_linux_view(artifact_dir)
        print(f"Linux OS view written to: {artifact_dir}")
        return 0

    if args.command == "report":
        report_dir = args.out or (args.artifacts / "report")
        report = generate_report(args.artifacts, report_dir, args.firmware_json, args.expected_platform)
        print(f"Platform report written to: {report_dir}")
        if args.fail_on_error and report_has_failures(report):
            print("Platform report has failing expected checks.")
            return 1
        return 0

    if args.command == "all":
        artifact_dir = args.out or default_artifact_dir()
        report_dir = args.report_out or (artifact_dir / "report")
        collect_linux_view(artifact_dir)
        report = generate_report(artifact_dir, report_dir, args.firmware_json, args.expected_platform)
        print(f"Linux OS view written to: {artifact_dir}")
        print(f"Platform report written to: {report_dir}")
        if args.fail_on_error and report_has_failures(report):
            print("Platform report has failing expected checks.")
            return 1
        return 0

    raise ValueError(f"Unsupported command: {args.command}")
