from __future__ import annotations

import json
import os
import re
import shutil
import socket
import subprocess
from pathlib import Path

from .models import ArtifactStatus, CommandSpec
from .utils import utc_timestamp


def can_use_passwordless_sudo() -> bool:
    if os.geteuid() == 0 or shutil.which("sudo") is None:
        return False

    result = subprocess.run(
        ["sudo", "-n", "true"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        check=False,
    )
    return result.returncode == 0


def write_status(output_dir: Path, status: ArtifactStatus) -> None:
    status_path = output_dir / f"{status.name}.status"
    lines = [status.status, status.detail]
    if status.path is not None:
        lines.append(status.path)
    status_path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def capture_file(output_dir: Path, name: str, source: Path) -> ArtifactStatus:
    destination = output_dir / f"{name}.txt"
    if not source.exists():
        status = ArtifactStatus(name=name, status="missing", detail=f"{source} does not exist")
        write_status(output_dir, status)
        return status

    if not os.access(source, os.R_OK):
        status = ArtifactStatus(name=name, status="failed", detail=f"{source} is not readable")
        write_status(output_dir, status)
        return status

    shutil.copyfile(source, destination)
    status = ArtifactStatus(name=name, status="ok", detail=str(source), path=str(destination))
    write_status(output_dir, status)
    return status


def capture_command(output_dir: Path, spec: CommandSpec, use_sudo: bool) -> ArtifactStatus:
    executable = spec.argv[0]
    destination = output_dir / f"{spec.name}.txt"
    error_path = output_dir / f"{spec.name}.err"

    if shutil.which(executable) is None:
        status = ArtifactStatus(
            name=spec.name,
            status="missing",
            detail=f"command not found: {executable}",
        )
        write_status(output_dir, status)
        return status

    argv = list(spec.argv)
    if spec.privileged and os.geteuid() != 0 and use_sudo:
        argv = ["sudo", "-n", *argv]

    with destination.open("w", encoding="utf-8", errors="replace") as stdout_file:
        with error_path.open("w", encoding="utf-8", errors="replace") as stderr_file:
            result = subprocess.run(argv, stdout=stdout_file, stderr=stderr_file, check=False)

    if result.returncode == 0:
        status = ArtifactStatus(
            name=spec.name,
            status="ok",
            detail=" ".join(argv),
            path=str(destination),
        )
    else:
        status = ArtifactStatus(
            name=spec.name,
            status="failed",
            detail=f"exit={result.returncode} command={' '.join(argv)}",
            path=str(destination),
        )

    write_status(output_dir, status)
    return status


def collect_linux_view(output_dir: Path) -> list[ArtifactStatus]:
    output_dir.mkdir(parents=True, exist_ok=True)
    use_sudo = can_use_passwordless_sudo()
    statuses: list[ArtifactStatus] = []

    manifest = {
        "timestampUtc": utc_timestamp(),
        "hostname": socket.gethostname(),
        "kernel": " ".join(os.uname()),
        "outputRoot": str(output_dir),
        "passwordlessSudo": use_sudo,
    }
    (output_dir / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")

    statuses.append(capture_file(output_dir, "proc_iomem", Path("/proc/iomem")))

    commands = [
        CommandSpec("dmesg", ("dmesg",), privileged=True),
        CommandSpec("dmidecode", ("dmidecode",), privileged=True),
        CommandSpec("lspci_vvv", ("lspci", "-vvv")),
        CommandSpec("lspci_tree", ("lspci", "-tv")),
        CommandSpec("lscpu", ("lscpu",)),
        CommandSpec("numactl_hardware", ("numactl", "-H")),
    ]

    for spec in commands:
        statuses.append(capture_command(output_dir, spec, use_sudo))

    dmesg_path = output_dir / "dmesg.txt"
    e820_path = output_dir / "e820.txt"
    if dmesg_path.exists() and dmesg_path.stat().st_size > 0:
        e820_lines = [
            line
            for line in dmesg_path.read_text(encoding="utf-8", errors="replace").splitlines()
            if re.search(r"e820|BIOS-e820|efi: mem|Memory:|NUMA", line, flags=re.IGNORECASE)
        ]
        e820_path.write_text("\n".join(e820_lines) + ("\n" if e820_lines else ""), encoding="utf-8")
        e820_status = ArtifactStatus("e820", "ok", "derived from dmesg.txt", str(e820_path))
    else:
        e820_status = ArtifactStatus("e820", "failed", "dmesg.txt is empty or unavailable")

    write_status(output_dir, e820_status)
    statuses.append(e820_status)
    return statuses

