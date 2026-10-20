# AGENTS.md

Instructions for the opencode agent working in this repository.

## What this repository is

- `prompts/triage.md` is the instruction used when the agent answers a newly
  opened issue automatically.
- `.github/workflows/` contains the workflows that drive this agent and the
  `release.yml` workflow that publishes a build.
- `.github/scripts/` contains the tooling the agent uses:
  - `publish-release.sh` builds something and publishes it as a release asset.
  - `gh-account.sh` does the account level operations.

There is no binary in the source tree any more.  The build output of
opencode v2.0.22 lives in the release `v2.0.22` of this repository.

## The rule about build output

**Build output is never committed to the source tree.  Every build ends in a
GitHub Release and the binary is an asset of that release.**

Use the script, it builds in a scratch directory, it never touches the
checkout and it never commits anything:

```bash
.github/scripts/publish-release.sh \
    --repo mmunki/ld --clone https://github.com/mmunki/ld --branch main \
    --build "make" --artifact bin/ld \
    --tag v0.1.0 --name "ld 0.1.0" --notes "what the release contains"
```

The script prints the URL of the release.  That URL is the deliverable, it
belongs in the answer to the issue.

The same thing can be done from the Actions tab: **Actions → release → Run
workflow**.

## Access to the other repositories of the owner

The agent runs with a token that has access to **all** repositories of the
`mmunki` account, not only this one.  That means you can:

- read another repository: `gh repo view mmunki/<repo>`, `gh api repos/mmunki/<repo>`
- clone it: `git clone https://github.com/mmunki/<repo>`
- open a pull request against it: `gh pr create --repo mmunki/<repo> ...`
- create a release in it: `gh release create --repo mmunki/<repo> ...`
- create a repository: `.github/scripts/gh-account.sh repo-create NAME --private`

Prefer `gh` for anything that touches the GitHub API, it is already
authenticated through `GH_TOKEN`.

## The account itself

`.github/scripts/gh-account.sh` wraps the account level operations:

| Command | What it does |
| --- | --- |
| `whoami` | the login, the id, the plan and the scopes of the token in use |
| `scopes` | the scopes of the token in use |
| `profile` | shows the profile |
| `profile bio="..." company="..." hireable=false` | changes the profile (name, e-mail, bio, company, blog, location, twitter) |
| `username` | shows the login |
| `username newname` | explains how to rename the account |
| `emails` | the e-mail addresses of the account |
| `ssh-keys` | the public SSH keys of the account |
| `pat --name NAME [--repo OWNER/NAME] [--out FILE]` | creates a new personal access token |
| `repo-create NAME [--public\|--private] [--description TEXT]` | creates a repository |

Two things have to be known about these operations:

- **The login of the account cannot be changed with an API.**  GitHub only
  allows it in the browser, `username` prints the exact steps.  Everything
  else about the account (display name, e-mail, bio, company, blog, location,
  SSH keys, tokens, repository visibility) is done through the script.
- **Creating a token needs the `OPENCODE_ADMIN_TOKEN` secret.**  A classic
  token cannot create other tokens, only a fine grained token with
  `Administration: write` can.  Without that secret the `pat` command stops
  with a clear message and everything else still works.

A token that the script creates is written to a file with mode 600, it is
never printed into a comment or into a log.  Store it with:

```bash
gh secret set OPENCODE_ADMIN_TOKEN --repo mmunki/opencode < token.txt
```

## Rules

- Never commit a build output, a binary or an archive to the source tree.
  Publish it as a release asset instead.
- Do not commit secrets, tokens or `.env` files.
- Keep changes to the `main` branch minimal and focused; create a branch for
  anything non-trivial.
- The `gh` CLI is authenticated as the repository owner, so `gh` commands that
  write (issue comments, PRs, branches, releases) act as that user.
