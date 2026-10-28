#!/usr/bin/env bash
# ============================================================================
# run-opencode-fallback.sh -- run opencode with automatic model fallback
# ----------------------------------------------------------------------------
# Tries the free models in order.  When a model stalls (no output for
# OPENCODE_STALL_TIMEOUT seconds), is rate-limited (429 / FreeUsageLimitError),
# or exceeds the per-model attempt timeout, the wrapper kills it and moves to
# the next model.  It never sits idle for the whole job timeout.
#
# The agent runs with `--auto` so every permission (gh, git, ...) is pre-approved
# in the headless runner.  It is expected to use `gh` itself to comment on the
# issue/PR it is handling; this wrapper only drives the model and falls back.
#
# Environment:
#   OPENCODE_ATTEMPT_TIMEOUT  total seconds per model (default: 1200)
#   OPENCODE_STALL_TIMEOUT    seconds without output before switching (default: 120)
#   OPENCODE_MODELS           space-separated override of the model list
#   TRIAGE_PROMPT             base instructions (set by the workflow)
#   GITHUB_EVENT_PATH         path to the webhook event JSON (issue/PR context)
#   GITHUB_TOKEN / GH_TOKEN   token opencode uses to call the GitHub API
#   OPENCODE_API_KEY          if set, keyed models (gemini, chatgpt, ...) work
# ============================================================================
set -uo pipefail

read -r -a models <<< "${OPENCODE_MODELS:-opencode/glm-5.3-flash-free opencode/flint-40b-free opencode/beam-free opencode/deepseek-v4.1-flash-free opencode/mimo-v2.6-flash-free}"
attempt_seconds="${OPENCODE_ATTEMPT_TIMEOUT:-1200}"
stall_seconds="${OPENCODE_STALL_TIMEOUT:-120}"

# --- extract issue/PR context and the user's message from the webhook event --
event_path="${GITHUB_EVENT_PATH:-}"
issue_number=""
pr_number=""
user_message=""

if [ -n "$event_path" ] && [ -f "$event_path" ] && command -v jq >/dev/null 2>&1; then
  issue_number="$(jq -r '.issue.number // .pull_request.number // empty' "$event_path" 2>/dev/null || true)"
  pr_number="$(jq -r '.pull_request.number // empty' "$event_path" 2>/dev/null || true)"
  user_message="$(jq -r '.comment.body // .issue.body // empty' "$event_path" 2>/dev/null || true)"
fi

# --- build the full prompt --------------------------------------------------
prompt_parts=()
if [ -n "${TRIAGE_PROMPT:-}" ]; then
  prompt_parts+=("$TRIAGE_PROMPT")
fi
if [ -n "$user_message" ]; then
  prompt_parts+=("\n---\nUser message:\n$user_message")
fi
if [ -n "$issue_number" ]; then
  prompt_parts+=("\nIssue number: $issue_number")
fi
if [ -n "$pr_number" ]; then
  prompt_parts+=("\nPull request number: $pr_number")
fi
full_prompt="$(printf '%s' "${prompt_parts[@]}")"

# --- run one model ----------------------------------------------------------
run_attempt() {
  local model="$1"
  local log="$2"
  local pid started now last_change last_size size status killed=0

  : > "$log"
  # --standalone runs a private model server so CI does not need a background
  # opencode service.  --auto approves every permission headless.  The message
  # is a positional argument (opencode run has no --prompt flag).
  opencode run --standalone --model "$model" --auto "$full_prompt" >"$log" 2>&1 &
  pid=$!

  started=$(date +%s)
  last_change=$started
  last_size=0
  status=1

  while kill -0 "$pid" 2>/dev/null; do
    sleep 2
    now=$(date +%s)
    size=$(stat -c %s "$log" 2>/dev/null || echo 0)
    if [ "$size" -ne "$last_size" ]; then
      last_size=$size
      last_change=$now
    fi
    if [ $((now - last_change)) -ge "$stall_seconds" ]; then
      echo "::warning::${model} produced no output for ${stall_seconds}s; switching model"
      kill -TERM "$pid" 2>/dev/null || true
      killed=1
      break
    fi
    if [ $((now - started)) -ge "$attempt_seconds" ]; then
      echo "::warning::${model} exceeded ${attempt_seconds}s; switching model"
      kill -TERM "$pid" 2>/dev/null || true
      killed=1
      break
    fi
  done

  wait "$pid" 2>/dev/null
  status=$?
  if [ "$killed" -eq 1 ]; then
    status=124
  fi
  return "$status"
}

# --- post the agent's reply back to the issue/PR ----------------------------
post_reply() {
  local log="$1"
  local target num
  if [ -n "$pr_number" ]; then
    target="pr"; num="$pr_number"
  elif [ -n "$issue_number" ]; then
    target="issue"; num="$issue_number"
  else
    return 0
  fi
  if [ -s "$log" ]; then
    gh "$target" comment "$num" --body-file "$log" >/dev/null 2>&1 || true
  fi
}

# --- main fallback loop -----------------------------------------------------
for model in "${models[@]}"; do
  echo "::group::opencode model: ${model}"
  log="$(mktemp)"

  set +e
  run_attempt "$model" "$log"
  status=$?
  set -e

  echo "::endgroup::"

  if [ "$status" -eq 0 ]; then
    echo "::notice::model ${model} succeeded"
    post_reply "$log"
    rm -f "$log"
    exit 0
  fi

  if [ "$status" -eq 124 ] || grep -qiE \
      'FreeUsageLimitError|free usage limit|usage limit reached|retry-after|429 Too Many|statusCode: *429|model not found|403' "$log"; then
    echo "::warning::model ${model} is rate limited or stalled; switching to the next model"
    rm -f "$log"
    continue
  fi

  echo "::error::model ${model} failed with status ${status}"
  tail -n 80 "$log"
  rm -f "$log"
  exit "$status"
done

echo "::error::every fallback model failed"
exit 1
