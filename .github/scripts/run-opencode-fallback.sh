#!/usr/bin/env bash
# ============================================================================
# run-opencode-fallback.sh -- run the agent with an automatic model fallback
# ----------------------------------------------------------------------------
# OpenCode's free models share a usage limit.  When the current model returns
# a 429 / FreeUsageLimitError, retrying the same model can sit in a retry
# loop for hours.  This wrapper tries the models in order and moves to the
# next one as soon as a model is rate limited (or an attempt times out).
#
# It restarts the GitHub task with the next model; it does not resume the
# model's in-memory session.  The issue/PR context is still the same, so the
# new model can pick up the work from the repository and the thread.
#
# Environment:
#   OPENCODE_ATTEMPT_TIMEOUT  per-model timeout (default: 20m)
#   OPENCODE_MODELS           space-separated override of the model list
# ============================================================================
set -uo pipefail

read -r -a models <<< "${OPENCODE_MODELS:-opencode/mimo-v2.6-flash-free opencode/glm-5.3-flash-free opencode/big-pickle opencode/flint-40b-free}"
attempt_timeout="${OPENCODE_ATTEMPT_TIMEOUT:-20m}"

for model in "${models[@]}"; do
  echo "::group::opencode model: ${model}"
  log="$(mktemp)"

  set +e
  timeout "${attempt_timeout}" env MODEL="${model}" opencode github run 2>&1 | tee "${log}"
  status=${PIPESTATUS[0]}
  set -e

  echo "::endgroup::"

  if [ "${status}" -eq 0 ]; then
    rm -f "${log}"
    exit 0
  fi

  if [ "${status}" -eq 124 ] || grep -qiE \
      'FreeUsageLimitError|free usage limit|usage limit reached|retry-after|statusCode: *429|429 Too Many' \
      "${log}"; then
    echo "::warning::model ${model} is rate limited or timed out; switching to the next model"
    rm -f "${log}"
    continue
  fi

  echo "::error::model ${model} failed with status ${status}"
  cat "${log}"
  rm -f "${log}"
  exit "${status}"
done

echo "::error::every fallback model failed"
exit 1
