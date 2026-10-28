#!/usr/bin/env bash
# Tests for .github/scripts/run-opencode-fallback.sh.
#
# The opencode binary and npm are replaced by stubs, so the suite runs without
# a provider key and without network access.
#
#   .github/scripts/tests/run-opencode-fallback.test.sh
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
SCRIPT="$ROOT/.github/scripts/run-opencode-fallback.sh"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

PASS=0
FAIL=0

ok()   { PASS=$((PASS + 1)); printf 'ok   %s\n' "$1"; }
bad()  { FAIL=$((FAIL + 1)); printf 'FAIL %s\n' "$1"; }
check() { # check <description> <command...>
  local description="$1"; shift
  if "$@"; then ok "$description"; else bad "$description"; fi
}

no_literal_escapes() { ! grep -qF '\n' "$FAKE_OPENCODE_PROMPT"; }

make_stubs() {
  local dir="$1"
  mkdir -p "$dir/bin"

  cat > "$dir/bin/opencode" <<'STUB'
#!/usr/bin/env bash
printf 'call\n' >> "$FAKE_OPENCODE_CALLS"
printf '%s\n' "$@" > "$FAKE_OPENCODE_ARGS"
last="${!#}"
printf '%s' "$last" > "$FAKE_OPENCODE_PROMPT"
printf 'fake opencode output\n'
exit "${FAKE_OPENCODE_EXIT:-0}"
STUB

  cat > "$dir/bin/npm" <<'STUB'
#!/usr/bin/env bash
printf 'npm %s\n' "$*" >> "$FAKE_NPM_LOG"
printf 'fake test failure\n'
exit "${FAKE_NPM_EXIT:-0}"
STUB

  chmod +x "$dir/bin/opencode" "$dir/bin/npm"
}

new_case() { # new_case <name> -> sets CASE, adds stubs, changes into it
  CASE="$WORK/$1"
  mkdir -p "$CASE/work"
  make_stubs "$CASE"
  export FAKE_OPENCODE_ARGS="$CASE/opencode.args"
  export FAKE_OPENCODE_CALLS="$CASE/opencode.calls"
  export FAKE_OPENCODE_PROMPT="$CASE/opencode.prompt"
  export FAKE_NPM_LOG="$CASE/npm.log"
  : > "$FAKE_OPENCODE_ARGS"
  : > "$FAKE_OPENCODE_CALLS"
  : > "$FAKE_OPENCODE_PROMPT"
  : > "$FAKE_NPM_LOG"
  unset FAKE_OPENCODE_EXIT FAKE_NPM_EXIT
  # The harness itself may export an event payload; every case sets what it needs.
  unset GITHUB_EVENT_PATH GITHUB_REPOSITORY GH_REPO PROMPT TRIAGE_PROMPT OPENCODE_MODELS GITHUB_WORKSPACE
  export PATH="$CASE/bin:$PATH"
  cd "$CASE/work" || exit 1
}

# --- 1. a repository without a test suite is not treated as a failure -------
new_case no-test-suite
cat > "$CASE/work/issue.json" <<'JSON'
{"issue":{"number":92,"body":"Please improve the fallback runner."}}
JSON
set +e
out="$(GITHUB_EVENT_PATH="$CASE/work/issue.json" GH_REPO="mmunki/opencode" \
      TRIAGE_PROMPT="Triage instructions." OPENCODE_MODELS="test/model" \
      timeout 60 "$SCRIPT" 2>&1)"
status=$?
set -e
check "no test suite: exit status is 0" [ "$status" -eq 0 ]
check "no test suite: opencode ran once" [ "$(wc -l < "$FAKE_OPENCODE_CALLS")" -eq 1 ]
check "no test suite: npm was never called" [ ! -s "$FAKE_NPM_LOG" ]
check "no test suite: gate is reported as skipped" grep -q 'No test suite' <<< "$out"
check "prompt keeps real newlines" grep -q '^SYSTEM INSTRUCTIONS:$' "$FAKE_OPENCODE_PROMPT"
check "prompt has no literal backslash-n" no_literal_escapes
check "prompt contains the issue body" grep -q '^Please improve the fallback runner\.$' "$FAKE_OPENCODE_PROMPT"
check "prompt names the issue in the gh command" grep -q 'issue comment "92"' "$FAKE_OPENCODE_PROMPT"

# --- 2. an explicit PROMPT wins over TRIAGE_PROMPT --------------------------
new_case explicit-prompt
set +e
PROMPT="Explicit workflow prompt." TRIAGE_PROMPT="Triage instructions." \
  GH_REPO="mmunki/opencode" OPENCODE_MODELS="test/model" \
  timeout 60 "$SCRIPT" >/dev/null 2>&1
status=$?
set -e
check "explicit prompt: exit status is 0" [ "$status" -eq 0 ]
check "explicit prompt: prompt is used" grep -q '^Explicit workflow prompt\.$' "$FAKE_OPENCODE_PROMPT"
check "explicit prompt: no issue comment instruction" grep -q 'no issue number' "$FAKE_OPENCODE_PROMPT"

# --- 3. a failing test suite is fed back to the agent -----------------------
new_case failing-tests
printf '{"name":"fixture","scripts":{"test":"exit 1"}}\n' > "$CASE/work/package.json"
export FAKE_NPM_EXIT=1
set +e
out="$(TRIAGE_PROMPT="Triage." GH_REPO="mmunki/opencode" OPENCODE_MODELS="test/model" \
      OPENCODE_TEST_RETRIES=2 timeout 60 "$SCRIPT" 2>&1)"
status=$?
set -e
check "failing tests: job fails" [ "$status" -ne 0 ]
check "failing tests: agent ran for every retry" [ "$(wc -l < "$FAKE_OPENCODE_CALLS")" -eq 2 ]
check "failing tests: npm ran twice" [ "$(wc -l < "$FAKE_NPM_LOG")" -eq 2 ]
check "failing tests: failure is fed back" grep -q 'CRITICAL FIX REQUIRED' "$FAKE_OPENCODE_PROMPT"

# --- 4. a stalled model is aborted and the next model is tried --------------
new_case stalled-model
cat > "$CASE/bin/opencode" <<'STUB'
#!/usr/bin/env bash
printf 'call\n' >> "$FAKE_OPENCODE_CALLS"
printf '%s\n' "$@" > "$FAKE_OPENCODE_ARGS"
printf 'starting\n'
sleep 60
STUB
chmod +x "$CASE/bin/opencode"
set +e
start=$(date +%s)
out="$(TRIAGE_PROMPT="Triage." GH_REPO="mmunki/opencode" OPENCODE_MODELS="slow/model fast/model" \
      OPENCODE_STALL_TIMEOUT=1 OPENCODE_ATTEMPT_TIMEOUT=5 timeout 60 "$SCRIPT" 2>&1)"
status=$?
elapsed=$(( $(date +%s) - start ))
set -e
check "stall: job fails after every model" [ "$status" -ne 0 ]
check "stall: both models were tried" [ "$(wc -l < "$FAKE_OPENCODE_CALLS")" -eq 2 ]
check "stall: watchdog reports the stall" grep -q 'stalled for 1s' <<< "$out"
check "stall: finishes promptly (${elapsed}s)" [ "$elapsed" -lt 40 ]

# --- 5. a manifest without a test script never starts npm -------------------
# `npm test --if-present` exits 0 for this file, but starting npm is still
# wrong: it is the exact path that produced the ENOENT failures.
new_case manifest-without-test
printf '{"name":"fixture","scripts":{"build":"true"}}\n' > "$CASE/work/package.json"
set +e
out="$(TRIAGE_PROMPT="Triage." GH_REPO="mmunki/opencode" OPENCODE_MODELS="test/model" \
      timeout 60 "$SCRIPT" 2>&1)"
status=$?
set -e
check "no test script: exit status is 0" [ "$status" -eq 0 ]
check "no test script: npm was never called" [ ! -s "$FAKE_NPM_LOG" ]
check "no test script: gate is reported as skipped" grep -q 'No test suite' <<< "$out"

# --- 6. the gate uses the workspace root, not the caller's directory --------
# The workflow may run a step from a subdirectory. `npm` reads the manifest in
# its own cwd, so the guard and the invocation must agree on GITHUB_WORKSPACE.
new_case workspace-root-gate
printf '{"name":"fixture","scripts":{"test":"exit 0"}}\n' > "$CASE/work/package.json"
mkdir -p "$CASE/work/sub"
cd "$CASE/work/sub" || exit 1
set +e
out="$(GITHUB_WORKSPACE="$CASE/work" TRIAGE_PROMPT="Triage." GH_REPO="mmunki/opencode" \
      OPENCODE_MODELS="test/model" timeout 60 "$SCRIPT" 2>&1)"
status=$?
set -e
check "workspace root: exit status is 0" [ "$status" -eq 0 ]
check "workspace root: npm ran once" [ "$(wc -l < "$FAKE_NPM_LOG")" -eq 1 ]
check "workspace root: npm ran the test script" grep -q '^npm test --if-present$' "$FAKE_NPM_LOG"
check "workspace root: tests are reported as passed" grep -q 'All tests passed' <<< "$out"

# --- 7. this repository's own manifest is a valid test entry point ----------
# Regression for "npm error ENOENT .../package.json": the checkout root must
# carry a parseable manifest with a `test` script, otherwise the gate turns a
# successful agent run into a bogus "your changes broke the tests" retry loop.
new_case repository-manifest
check "repository: package.json exists" test -f "$ROOT/package.json"
check "repository: package.json is valid JSON" node -e '
  const m = JSON.parse(require("fs").readFileSync(process.argv[1], "utf8"));
  process.exit(typeof m.scripts?.test === "string" && m.scripts.test.length > 0 ? 0 : 1);
' "$ROOT/package.json"
set +e
out="$(GITHUB_WORKSPACE="$ROOT" TRIAGE_PROMPT="Triage." GH_REPO="mmunki/opencode" \
      OPENCODE_MODELS="test/model" timeout 60 "$SCRIPT" 2>&1)"
status=$?
set -e
check "repository: gate accepts the manifest" [ "$status" -eq 0 ]
check "repository: npm ran once" [ "$(wc -l < "$FAKE_NPM_LOG")" -eq 1 ]
check "repository: tests are reported as passed" grep -q 'All tests passed' <<< "$out"

echo
printf '%d passed, %d failed\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ]
