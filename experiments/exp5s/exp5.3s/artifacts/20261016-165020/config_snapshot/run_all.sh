#!/usr/bin/env bash
set -Eeuo pipefail
suite_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
exec python3 -B "${suite_dir}/run_sweep.py" "$@"
