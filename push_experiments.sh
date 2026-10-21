#!/usr/bin/env bash
set -euo pipefail

HF_BUCKET="hf://buckets/Plamephia/DAG-routing-exps"

hf buckets sync ./experiments "$HF_BUCKET"
