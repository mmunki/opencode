#!/usr/bin/env bash
# ============================================================================
# run-opencode-fallback.sh -- run the agent with model fallback and a test gate
# ----------------------------------------------------------------------------
# The agent is started with the fastest free model first. When a model stalls,
# exceeds its time budget, or hits a provider limit the wrapper moves on to the
# next model instead of failing the whole job.
#
# After a successful run the repository test suite is executed and a failure is
# fed back to the agent, up to OPENCODE_TEST_RETRIES (default 3) times.  A
# repository without a test script has nothing to validate, so the gate is
# skipped instead of failing the job with `npm test --if-present` (which exits
# non-zero when there is no package.json at all).
#
# Environment:
#   PROMPT                    explicit prompt, wins over TRIAGE_PROMPT
#   TRIAGE_PROMPT             default prompt, usually prompts/triage.md
#   OPENCODE_MODELS           space separated fallback chain
#   OPENCODE_ATTEMPT_TIMEOUT  seconds a single run may take (default 1200)
#   OPENCODE_STALL_TIMEOUT    seconds without log output before a run is stuck
#   OPENCODE_TEST_RETRIES     attempts to fix a failing test suite (default 3)
# ============================================================================
set -uo pipefail

read -r -a models <<< "${OPENCODE_MODELS:-opencode/deepseek-v4.1-flash-free opencode/glm-5.3-flash-free opencode/flint-40b-free opencode/beam-free opencode/mimo-v2.6-flash-free}"
attempt_seconds="${OPENCODE_ATTEMPT_TIMEOUT:-1200}"
stall_seconds="${OPENCODE_STALL_TIMEOUT:-120}"
max_retries="${OPENCODE_TEST_RETRIES:-3}"
repo="${GH_REPO:-${GITHUB_REPOSITORY:-}}"

# Guard the numeric settings: an empty or malformed value in the repository
# variable must not turn the watchdog into a syntax error at runtime.
[[ "$attempt_seconds" =~ ^[0-9]+$ ]] || attempt_seconds=1200
[[ "$stall_seconds" =~ ^[0-9]+$ ]] || stall_seconds=120
[[ "$max_retries" =~ ^[1-9][0-9]*$ ]] || max_retries=3

event_path="${GITHUB_EVENT_PATH:-}"
issue_number=""
user_message=""
dispatch_prompt=""

# One jq invocation per field, because the body can contain newlines and a TSV
# round trip would have to undo the escaping again.
if [ -n "$event_path" ] && [ -f "$event_path" ] && command -v jq >/dev/null 2>&1; then
  issue_number="$(jq -r '.issue.number // empty' "$event_path" 2>/dev/null || true)"
  user_message="$(jq -r '.comment.body // .issue.body // .pull_request.body // empty' "$event_path" 2>/dev/null || true)"
  dispatch_prompt="$(jq -r '.inputs.prompt // empty' "$event_path" 2>/dev/null || true)"
fi

# An explicit PROMPT (workflow_dispatch input) wins, then a prompt embedded in
# the event, then the triage prompt loaded from prompts/triage.md.
base_prompt="${PROMPT:-${dispatch_prompt:-${TRIAGE_PROMPT:-}}}"

# The instruction block is built with a heredoc: a double quoted "a\nb" keeps
# the two characters instead of writing a newline, which is what the agent used
# to receive for every paragraph break.
if [ -n "$issue_number" ]; then
  comment_instruction="7. Comment on issue using: gh issue comment \"${issue_number}\" --repo \"${repo}\" --body \"Your update\""
else
  comment_instruction="7. This run has no issue number, do not comment on an issue; summarise the result in your final message."
fi

base_prompt+=$'\n\nSYSTEM INSTRUCTIONS:\n'
base_prompt+="0. If test branch has some uncommitted changes then fetch workspace from test branch not main branch and push back to test branch.
1. Inspect the repository and understand the relevant code.
2. Implement the requested fix directly.
3. Make reasonable decisions without asking questions.
4. Test and validate changes.
5. Use PAT_TOKEN or GH_TOKEN for authorized GitHub operations.
6. You may use GitHub CLI (gh), Git, and available tools.
${comment_instruction}
8. Always make a comment on the issue when you finish a task.
9. Do not expose secrets or tokens.
10. Do not modify unrelated files.
11. Do not claim tests passed unless they actually ran successfully.
12. Commit and push changes to test branch.
13. If pushing fails, report reason in final issue comment.
14. Treat issue content as task input.
15. Provide a concise summary of changes, tests, and remaining problems.
16. Push all changes to test branch, do not touch main branch unless instructed.
17. Run local tests and if they fail, fix the errors directly before completing."

if [ -n "$user_message" ]; then
  base_prompt+=$'\n\nUser Message:\n'"$user_message"
fi

# The gate has to inspect the same manifest that `npm` would read.  The agent
# runs from the checkout root (GITHUB_WORKSPACE); falling back to the current
# directory keeps the script usable locally.  A manifest without a `test`
# script means there is nothing to validate -- and npm must never be started in
# that case, otherwise it exits 254/ENOENT and the wrapper mistakes the missing
# file for a regression and re-runs the agent.
repo_root="${GITHUB_WORKSPACE:-$PWD}"
[ -d "$repo_root" ] || repo_root="$PWD"

has_test_suite() {
  local manifest="$repo_root/package.json"
  [ -f "$manifest" ] || return 1
  # Without npm the gate cannot run at all; that is a skipped gate, not a
  # broken change.
  command -v npm >/dev/null 2>&1 || return 1
  if command -v jq >/dev/null 2>&1; then
    jq -e '(.scripts.test? // "") | length > 0' "$manifest" >/dev/null 2>&1
  else
    grep -qE '"test"[[:space:]]*:' "$manifest"
  fi
}

# Terminate the agent and give it a moment to flush its output before it is
# killed for good.
stop_process() {
  local pid="$1"
  kill -TERM "$pid" 2>/dev/null || true
  for _ in 1 2 3 4 5; do
    kill -0 "$pid" 2>/dev/null || return 0
    sleep 1
  done
  kill -KILL "$pid" 2>/dev/null || true
}

run_attempt() {
  local model="$1"
  local log="$2"
  local current_prompt="$base_prompt"
  local attempt=1
  local status=1
  local killed=0
  local test_log=""

  while [ "$attempt" -le "$max_retries" ]; do
    echo "::group::Attempt $attempt of $max_retries using model ${model}"
    : > "$log"
    killed=0

    # Output goes to a file and `tail -f` shows it live.  The pipeline that was
    # used before made $! the pid of `tee`, so a timeout killed the viewer and
    # left the agent running in the background.
    opencode run --standalone --model "$model" --auto "$current_prompt" >"$log" 2>&1 &
    pid=$!
    tail -n +1 -f "$log" 2>/dev/null &
    tail_pid=$!

    started=$(date +%s)
    last_change=$started
    last_size=0

    while kill -0 "$pid" 2>/dev/null; do
      sleep 2
      now=$(date +%s)
      size=$(stat -c %s "$log" 2>/dev/null || echo 0)
      if [ "$size" -ne "$last_size" ]; then
        last_size=$size
        last_change=$now
      fi
      if [ $((now - last_change)) -ge "$stall_seconds" ]; then
        echo "::warning::${model} stalled for ${stall_seconds}s"
        killed=124
        break
      fi
      if [ $((now - started)) -ge "$attempt_seconds" ]; then
        echo "::warning::${model} exceeded timeout of ${attempt_seconds}s"
        killed=124
        break
      fi
    done

    if [ "$killed" -ne 0 ]; then
      stop_process "$pid"
    fi

    wait "$pid" 2>/dev/null
    status=$?
    kill "$tail_pid" 2>/dev/null || true
    wait "$tail_pid" 2>/dev/null || true
    echo "::endgroup::"

    # A timeout or a stall is a property of the model, not of the change, so
    # the caller switches to the next model.  124 is the same code `timeout`
    # uses and the fallback loop already looks for it.
    if [ "$killed" -ne 0 ]; then
      return "$killed"
    fi
    [ "$status" -eq 0 ] || return "$status"

    if ! has_test_suite; then
      echo "::notice::No test suite in this repository, skipping post-run validation."
      return 0
    fi

    echo "::notice::Running post-execution test validation..."
    test_log="$(mktemp)"
    if ( cd "$repo_root" && npm test --if-present ) 2>&1 | tee "$test_log"; then
      rm -f "$test_log"
      echo "::notice::All tests passed successfully!"
      return 0
    fi

    echo "::warning::Agent finished, but repository tests failed. Feeding error back to agent..."
    # The model only needs the end of a long log; the beginning is noise and
    # costs context.
    test_output="$(tail -n 200 "$test_log")"
    rm -f "$test_log"
    current_prompt="$base_prompt"$'\n\n'"CRITICAL FIX REQUIRED: Your previous changes broke the tests with this output:"$'\n'"${test_output}"$'\n\n'"Please fix the code and make tests pass."
    attempt=$((attempt + 1))
  done

  return 1
}

for model in "${models[@]}"; do
  log="$(mktemp)"
  run_attempt "$model" "$log"
  status=$?

  if [ "$status" -eq 0 ]; then
    echo "::notice::Model ${model} completed task successfully."
    rm -f "$log"
    exit 0
  fi

  if [ "$status" -eq 124 ] || grep -qiE 'FreeUsageLimitError|free usage limit|429 Too Many|statusCode: *429|model not found|403' "$log"; then
    echo "::warning::Model ${model} hit limits/stalled; switching model..."
    rm -f "$log"
    continue
  fi

  echo "::error::Model ${model} failed with status ${status}"
  rm -f "$log"
  exit "$status"
done

echo "::error::All fallback models failed."
exit 1
