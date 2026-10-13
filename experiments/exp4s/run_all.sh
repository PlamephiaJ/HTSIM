#!/usr/bin/env bash
set -Eeuo pipefail
suite_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(git -C "${suite_dir}" rev-parse --show-toplevel)"
python3 "${suite_dir}/generate.py"
cmake -S "${repo_root}/htsim/sim" -B "${repo_root}/htsim/sim/build"
cmake --build "${repo_root}/htsim/sim/build" --target htsim_uec_df --parallel 4
mkdir -p "${suite_dir}/artifacts"
batch_dir="$(mktemp -d "${suite_dir}/artifacts/$(date +%Y%m%d-%H%M%S)-XXXXXX")"
cp "${suite_dir}/compare.py" "${batch_dir}/compare.py"
cp "${suite_dir}/run_all.sh" "${batch_dir}/run_all.sh"
compare_args=()
for case in exp4.ref1 exp4.ref2 exp4.1 exp4.2 exp4.3 exp4.4; do
    echo "Running ${case} ..."
    "${suite_dir}/${case}/run.sh" > "${batch_dir}/${case}.log" 2>&1
    artifact="$(readlink -f "${suite_dir}/${case}/artifacts/latest")"
    compare_args+=( "--${case#exp4.}" "${artifact}" )
done
python3 "${suite_dir}/compare.py" "${compare_args[@]}" --output-dir "${batch_dir}"
ln -sfn "$(basename "${batch_dir}")" "${suite_dir}/artifacts/latest"
cat "${batch_dir}/comparison.md"
