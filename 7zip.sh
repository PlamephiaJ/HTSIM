#!/usr/bin/env bash

if [ "$#" -eq 0 ]; then
    echo "用法: $0 <文件或目录> [更多文件或目录...]"
    exit 1
fi

if command -v 7z >/dev/null 2>&1; then
    cmd=7z
elif command -v 7zz >/dev/null 2>&1; then
    cmd=7zz
else
    echo "错误：未安装 7-Zip"
    exit 1
fi

status=0

for target in "$@"; do
    if [[ ! -e "$target" ]]; then
        echo "错误：路径不存在 $target"
        status=1
        continue
    fi

    dir=$(dirname -- "$target")
    name=$(basename -- "$target")

    (
        cd -- "$dir" || exit 1

        if [[ -e "./${name}.7z" ]]; then
            echo "错误：压缩包已存在 ${name}.7z"
            exit 1
        fi

        "$cmd" a \
            -t7z \
            -mx=9 \
            -m0=LZMA2 \
            -ms=on \
            -mmt=on \
            "./${name}.7z" \
            "./${name}"
    ) || status=1
done

exit "$status"
