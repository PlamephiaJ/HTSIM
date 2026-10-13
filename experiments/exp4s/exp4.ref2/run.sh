#!/usr/bin/env bash
set -Eeuo pipefail
case_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
exec "${case_dir}/../run_case.sh" "${1:-${case_dir}/config.env}"
