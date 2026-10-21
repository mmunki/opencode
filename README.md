# opencode

Personal opencode agent. Ask it anything through GitHub Issues.

## What is in here

| Path | Description |
| --- | --- |
| `.github/workflows/opencode.yml` | Answers questions asked in issue / PR comments. |
| `.github/workflows/opencode-triage.yml` | Automatically answers newly opened issues. |
| `.github/workflows/release.yml` | Builds something and publishes it as a release. |
| `.github/scripts/publish-release.sh` | Builds an artefact and publishes it as a release asset. |
| `.github/scripts/gh-account.sh` | The account level operations (profile, tokens, repositories). |
| `prompts/triage.md` | The instruction used for automatic issue triage. |
| `AGENTS.md` | Context the agent loads for this repository. |

There is **no binary in the source tree**. Every build ends in a GitHub
Release and the binary is an asset of that release.

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
/oc what does the release of this repository contain?
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

## Releases

Build output is never committed to the source tree. It is always published as
a release asset, and the release URL is what the agent reports back.

The SHA-256 of every published asset is written into the notes of the release,
so a download can be verified against the release it came from.

The releases of this repository:

- https://github.com/mmunki/opencode/releases

Ask for a build and the agent publishes it:

```
/oc build the final ld binary from mmunki/ld and publish it as a release
```

The same can be done from the Actions tab: **Actions → release → Run
workflow**. The build always runs in a scratch copy of the source, so the
checkout is never modified.

### Downloading the .deb

The package is published as a release asset, byte-for-byte as it was built,
so the blob on GitHub has the exact same SHA-1 as the file on the device:

```bash
gh release download v2.0.22 --repo mmunki/opencode \
  --pattern 'opencode_2.0.22_aarch64.deb'
```

Verify it after downloading:

```bash
sha256sum opencode_2.0.22_aarch64.deb
# 629b6819e40418b96a8dedfc15f8b6cac8f5e4b54682d9f4194c59b68aa95829
```

### The OpenJDK 27 package for Termux

The agent also builds Termux packages. The OpenJDK 27 package for Termux
(Android, `aarch64`) has its own repository with the build recipe and the
patches:

- https://github.com/mmunki/termux-openjdk-27

It is a real OpenJDK 27.0.1 build: the sources of the OpenJDK 27.0.1 GA
release are compiled with the Android NDK for the `aarch64-linux-android`
target, with the Android patches of the Termux package applied. The `.deb`
files are published as a release:

- https://github.com/mmunki/termux-openjdk-27/releases/tag/v27.0.1

## Reach of the agent

The agent authenticates with a token that has access to **every repository of
the `mmunki` account**, so it can read and modify any of them, not just this
one. You can ask it to work on another repository:

```
/oc fix the failing build in mmunki/su and open a PR there
```

It can also work on the account itself - the profile, the repositories, the
releases and the tokens. `.github/scripts/gh-account.sh` is the interface:

| Command | What it does |
| --- | --- |
| `whoami`, `scopes` | the login, the id and the scopes of the token in use |
| `profile`, `profile bio="..." company="..."` | shows and changes the profile |
| `username` | shows the login and explains how to rename the account |
| `emails`, `ssh-keys` | the e-mail addresses and the SSH keys of the account |
| `pat --name NAME [--repo OWNER/NAME]` | creates a new personal access token |
| `repo-create NAME [--public\|--private]` | creates a repository |

Two limits are worth knowing:

- **The login of the account cannot be changed with an API.** GitHub only
  allows it in the browser, `username` prints the exact steps.
- **Creating a token needs the `OPENCODE_ADMIN_TOKEN` secret**, a fine grained
  token with `Administration: write`. A classic token cannot create other
  tokens. Without that secret the `pat` command stops with a clear message
  and everything else still works.

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
| `OPENCODE_GITHUB_TOKEN` | yes | Token used for the GitHub API and for git clone/fetch/push. Needs `repo` and `workflow` scopes to reach all repositories. With the `user` scope it can also change the profile, with `admin:public_key` it can manage the SSH keys. |
| `OPENCODE_ADMIN_TOKEN` | no | Fine grained token with `Administration: write`. It is the only way to create further tokens. |
| `OPENCODE_API_KEY` | no | Only needed if you switch away from the free model. |

### Rotating the token

Create a classic personal access token with the `repo` and `workflow` scopes,
then update the secret:

```bash
gh secret set OPENCODE_GITHUB_TOKEN --repo mmunki/opencode
```

### Adding the administration token

Create a **fine grained** personal access token for the `mmunki` account with
the `Administration: write` permission, then store it:

```bash
gh secret set OPENCODE_ADMIN_TOKEN --repo mmunki/opencode
```

With that secret the agent can create new personal access tokens through
`.github/scripts/gh-account.sh pat`.
