A new issue was opened on this repository. The issue title and body are appended below.

Please do the following:

1. Investigate. Read the files in this repository that are relevant to the issue.
   You also have a GitHub token with access to every repository of the owner, so you
   may read other repositories under `mmunki/` if the issue relates to them
   (use `gh` or `git clone https://github.com/mmunki/<repo>`).
2. Reply with a clear and concise explanation:
   - what is actually happening,
   - why it happens,
   - the concrete fix (exact file and change) if you can determine one.
3. If you produced code changes, commit them on a branch and open a pull request
   that references this issue.
4. If the issue asks for a project, an app, a module or a build, the project
   gets its own repository. `mmunki/opencode` is only the control repository:
   never commit a project's source to it and never publish a project release
   in it.

   - Pick a short, lowercase, hyphenated repository name (for example
     `sipolicy`). Reuse the existing repository when the issue is a follow-up
     for a project that already has one.
   - Create the repository when it does not exist yet:

         .github/scripts/gh-account.sh repo-create <name> --public \
             --description "<one line about the project>"

   - Put the complete source in that repository, commit it on `main`, and
     push it. A pull request in `mmunki/opencode` is not where a project
     lives.
   - Build and test in that repository. For a native or Magisk project build
     and test every supported ABI before publishing; the release notes must
     name the test matrix and its result.
   - Publish the release in the project repository with the binary and the
     source archive:

         .github/scripts/publish-release.sh \
             --repo mmunki/<name> --clone https://github.com/mmunki/<name> \
             --branch main --build "<build command>" --artifact <path> \
             --source-archive \
             --tag <tag> --name "<title>" --notes "<notes>"

   The script prints the URL of the release. Put that URL in your answer, it is
   the deliverable. Add the checksums of the published assets to the notes.
5. If the issue asks for something on the GitHub account itself (the profile,
   the name, a new token, the visibility of a repository, a release), use
   `.github/scripts/gh-account.sh` and report what it printed. The login of the
   account cannot be changed with an API, only in the browser - say so and give
   the steps.
6. Never delete source code. Do not remove a file from the source tree, do not
   rewrite history, do not force push and do not delete a branch, a tag or a
   published release. If a file looks obsolete, say so in your answer and let
   the owner remove it. A pull request that deletes a file fails the
   `protect-source` workflow.

Do not ask clarifying questions when the issue is understandable. If it is
ambiguous, state the assumptions you made and answer under those assumptions.
Keep the reply focused and skip pleasantries.
