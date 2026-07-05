from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any


@dataclass(frozen=True)
class CommandSpec:
    name: str
    argv: tuple[str, ...]
    privileged: bool = False


@dataclass
class ArtifactStatus:
    name: str
    status: str
    detail: str
    path: str | None = None


@dataclass
class LinuxView:
    artifact_dir: str
    statuses: list[ArtifactStatus] = field(default_factory=list)
    iomem_regions: list[str] = field(default_factory=list)
    e820_lines: list[str] = field(default_factory=list)
    pci_devices: list[str] = field(default_factory=list)
    cpu_summary: dict[str, str] = field(default_factory=dict)
    numa_summary: list[str] = field(default_factory=list)
    smbios_identity: dict[str, str] = field(default_factory=dict)


@dataclass
class FirmwareView:
    source_path: str | None = None
    loaded: bool = False
    data: dict[str, Any] = field(default_factory=dict)
    error: str | None = None


@dataclass
class ExpectedPlatform:
    source_path: str | None = None
    loaded: bool = False
    platform_id: str | None = None
    expected_firmware_vendor: str | None = None
    expected_firmware_vendors: list[str] = field(default_factory=list)
    required_acpi_tables: list[str] = field(default_factory=list)
    min_memory_mib: int | None = None
    require_smbios: bool = False
    required_linux_artifacts: list[str] = field(default_factory=list)
    error: str | None = None


@dataclass
class ExpectedCheck:
    status: str
    name: str
    detail: str


@dataclass
class Diagnostics:
    missing_artifacts: list[str] = field(default_factory=list)
    failed_artifacts: list[str] = field(default_factory=list)
    missing_acpi_tables: list[str] = field(default_factory=list)
    suspicious_memory_ranges: list[str] = field(default_factory=list)
    pci_topology_notes: list[str] = field(default_factory=list)
    identity_notes: list[str] = field(default_factory=list)
    expected_checks: list[ExpectedCheck] = field(default_factory=list)


@dataclass
class PlatformReport:
    schema_version: int
    generated_at_utc: str
    firmware_view: FirmwareView
    linux_view: LinuxView
    expected_platform: ExpectedPlatform
    diagnostics: Diagnostics
