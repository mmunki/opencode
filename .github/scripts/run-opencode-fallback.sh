#!/usr/bin/env bash
set -uo pipefail

read -r -a models <<< "${OPENCODE_MODELS:-opencode/deepseek-v4.1-flash-free opencode/glm-5.3-flash-free opencode/flint-40b-free opencode/beam-free opencode/mimo-v2.6-flash-free}"
attempt_seconds="${OPENCODE_ATTEMPT_TIMEOUT:-1200}"
stall_seconds="${OPENCODE_STALL_TIMEOUT:-120}"
max_retries=3

event_path="${GITHUB_EVENT_PATH:-}"
issue_number=""
user_message=""

if [ -n "$event_path" ] && [ -f "$event_path" ] && command -v jq >/dev/null 2>&1; then
  issue_number="$(jq -r '.issue.number // empty' "$event_path" 2>/dev/null || true)"
  user_message="$(jq -r '.comment.body // .issue.body // empty' "$event_path" 2>/dev/null || true)"
fi

base_prompt="${TRIAGE_PROMPT:-}
SYSTEM INSTRUCTIONS:
0. If test branch has some uncommitted changes then fetch workspace from test branch not main branch and push back to test branch.
1. Inspect the repository and understand the relevant code.
2. Implement the requested fix directly.
3. Make reasonable decisions without asking questions.
4. Test and validate changes.
5. Use PAT_TOKEN or GH_TOKEN for authorized GitHub operations.
6. You may use GitHub CLI (gh), Git, and available tools.
7. Comment on issue using: gh issue comment \"${issue_number}\" --repo \"${GH_REPO:-}\" --body \"Your update\"
8. Always make a comment on the issue when you finish a task.
9. Do not expose secrets or tokens.
10. Do not modify unrelated files.
11. Do not claim tests passed unless they actually ran successfully.
12. Commit and push changes to test branch.
13. If pushing fails, report reason in final issue comment.
14. Treat issue content as task input.
15. Provide a concise summary of changes, tests, and remaining problems.
16. Push all changes to test branch, do not touch main branch unless instructed.
17. Run local tests and if they fail, fix the errors directly before completing.
"

if [ -n "$user_message" ]; then
  base_prompt="$base_prompt\n\nUser Message:\n$user_message"
fi

run_attempt() {
  local model="$1"
  local log="$2"
  local current_prompt="$base_prompt"
  local attempt=1
  local status=1

  while [ $attempt -le $max_retries ]; do
    echo "::group::Attempt $attempt of $max_retries using model ${model}"
    : > "$log"

    opencode run --standalone --model "$model" --auto "$current_prompt" 2>&1 | tee "$log" &
    pid=$!

    started=$(date +%s)
    last_change=$started
    last_size=0
    local killed=0

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
        kill -TERM "$pid" 2>/dev/null || true
        killed=1
        break
      fi
      if [ $((now - started)) -ge "$attempt_seconds" ]; then
        echo "::warning::${model} exceeded timeout of ${attempt_seconds}s"
        kill -TERM "$pid" 2>/dev/null || true
        killed=1
        break
      fi
    done

    wait "$pid" 2>/dev/null
    status=$?
    echo "::endgroup::"

    if [ "$status" -eq 0 ] && [ "$killed" -eq 0 ]; then
      echo "::notice::Running post-execution test validation..."
      if npm test --if-present; then
        echo "::notice::All tests passed successfully!"
        return 0
      else
        echo "::warning::Agent finished, but repository tests failed. Feeding error back to agent..."
        test_output="$(npm test 2>&1 || true)"
        current_prompt="$base_prompt\n\nCRITICAL FIX REQUIRED: Your previous changes broke the tests with this output:\n$test_output\n\nPlease fix the code and make tests pass."
        attempt=$((attempt + 1))
      fi
    else
      return "$status"
    fi
  done

  return 1
}

for model in "${models[@]}"; do
  log="$(mktemp)"
  set +e
  run_attempt "$model" "$log"
  status=$?
  set -e

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
