# opencode

Personal opencode agent. Ask it anything through GitHub Issues.

## What is in here

| Path | Description |
| --- | --- |
| `opencode_2.0.22_aarch64.deb` | The Android `aarch64` build of opencode v2.0.22, committed byte-for-byte as built. |
| `.github/workflows/opencode.yml` | Answers questions asked in issue / PR comments. |
| `.github/workflows/opencode-triage.yml` | Automatically answers newly opened issues. |
| `prompts/triage.md` | The instruction used for automatic issue triage. |
| `AGENTS.md` | Context the agent loads for this repository. |

## How to use it

### Where does the answer appear?

**As a comment on the issue, right below your question.**

When you ask, the agent immediately adds a `👀` reaction to your comment and
posts a short `[Working...]` placeholder comment that links to the run. When
the agent finishes it **edits that same comment** into the final answer, so the
issue ends up with exactly one comment from the agent and no leftover
placeholder. The run log stays available under the
`[github run]` link at the bottom of the answer.

If the agent decides the issue needs a code change, it opens a pull request and
replies with a `Created PR #<n>` comment instead of a prose answer.

### Ask a question

Open an issue (or comment on an existing one) and write:

```
/oc what does the deb in this repo contain?
```

or

```
/opencode explain how the triage workflow works
```

The agent reads the whole thread, then replies with its answer. If the answer
requires code changes it opens a pull request instead.

### Automatic answers

Every newly opened issue is answered automatically by the triage workflow.
Only the `opened` event is a trigger - if `edited` were included, GitHub would
emit a second event right after creation and every new issue would be
answered twice.

### Manual run

Go to **Actions → opencode-triage → Run workflow** and type your own prompt.

## Downloading the .deb

The package is committed to this repository byte-for-byte as it was built, so
the blob on GitHub has the exact same SHA-1 as the file on the device:

```
629b6819e40418b96a8dedfc15f8b6cac8f5e4b54682d9f4194c59b68aa95829  opencode_2.0.22_aarch64.deb
```

Because the repository is private, download it with an authenticated client:

```bash
gh api repos/mmunki/opencode/contents/opencode_2.0.22_aarch64.deb \
  --jq .content | base64 -d > opencode_2.0.22_aarch64.deb
```

Verify it after downloading:

```bash
sha256sum opencode_2.0.22_aarch64.deb
# 629b6819e40418b96a8dedfc15f8b6cac8f5e4b54682d9f4194c59b68aa95829
```

## Reach of the agent

The agent authenticates with a token that has access to **every repository of
the `mmunki` account**, so it can read and modify any of them, not just this
one. You can ask it to work on another repository:

```
/oc fix the failing build in mmunki/su and open a PR there
```

## Setup details

- **Model** – `opencode/minimax-m3.1-flash-free`. The free opencode model is
  used by default, so no provider API key is required. To switch to a keyed
  model, add an `OPENCODE_API_KEY` secret to the repository and update the
  `model:` input in the workflows.
- **Auth** – the workflows use `use_github_token: true`, which means the
  OpenCode GitHub App does **not** need to be installed. The token is read from
  the `OPENCODE_GITHUB_TOKEN` secret.
- **Action** – pinned to `anomalyco/opencode/github@github-v1.2.26`. The action
  installs opencode v1, which is the release that still ships the
  `opencode github run` command.

## Secrets used

| Secret | Required | Purpose |
| --- | --- | --- |
| `OPENCODE_GITHUB_TOKEN` | yes | Token used for the GitHub API and for git clone/fetch/push. Needs `repo` and `workflow` scopes to reach all repositories. |
| `OPENCODE_API_KEY` | no | Only needed if you switch away from the free model. |

### Rotating the token

Create a classic personal access token with the `repo` and `workflow` scopes,
then update the secret:

```bash
gh secret set OPENCODE_GITHUB_TOKEN --repo mmunki/opencode
```
