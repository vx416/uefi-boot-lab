from __future__ import annotations

from pathlib import Path

from .models import ExpectedPlatform


def _parse_bool(value: str) -> bool:
    normalized = value.strip().lower()
    if normalized in {"true", "yes", "1"}:
        return True
    if normalized in {"false", "no", "0"}:
        return False
    raise ValueError(f"invalid boolean value: {value}")


def load_expected_platform(path: Path | None) -> ExpectedPlatform:
    # Keep the expected-platform format intentionally small and dependency-free.
    # It supports only the simple YAML shape used by this lab's validation
    # contract, so the Linux guest does not need PyYAML installed.
    if path is None:
        return ExpectedPlatform(error="expected platform YAML was not provided")

    if not path.exists():
        return ExpectedPlatform(source_path=str(path), error=f"{path} does not exist")

    expected = ExpectedPlatform(source_path=str(path), loaded=True)
    current_list: str | None = None

    try:
        for raw_line in path.read_text(encoding="utf-8").splitlines():
            line = raw_line.split("#", 1)[0].rstrip()
            if not line.strip():
                continue

            if line.startswith("  - "):
                if current_list is None:
                    raise ValueError(f"list item without list key: {raw_line}")
                item = line[4:].strip()
                if current_list == "expected_firmware_vendors":
                    expected.expected_firmware_vendors.append(item)
                elif current_list == "required_acpi_tables":
                    expected.required_acpi_tables.append(item)
                elif current_list == "required_linux_artifacts":
                    expected.required_linux_artifacts.append(item)
                else:
                    raise ValueError(f"unsupported list key: {current_list}")
                continue

            current_list = None
            if ":" not in line:
                raise ValueError(f"invalid expected-platform line: {raw_line}")

            key, value = line.split(":", 1)
            key = key.strip()
            value = value.strip()

            if key in {"expected_firmware_vendors", "required_acpi_tables", "required_linux_artifacts"}:
                if value:
                    raise ValueError(f"{key} must use indented list items")
                current_list = key
            elif key == "platform_id":
                expected.platform_id = value
            elif key == "expected_firmware_vendor":
                expected.expected_firmware_vendor = value
            elif key == "min_memory_mib":
                expected.min_memory_mib = int(value)
            elif key == "require_smbios":
                expected.require_smbios = _parse_bool(value)
            else:
                raise ValueError(f"unsupported expected-platform key: {key}")
    except (OSError, ValueError) as exc:
        return ExpectedPlatform(source_path=str(path), error=str(exc))

    return expected
