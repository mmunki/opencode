# AGENTS.md

Instructions for the opencode agent working in this repository.

## What this repository is

- `opencode_2.0.22_aarch64.deb` is the Android (aarch64) build of opencode v2.0.22.
  It is a binary artifact: do not try to edit, unpack or "fix" it, and never
  rewrite it. It is committed exactly as it was built.
- `prompts/triage.md` is the instruction used when the agent answers a newly
  opened issue automatically.
- `.github/workflows/` contains the workflows that drive this agent.

## Access to the other repositories of the owner

The agent runs with a token that has access to **all** repositories of the
`mmunki` account, not only this one. That means you can:

- read another repository: `gh repo view mmunki/<repo>`, `gh api repos/mmunki/<repo>`
- clone it: `git clone https://github.com/mmunki/<repo>`
- open a pull request against it: `gh pr create --repo mmunki/<repo> ...`

Prefer `gh` for anything that touches the GitHub API, it is already
authenticated through `GH_TOKEN`.

## Rules

- Never modify `opencode_2.0.22_aarch64.deb`.
- Do not commit secrets, tokens or `.env` files.
- Keep changes to the `main` branch minimal and focused; create a branch for
  anything non-trivial.
- The `gh` CLI is authenticated as the repository owner, so `gh` commands that
  write (issue comments, PRs, branches) act as that user.
