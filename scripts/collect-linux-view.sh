#!/usr/bin/env bash
set -euo pipefail

# Compatibility wrapper. The Phase 2 collector and Phase 3 report generator now
# live in one typed Python program.

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
output_root="${LINUX_VIEW_OUT:-}"

if [[ -n "$output_root" ]]; then
  exec python3 "$repo_root/tools/platform_report.py" all --out "$output_root"
fi

exec python3 "$repo_root/tools/platform_report.py" all
