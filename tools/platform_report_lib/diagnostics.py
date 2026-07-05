from __future__ import annotations

from typing import Any

from .models import Diagnostics, ExpectedCheck, ExpectedPlatform, FirmwareView, LinuxView


def _firmware_acpi_tables(firmware_view: FirmwareView) -> list[str]:
    acpi = firmware_view.data.get("acpi", {})
    if not isinstance(acpi, dict):
        return []

    tables = acpi.get("tables", [])
    if not isinstance(tables, list):
        return []

    return [table for table in tables if isinstance(table, str)]


def _firmware_value(firmware_view: FirmwareView, *keys: str) -> Any:
    value: Any = firmware_view.data
    for key in keys:
        if not isinstance(value, dict):
            return None
        value = value.get(key)
    return value


def _linux_artifact_statuses(linux_view: LinuxView) -> dict[str, str]:
    return {status.name: status.status for status in linux_view.statuses}


def _firmware_memory_mib(firmware_view: FirmwareView) -> int | None:
    memory_map = _firmware_value(firmware_view, "memoryMap")
    if not isinstance(memory_map, dict):
        return None

    usable_pages = memory_map.get("usablePages")
    if not isinstance(usable_pages, int):
        return None

    # UEFI memory descriptors count 4 KiB pages. Convert only the usable RAM
    # pages into MiB so expected-platform can express a simple capacity floor.
    return usable_pages * 4096 // (1024 * 1024)


def _build_expected_checks(
    linux_view: LinuxView,
    firmware_view: FirmwareView,
    expected_platform: ExpectedPlatform,
) -> list[ExpectedCheck]:
    checks: list[ExpectedCheck] = []

    if not expected_platform.loaded:
        checks.append(
            ExpectedCheck(
                status="WARN",
                name="expected_platform_loaded",
                detail=f"expected platform YAML not loaded: {expected_platform.error}",
            )
        )
        return checks

    if expected_platform.platform_id is not None:
        observed_platform_id = _firmware_value(firmware_view, "uefiBootLab", "platformId")
        if not firmware_view.loaded:
            checks.append(
                ExpectedCheck(
                    status="WARN",
                    name="platform_id",
                    detail="firmware JSON not provided, cannot validate platform_id",
                )
            )
        elif observed_platform_id == expected_platform.platform_id:
            checks.append(
                ExpectedCheck(
                    status="PASS",
                    name="platform_id",
                    detail=f"firmware producer platform_id is {expected_platform.platform_id}",
                )
            )
        else:
            checks.append(
                ExpectedCheck(
                    status="FAIL",
                    name="platform_id",
                    detail=f"expected {expected_platform.platform_id}, observed {observed_platform_id or 'missing'}",
                )
            )

    expected_vendors = list(expected_platform.expected_firmware_vendors)
    if expected_platform.expected_firmware_vendor is not None:
        expected_vendors.append(expected_platform.expected_firmware_vendor)

    if expected_vendors:
        observed_vendor = _firmware_value(firmware_view, "firmware", "vendor")
        if not firmware_view.loaded:
            checks.append(
                ExpectedCheck(
                    status="WARN",
                    name="expected_firmware_vendor",
                    detail="firmware JSON not provided, cannot validate firmware vendor",
                )
            )
        elif observed_vendor in expected_vendors:
            checks.append(
                ExpectedCheck(
                    status="PASS",
                    name="expected_firmware_vendor",
                    detail=f"firmware vendor is {observed_vendor}",
                )
            )
        else:
            checks.append(
                ExpectedCheck(
                    status="FAIL",
                    name="expected_firmware_vendor",
                    detail=f"expected one of {', '.join(expected_vendors)}, observed {observed_vendor or 'missing'}",
                )
            )

    acpi_tables = set(_firmware_acpi_tables(firmware_view))
    for table in expected_platform.required_acpi_tables:
        if not firmware_view.loaded:
            checks.append(
                ExpectedCheck(
                    status="WARN",
                    name=f"required_acpi_table.{table}",
                    detail="firmware JSON not provided, cannot validate ACPI table",
                )
            )
        elif table in acpi_tables:
            checks.append(
                ExpectedCheck(
                    status="PASS",
                    name=f"required_acpi_table.{table}",
                    detail=f"required ACPI table {table} present",
                )
            )
        else:
            checks.append(
                ExpectedCheck(
                    status="FAIL",
                    name=f"required_acpi_table.{table}",
                    detail=f"required ACPI table {table} missing",
                )
            )

    artifact_statuses = _linux_artifact_statuses(linux_view)
    for artifact in expected_platform.required_linux_artifacts:
        status = artifact_statuses.get(artifact)
        if status == "ok":
            checks.append(
                ExpectedCheck(
                    status="PASS",
                    name=f"required_linux_artifact.{artifact}",
                    detail=f"Linux {artifact} collected",
                )
            )
        elif status is None:
            checks.append(
                ExpectedCheck(
                    status="FAIL",
                    name=f"required_linux_artifact.{artifact}",
                    detail=f"Linux {artifact} status file missing",
                )
            )
        else:
            checks.append(
                ExpectedCheck(
                    status="FAIL",
                    name=f"required_linux_artifact.{artifact}",
                    detail=f"Linux {artifact} status is {status}",
                )
            )

    if expected_platform.min_memory_mib is not None:
        memory_mib = _firmware_memory_mib(firmware_view)
        if not firmware_view.loaded:
            checks.append(
                ExpectedCheck(
                    status="WARN",
                    name="min_memory_mib",
                    detail="firmware JSON not provided, cannot validate firmware memory size",
                )
            )
        elif memory_mib is None:
            checks.append(
                ExpectedCheck(
                    status="FAIL",
                    name="min_memory_mib",
                    detail="firmware memory summary missing usablePages",
                )
            )
        elif memory_mib >= expected_platform.min_memory_mib:
            checks.append(
                ExpectedCheck(
                    status="PASS",
                    name="min_memory_mib",
                    detail=f"usable firmware memory {memory_mib} MiB >= expected {expected_platform.min_memory_mib} MiB",
                )
            )
        else:
            checks.append(
                ExpectedCheck(
                    status="FAIL",
                    name="min_memory_mib",
                    detail=f"usable firmware memory {memory_mib} MiB < expected {expected_platform.min_memory_mib} MiB",
                )
            )

    if expected_platform.require_smbios:
        if linux_view.smbios_identity:
            checks.append(
                ExpectedCheck(
                    status="PASS",
                    name="require_smbios",
                    detail="Linux SMBIOS identity parsed from dmidecode",
                )
            )
        else:
            checks.append(
                ExpectedCheck(
                    status="FAIL",
                    name="require_smbios",
                    detail="SMBIOS identity missing from Linux dmidecode output",
                )
            )

    return checks


def build_diagnostics(
    linux_view: LinuxView,
    firmware_view: FirmwareView,
    expected_platform: ExpectedPlatform,
) -> Diagnostics:
    missing = [status.name for status in linux_view.statuses if status.status == "missing"]
    failed = [status.name for status in linux_view.statuses if status.status == "failed"]
    missing_acpi: list[str] = []
    suspicious_memory: list[str] = []
    pci_notes: list[str] = []
    identity_notes: list[str] = []

    if not linux_view.iomem_regions:
        suspicious_memory.append("/proc/iomem was not collected or was empty")

    if not linux_view.e820_lines:
        suspicious_memory.append("No e820 or EFI memory lines were found in dmesg")

    if not linux_view.pci_devices:
        pci_notes.append("No PCI devices parsed from lspci -vvv output")

    if not linux_view.smbios_identity:
        identity_notes.append("No SMBIOS identity parsed from dmidecode output")

    # ACPI table checks connect Phase 1 and Phase 2: FirmwareView records the
    # tables OVMF exposed before Linux booted, and Linux should later explain
    # CPU/interrupt/PCI behavior through those tables.
    if not firmware_view.loaded:
        identity_notes.append(f"Firmware view was not loaded: {firmware_view.error}")
    else:
        acpi_tables = set(_firmware_acpi_tables(firmware_view))
        for table in ("FACP", "APIC", "MCFG"):
            if table not in acpi_tables:
                missing_acpi.append(table)

        firmware_vendor = _firmware_value(firmware_view, "firmware", "vendor")
        bios_vendor = linux_view.smbios_identity.get("BIOSInformation.Vendor")
        if firmware_vendor and bios_vendor and str(firmware_vendor) not in bios_vendor:
            identity_notes.append(
                f"Firmware vendor '{firmware_vendor}' differs from Linux SMBIOS BIOS vendor '{bios_vendor}'"
            )

    return Diagnostics(
        missing_artifacts=missing,
        failed_artifacts=failed,
        missing_acpi_tables=missing_acpi,
        suspicious_memory_ranges=suspicious_memory,
        pci_topology_notes=pci_notes,
        identity_notes=identity_notes,
        expected_checks=_build_expected_checks(linux_view, firmware_view, expected_platform),
    )
