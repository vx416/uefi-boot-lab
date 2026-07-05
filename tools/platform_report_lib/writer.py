from __future__ import annotations

import json
from dataclasses import asdict
from pathlib import Path

from .diagnostics import build_diagnostics
from .expected import load_expected_platform
from .models import PlatformReport
from .parsers import build_firmware_view, build_linux_view
from .utils import utc_timestamp


def write_markdown(report: PlatformReport, output_path: Path) -> None:
    lines = [
        "# Platform Report",
        "",
        f"- Generated UTC: `{report.generated_at_utc}`",
        f"- Firmware view source: `{report.firmware_view.source_path or 'not provided'}`",
        f"- Expected platform source: `{report.expected_platform.source_path or 'not provided'}`",
        f"- Linux artifact dir: `{report.linux_view.artifact_dir}`",
        "",
        "## Firmware View Summary",
        "",
    ]

    if report.firmware_view.loaded:
        firmware = report.firmware_view.data.get("firmware", {})
        acpi = report.firmware_view.data.get("acpi", {})
        memory_map = report.firmware_view.data.get("memoryMap", {})
        lines.append(f"- Firmware vendor: `{firmware.get('vendor', 'unknown')}`")
        lines.append(f"- UEFI revision: `{firmware.get('uefiRevision', 'unknown')}`")
        lines.append(f"- ACPI tables: `{', '.join(acpi.get('tables', []))}`")
        lines.append(f"- Memory descriptors: `{memory_map.get('descriptorCount', 'unknown')}`")
    else:
        lines.append(f"- Not loaded: `{report.firmware_view.error}`")

    lines.extend([
        "",
        "## Artifact Status",
        "",
        "| Artifact | Status | Detail |",
        "| --- | --- | --- |",
    ])

    for status in report.linux_view.statuses:
        lines.append(f"| `{status.name}` | `{status.status}` | {status.detail} |")

    lines.extend(["", "## Linux View Summary", ""])
    lines.append(f"- `/proc/iomem` regions: {len(report.linux_view.iomem_regions)}")
    lines.append(f"- e820 / EFI memory lines: {len(report.linux_view.e820_lines)}")
    lines.append(f"- PCI devices parsed: {len(report.linux_view.pci_devices)}")
    lines.append(f"- CPU fields parsed: {len(report.linux_view.cpu_summary)}")
    lines.append(f"- NUMA lines parsed: {len(report.linux_view.numa_summary)}")
    lines.append(f"- SMBIOS identity fields parsed: {len(report.linux_view.smbios_identity)}")

    lines.extend(["", "## Expected Platform Checks", ""])
    lines.extend(["| Status | Check | Detail |", "| --- | --- | --- |"])
    if report.diagnostics.expected_checks:
        for check in report.diagnostics.expected_checks:
            lines.append(f"| `{check.status}` | `{check.name}` | {check.detail} |")
    else:
        lines.append("| `WARN` | `expected_platform_loaded` | no expected checks were configured |")

    lines.extend(["", "## Diagnostics", ""])
    for label, values in [
        ("Missing artifacts", report.diagnostics.missing_artifacts),
        ("Failed artifacts", report.diagnostics.failed_artifacts),
        ("Missing ACPI tables", report.diagnostics.missing_acpi_tables),
        ("Suspicious memory ranges", report.diagnostics.suspicious_memory_ranges),
        ("PCI topology notes", report.diagnostics.pci_topology_notes),
        ("Identity notes", report.diagnostics.identity_notes),
    ]:
        lines.append(f"### {label}")
        if values:
            lines.extend(f"- {value}" for value in values)
        else:
            lines.append("- None")
        lines.append("")

    output_path.write_text("\n".join(lines), encoding="utf-8")


def generate_report(
    artifact_dir: Path,
    output_dir: Path,
    firmware_json: Path | None = None,
    expected_platform_yaml: Path | None = None,
) -> PlatformReport:
    output_dir.mkdir(parents=True, exist_ok=True)
    linux_view = build_linux_view(artifact_dir)
    firmware_view = build_firmware_view(firmware_json)
    expected_platform = load_expected_platform(expected_platform_yaml)
    report = PlatformReport(
        schema_version=1,
        generated_at_utc=utc_timestamp(),
        firmware_view=firmware_view,
        linux_view=linux_view,
        expected_platform=expected_platform,
        diagnostics=build_diagnostics(linux_view, firmware_view, expected_platform),
    )

    json_path = output_dir / "platform-report.json"
    md_path = output_dir / "platform-report.md"
    json_path.write_text(json.dumps(asdict(report), indent=2) + "\n", encoding="utf-8")
    write_markdown(report, md_path)
    return report
