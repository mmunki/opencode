# Nightly maintenance log

State carried from one autonomous run to the next. Each entry records what was
inspected, the single change that was made, how it was verified, and what is
still open, so the next run does not repeat the same investigation.

## 2026-10-28

**Area:** `.github/scripts/run-opencode-fallback.sh` (the runner both agent
workflows use).

**Finding:** every successful agent run in this repository ended as a failure.
After the model finished, the wrapper ran `npm test --if-present`; this control
repository has no `package.json`, and npm exits 254 in that case. The wrapper
interpreted that as "the agent broke the tests", re-ran the agent up to three
times with a bogus failure message, then returned 1 and failed the job. The
fallback chain was never reached because the log did not contain a provider
limit error.

Three smaller defects were found in the same path:

- `PROMPT` (the `workflow_dispatch` input and the value exported by
  `opencode-triage.yml`) was read by the workflow but never used by the script.
- Prompts were built with `"\n"` inside double quotes, so the agent received
  the two characters `\` and `n` instead of paragraph breaks.
- A stall or timeout killed `tee`, not the agent, and returned status 143
  instead of 124, so the next model was not tried and the agent kept running.

**Change:** gate post-run validation on a real `test` script in
`package.json`, use `PROMPT` first and build the prompt with `$'\n'`, run the
agent with its output redirected to the log and stream it with `tail -f`, kill
the actual process (TERM then KILL) and return 124 on stall/timeout. A failing
suite is fed back with the last 200 log lines. Added a stubbed-binary test
suite and a `scripts-test` workflow.

**Verification:** `shellcheck -x` is clean; the 19 assertions in
`.github/scripts/tests/run-opencode-fallback.test.sh` pass, including the
no-`package.json` success case, the failing-suite retry case and the stall
watchdog.

**Open, not changed this run:**

- The "Create or Update Pull Request Automatically" step in
  `opencode-triage.yml` runs against the checked out default branch and does
  not push the agent's branch. It is effectively dead today and its title uses
  an empty issue number on `workflow_dispatch`. It should be replaced by an
  explicit branch/push/PR step or removed by the owner.
- `publish-release.sh --help` prints only a slice of the header and the header
  contains a stray `#       --source-archive` line. Cosmetic.
