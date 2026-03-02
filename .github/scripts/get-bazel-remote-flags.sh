#!/usr/bin/env bash
set -euo pipefail

if [[ -n "${BUILDBUDDY_API_KEY:-}" ]]; then
  echo "Using BuildBuddy Remote Cache with --config=remote" >&2
  echo "--config=remote"
  echo "--remote_header=x-buildbuddy-api-key=${BUILDBUDDY_API_KEY}"
fi
