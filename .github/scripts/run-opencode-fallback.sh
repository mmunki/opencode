#!/usr/bin/env bash
# ============================================================================
# run-opencode-fallback.sh -- run the agent with an automatic model fallback
# ----------------------------------------------------------------------------
# OpenCode's free models share a usage limit.  When the current model returns
# a 429 / FreeUsageLimitError, retrying the same model can sit in a retry
# loop for hours.  A model can also accept the request and then never produce
# a token.  This wrapper detects both cases:
#
#   * no output for OPENCODE_STALL_TIMEOUT seconds -> kill and try next model
#   * the whole attempt runs longer than OPENCODE_ATTEMPT_TIMEOUT -> try next
#   * a 429 / FreeUsageLimitError in the output -> try next immediately
#
# It restarts the GitHub task with the next model; it does not resume the
# model's in-memory session.  The issue/PR context is still the same, so the
# new model can pick up the work from the repository and the thread.
#
# Environment:
#   OPENCODE_ATTEMPT_TIMEOUT  total seconds per model (default: 1200)
#   OPENCODE_STALL_TIMEOUT    seconds without output (default: 120)
#   OPENCODE_MODELS           space-separated override of the model list
# ============================================================================
set -uo pipefail

read -r -a models <<< "${OPENCODE_MODELS:-opencode/glm-5.3-flash-free opencode/flint-40b-free opencode/beam-free opencode/deepseek-v4.1-flash-free opencode/mimo-v2.6-flash-free}"
attempt_seconds="${OPENCODE_ATTEMPT_TIMEOUT:-1200}"
stall_seconds="${OPENCODE_STALL_TIMEOUT:-120}"

# Run one model.  stdout/stderr go to a file so a watchdog can see whether
# the model is producing anything; tail streams the same lines to the job log.
run_attempt() {
  local model="$1"
  local log="$2"
  local pid tailpid started now last_change last_size size status killed=0

  : >"${log}"
  timeout --signal=TERM --kill-after=5s "${attempt_seconds}s" \
    env MODEL="${model}" opencode github run >"${log}" 2>&1 &
  pid=$!
  tail -n +1 -f "${log}" &
  tailpid=$!

  started=$(date +%s)
  last_change=${started}
  last_size=0
  status=1

  while kill -0 "${pid}" 2>/dev/null; do
    sleep 2
    now=$(date +%s)
    size=$(stat -c %s "${log}" 2>/dev/null || echo 0)

    if [ "${size}" -ne "${last_size}" ]; then
      last_size=${size}
      last_change=${now}
    fi

    if [ $((now - last_change)) -ge "${stall_seconds}" ]; then
      echo "::warning::${model} produced no output for ${stall_seconds}s; switching model"
      kill -TERM "${pid}" 2>/dev/null || true
      killed=1
      break
    fi

    if [ $((now - started)) -ge "${attempt_seconds}" ]; then
      echo "::warning::${model} exceeded ${attempt_seconds}s; switching model"
      kill -TERM "${pid}" 2>/dev/null || true
      killed=1
      break
    fi
  done

  wait "${pid}" 2>/dev/null
  status=$?
  if [ "${killed}" -eq 1 ]; then
    status=124
  fi
  sleep 1
  kill "${tailpid}" 2>/dev/null || true
  wait "${tailpid}" 2>/dev/null || true
  return "${status}"
}

for model in "${models[@]}"; do
  echo "::group::opencode model: ${model}"
  log="$(mktemp)"

  set +e
  run_attempt "${model}" "${log}"
  status=$?
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
  tail -n 80 "${log}"
  rm -f "${log}"
  exit "${status}"
done

echo "::error::every fallback model failed"
exit 1
