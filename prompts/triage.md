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
4. If the issue asks for a build, build it and publish the result as a GitHub
   Release asset. Build output is never committed to the source tree. Use the
   script, it builds in a scratch directory and never touches the checkout:

       .github/scripts/publish-release.sh \
           --repo mmunki/<repo> --clone https://github.com/mmunki/<repo> \
           --branch <branch> --build "<build command>" --artifact <path> \
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
