#!/usr/bin/env bash
set -Eeuo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

SOURCE="${SCRIPT_DIR}/experiments"
DEST="gdrive:ExpData/experiments"

# 检查源目录
if [[ ! -d "$SOURCE" ]]; then
    echo "ERROR: Source directory not found: $SOURCE"
    exit 1
fi

echo "========================================"
echo "Source: $SOURCE"
echo "Target: $DEST"
echo "Start:  $(date '+%Y-%m-%d %H:%M:%S')"
echo "========================================"

rclone sync "$SOURCE" "$DEST" \
    --transfers 8 \
    --checkers 16 \
    --fast-list \
    --skip-links \
    --delete-after \
    --progress \
    --stats 15s \
    --retries 3 \
    "$@"

echo "========================================"
echo "Sync completed: $(date '+%Y-%m-%d %H:%M:%S')"
echo "========================================"

