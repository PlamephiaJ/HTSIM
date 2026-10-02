#!/usr/bin/env bash
set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
if ! REPO_ROOT="$(git -C "${SCRIPT_DIR}" rev-parse --show-toplevel 2>/dev/null)"; then
    echo "error: could not locate the HTSIM repository root from ${SCRIPT_DIR}" >&2
    exit 2
fi
CONFIG_FILE="${1:-${SCRIPT_DIR}/config.env}"
SIM_EXTRA_ARGS=()

if [[ ! -f "${CONFIG_FILE}" ]]; then
    echo "error: config file not found: ${CONFIG_FILE}" >&2
    exit 2
fi

# shellcheck source=config.env
source "${CONFIG_FILE}"

: "${ROUTING:?ROUTING is required}"
: "${SENDER_CC_ALGO:?SENDER_CC_ALGO is required}"
: "${LOAD_BALANCING_ALGO:?LOAD_BALANCING_ALGO is required}"
: "${PATH_ENTROPY_SIZE:?PATH_ENTROPY_SIZE is required}"
: "${QUEUE_PACKETS:?QUEUE_PACKETS is required}"
: "${CWND_BYTES:?CWND_BYTES is required}"
: "${WORKLOAD:?WORKLOAD is required}"
: "${TOPOLOGY:?TOPOLOGY is required}"
: "${BINARY:?BINARY is required}"

resolve_from_repo() {
    if [[ "$1" = /* ]]; then
        printf '%s\n' "$1"
    else
        printf '%s/%s\n' "${REPO_ROOT}" "$1"
    fi
}

WORKLOAD_PATH="$(resolve_from_repo "${WORKLOAD}")"
TOPOLOGY_PATH="$(resolve_from_repo "${TOPOLOGY}")"
BINARY_PATH="$(resolve_from_repo "${BINARY}")"
SUMMARIZER_PATH="${REPO_ROOT}/experiments/common/summarize.py"
SLACK_ANALYZER_PATH="${REPO_ROOT}/experiments/common/analyze_slack.py"
RANK_PLACEMENT_PATH=""
MESSAGELET_CONFIG_PATH=""

if [[ -n "${RANK_PLACEMENT_CONFIG:-}" ]]; then
    RANK_PLACEMENT_PATH="$(resolve_from_repo "${RANK_PLACEMENT_CONFIG}")"
fi
if [[ -n "${MESSAGELET_CONFIG:-}" ]]; then
    MESSAGELET_CONFIG_PATH="$(resolve_from_repo "${MESSAGELET_CONFIG}")"
fi

if [[ ! -f "${WORKLOAD_PATH}" ]]; then
    echo "error: workload not found: ${WORKLOAD_PATH}" >&2
    exit 2
fi
if [[ ! -f "${TOPOLOGY_PATH}/dragonfly.topo" ]]; then
    echo "error: Dragonfly topology not found: ${TOPOLOGY_PATH}" >&2
    exit 2
fi
if [[ -n "${RANK_PLACEMENT_PATH}" && ! -f "${RANK_PLACEMENT_PATH}" ]]; then
    echo "error: rank placement config not found: ${RANK_PLACEMENT_PATH}" >&2
    exit 2
fi
if [[ -n "${MESSAGELET_CONFIG_PATH}" && ! -f "${MESSAGELET_CONFIG_PATH}" ]]; then
    echo "error: messagelet config not found: ${MESSAGELET_CONFIG_PATH}" >&2
    exit 2
fi
if [[ ! -x "${SUMMARIZER_PATH}" ]]; then
    echo "error: shared summarizer not found: ${SUMMARIZER_PATH}" >&2
    exit 2
fi
if [[ ! -f "${SLACK_ANALYZER_PATH}" ]]; then
    echo "error: shared slack analyzer not found: ${SLACK_ANALYZER_PATH}" >&2
    exit 2
fi

if [[ ! -x "${BINARY_PATH}" ]]; then
    if [[ "${BUILD_IF_MISSING:-0}" != 1 ]]; then
        echo "error: simulator binary not found: ${BINARY_PATH}" >&2
        exit 2
    fi
    echo "Building htsim_uec_df ..."
    cmake -S "${REPO_ROOT}/htsim/sim" -B "${REPO_ROOT}/htsim/sim/build"
    cmake --build "${REPO_ROOT}/htsim/sim/build" --target htsim_uec_df --parallel
fi

run_id="$(date +%Y%m%d-%H%M%S)"
run_dir="${SCRIPT_DIR}/artifacts/${run_id}"
suffix=1
while [[ -e "${run_dir}" ]]; do
    run_dir="${SCRIPT_DIR}/artifacts/${run_id}-$(printf '%02d' "${suffix}")"
    ((suffix += 1))
done

mkdir -p "${run_dir}/config_snapshot" "${run_dir}/output_metrics"
cp "${CONFIG_FILE}" "${run_dir}/config_snapshot/config.env"
cp "${SCRIPT_DIR}/run.sh" "${run_dir}/config_snapshot/run.sh"
cp "${SUMMARIZER_PATH}" "${run_dir}/config_snapshot/summarize.py"
cp "${SLACK_ANALYZER_PATH}" "${run_dir}/config_snapshot/analyze_slack.py"
cp "${WORKLOAD_PATH}" "${run_dir}/config_snapshot/workload.bin"
if [[ -f "${WORKLOAD_PATH%.bin}.goal" ]]; then
    cp "${WORKLOAD_PATH%.bin}.goal" "${run_dir}/config_snapshot/workload.goal"
fi
if [[ -f "${WORKLOAD_PATH%.bin}.meta.json" ]]; then
    cp "${WORKLOAD_PATH%.bin}.meta.json" "${run_dir}/config_snapshot/workload.meta.json"
fi
cp -a "${TOPOLOGY_PATH}" "${run_dir}/config_snapshot/topology"

command=(
    "${BINARY_PATH}"
    -basepath "${run_dir}/config_snapshot/topology"
    -goal "${run_dir}/config_snapshot/workload.bin"
    -routing "${ROUTING}"
    -sender_cc_algo "${SENDER_CC_ALGO}"
    -load_balancing_algo "${LOAD_BALANCING_ALGO}"
    -paths "${PATH_ENTROPY_SIZE}"
    -q "${QUEUE_PACKETS}"
    -cwnd "${CWND_BYTES}"
)

if [[ -n "${RANK_PLACEMENT_PATH}" ]]; then
    cp "${RANK_PLACEMENT_PATH}" "${run_dir}/config_snapshot/rank_placement.json"
    command+=( -rank_placement "${run_dir}/config_snapshot/rank_placement.json" )
fi
if [[ -n "${MESSAGELET_CONFIG_PATH}" ]]; then
    cp "${MESSAGELET_CONFIG_PATH}" "${run_dir}/config_snapshot/messagelet_config.json"
    command+=( -messagelet_config "${run_dir}/config_snapshot/messagelet_config.json" )
fi
command+=("${SIM_EXTRA_ARGS[@]}")

printf '%q ' "${command[@]}" > "${run_dir}/command.txt"
printf '\n' >> "${run_dir}/command.txt"
date --iso-8601=seconds > "${run_dir}/started_at.txt"
git -C "${REPO_ROOT}" rev-parse HEAD > "${run_dir}/git-revision.txt" 2>/dev/null || true
git -C "${REPO_ROOT}" status --short > "${run_dir}/git-status.txt" 2>/dev/null || true

echo "Run directory: ${run_dir}"
echo "Routing: ${ROUTING}"
echo "Command: $(<"${run_dir}/command.txt")"

set +e
(
    cd "${run_dir}"
    HTSIM_TRACE_FLOW_COMPLETIONS="${TRACE_FLOW_COMPLETIONS:-0}" \
        "${command[@]}" 2>&1 | tee simulator.log
    exit "${PIPESTATUS[0]}"
)
status=$?
set -e

date --iso-8601=seconds > "${run_dir}/finished_at.txt"
printf '%s\n' "${status}" > "${run_dir}/exit_code.txt"
python3 "${run_dir}/config_snapshot/summarize.py" "${run_dir}"
analysis_status=0
if (( status == 0 )); then
    if python3 "${run_dir}/config_snapshot/analyze_slack.py" \
        "${run_dir}" \
        --ready-window-ns "${SLACK_READY_WINDOW_NS:-100}"; then
        :
    else
        analysis_status=$?
    fi
fi
ln -sfn "$(basename "${run_dir}")" "${SCRIPT_DIR}/artifacts/latest"

echo
echo "Final summary: ${run_dir}/summary.md"
cat "${run_dir}/summary.md"
echo
if (( status == 0 && analysis_status == 0 )); then
    echo "Slack analysis: ${run_dir}/slack_analysis.md"
    echo
fi

if (( status != 0 )); then
    echo "Experiment failed with exit code ${status}; see ${run_dir}/simulator.log" >&2
    exit "${status}"
fi
if (( analysis_status != 0 )); then
    echo "Experiment completed, but slack analysis failed with exit code ${analysis_status}" >&2
    exit "${analysis_status}"
fi

echo "Experiment completed: ${run_dir}"
