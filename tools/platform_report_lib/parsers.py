from __future__ import annotations

import re
import json
from pathlib import Path

from .models import ArtifactStatus, FirmwareView, LinuxView
from .utils import read_text_if_exists


def read_statuses(artifact_dir: Path) -> list[ArtifactStatus]:
    statuses: list[ArtifactStatus] = []
    for path in sorted(artifact_dir.glob("*.status")):
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
        statuses.append(
            ArtifactStatus(
                name=path.stem,
                status=lines[0] if len(lines) > 0 else "unknown",
                detail=lines[1] if len(lines) > 1 else "",
                path=lines[2] if len(lines) > 2 else None,
            )
        )
    return statuses


def parse_key_value_lines(text: str) -> dict[str, str]:
    values: dict[str, str] = {}
    for line in text.splitlines():
        if ":" not in line:
            continue
        key, value = line.split(":", 1)
        values[key.strip()] = value.strip()
    return values


def parse_lspci_devices(text: str) -> list[str]:
    devices: list[str] = []
    for line in text.splitlines():
        if re.match(r"^[0-9a-fA-F:.]+\s", line):
            devices.append(line.strip())
    return devices


def parse_dmidecode_identity(text: str) -> dict[str, str]:
    identity: dict[str, str] = {}
    current_section = ""

    for line in text.splitlines():
        stripped = line.strip()
        if stripped in {"BIOS Information", "System Information", "Base Board Information"}:
            current_section = stripped
            continue

        if not current_section or ":" not in stripped:
            continue

        key, value = stripped.split(":", 1)
        normalized_key = f"{current_section}.{key.strip()}".replace(" ", "")
        identity[normalized_key] = value.strip()

    return identity


def build_linux_view(artifact_dir: Path) -> LinuxView:
    proc_iomem = read_text_if_exists(artifact_dir / "proc_iomem.txt")
    e820 = read_text_if_exists(artifact_dir / "e820.txt")
    lspci = read_text_if_exists(artifact_dir / "lspci_vvv.txt")
    lscpu = read_text_if_exists(artifact_dir / "lscpu.txt")
    numa = read_text_if_exists(artifact_dir / "numactl_hardware.txt")
    dmidecode = read_text_if_exists(artifact_dir / "dmidecode.txt")

    return LinuxView(
        artifact_dir=str(artifact_dir),
        statuses=read_statuses(artifact_dir),
        iomem_regions=[line for line in proc_iomem.splitlines() if line.strip()],
        e820_lines=[line for line in e820.splitlines() if line.strip()],
        pci_devices=parse_lspci_devices(lspci),
        cpu_summary=parse_key_value_lines(lscpu),
        numa_summary=[line for line in numa.splitlines() if line.strip()],
        smbios_identity=parse_dmidecode_identity(dmidecode),
    )


def build_firmware_view(firmware_json: Path | None) -> FirmwareView:
    # Phase 1 writes firmware-view.json on the EFI system partition. Phase 3
    # treats that file as firmware's view of the same machine that Linux just
    # booted on.
    if firmware_json is None:
        return FirmwareView(error="firmware JSON was not provided")

    if not firmware_json.exists():
        return FirmwareView(
            source_path=str(firmware_json),
            error=f"{firmware_json} does not exist",
        )

    try:
        data = json.loads(firmware_json.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        return FirmwareView(
            source_path=str(firmware_json),
            error=f"invalid JSON: {exc}",
        )

    return FirmwareView(
        source_path=str(firmware_json),
        loaded=True,
        data=data,
    )
