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

### Second run, 2026-10-28

**Reported symptom:** the nightly run failed again with
`npm error path .../package.json` / `ENOENT`, and the retry prompt
("CRITICAL FIX REQUIRED") was passed back to the agent.

**Root cause:** the fix above exists only on the `test` branch. The
`opencode-triage` workflow checks out
`ref: ${{ github.event.repository.default_branch }}`, which is `main`, and
`main` still contains the old runner that calls `npm test` unconditionally.
Every issue-triggered run therefore executes the unfixed script from `main`;
the guard on `test` is never reached. This is a deployment gap, not a new
regression in the script.

**Change (on `test`):** resolve the manifest from `GITHUB_WORKSPACE` (the
checkout root, falling back to the current directory) and run `npm` from that
directory, so the guard and the invocation cannot disagree when a workflow
step starts in a subdirectory. Skip the gate when `npm` itself is missing.
Added two regression cases: a manifest without a `test` script must never
start `npm`, and a run from a subdirectory must validate the workspace root.
The suite is now 26 assertions.

**Verification:** `shellcheck -x` is clean; all 26 assertions pass.

**Open, needs the owner:** PR #93 (`test` → `main`) carries the runner fix and
must be merged before issue-triggered runs stop using the old script. Until it
is merged, no change on `test` can affect the workflow, because the workflow
always checks out the default branch. An alternative is to change the
checkout `ref` in `opencode-triage.yml` to the branch the agent uses.

### Third run, 2026-10-28

**Reported symptom (issue #92, still open):** every consumer of `npm test` at
the checkout root - the agent's post-run gate in this repository, and any
external harness - failed with

```
npm error path /home/runner/work/opencode/opencode/package.json
npm error enoent Could not read package.json
```

**Root cause:** the previous run fixed the guard inside the runner, but the
repository still had no manifest at all. Any `npm test` invocation that did
not go through the guard (the old runner on `main`, a CI harness, a developer)
hit ENOENT. Fixing the caller and leaving the missing file in place made the
failure dependent on which copy of the script ran.

**Change:**

- added `package.json` at the repository root: `npm test` runs
  `.github/scripts/tests/run-opencode-fallback.test.sh`, `npm run lint` runs
  `shellcheck`, `npm run check` runs both. The manifest is private, has no
  dependencies and adds no install step.
- `scripts-test.yml` now invokes `npm run lint` / `npm test`, so CI validates
  the same entry points the gate uses, and the workflow triggers on
  `package.json` changes.
- the runner test suite gained case 7: it asserts the repository manifest
  exists, parses as JSON and exposes a non-empty `test` script, and that the
  gate accepts it (31 assertions).

**Verification:** `npm test` (31 assertions) and `npm run lint` pass; the
runner still skips the gate for a manifest without a test script and for a
missing manifest.

**Still open:** the `issues` event always loads the workflow from the default
branch, so `main` must receive the runner and manifest through a pull request
(`test` → `main`) before issue-triggered runs use them. Nothing on `test` can
change what an issue-triggered run executes.

