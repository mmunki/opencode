#!/usr/bin/env bash
# ============================================================================
# publish-release.sh -- build something and publish it as a release asset
# ----------------------------------------------------------------------------
# The rule of this repository: build output is never committed to the source
# tree, it is always published as a GitHub Release asset.
#
#   publish-release.sh --clone https://github.com/mmunki/ld --branch main \
#       --build "make" --artifact bin/ld --tag v0.1.0 --name "ld 0.1.0"
#
#   publish-release.sh --build "make" --artifact dist/app.tar.gz \
#       --tag v1.2.3 --notes-file NOTES.md
#
# The SHA-256 of every published asset is appended to the notes of the
# release, it is what makes a download verifiable.
#
# The build always runs in a scratch directory, the checkout that calls this
# script is never modified and nothing is ever committed.
# ============================================================================
set -euo pipefail

log() { printf '==> %s\n' "$*" >&2; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

usage() {
  sed -n '3,20p' "$0" | sed 's/^# \{0,1\}//'
  exit "${1:-0}"
}

REPO="${GITHUB_REPOSITORY:-}"
CLONE=""
BRANCH=""
BUILD=""
TAG=""
NAME=""
NOTES=""
NOTES_FILE=""
DRAFT=0
PRERELEASE=0
DRYRUN=0
ARTIFACTS=()

while [ $# -gt 0 ]; do
  case "$1" in
    --repo)         REPO="${2:?--repo needs a value}"; shift 2 ;;
    --clone)        CLONE="${2:?--clone needs a value}"; shift 2 ;;
    --branch)       BRANCH="${2:?--branch needs a value}"; shift 2 ;;
    --build)        BUILD="${2:?--build needs a value}"; shift 2 ;;
    --tag)          TAG="${2:?--tag needs a value}"; shift 2 ;;
    --name)         NAME="${2:?--name needs a value}"; shift 2 ;;
    --notes)        NOTES="${2:?--notes needs a value}"; shift 2 ;;
    --notes-file)   NOTES_FILE="${2:?--notes-file needs a value}"; shift 2 ;;
    --artifact)     ARTIFACTS+=("${2:?--artifact needs a value}"); shift 2 ;;
    --draft)        DRAFT=1; shift ;;
    --prerelease)   PRERELEASE=1; shift ;;
    --dry-run)      DRYRUN=1; shift ;;
    -h|--help)      usage 0 ;;
    -*)             die "unknown option: $1 (try --help)" ;;
    *)              ARTIFACTS+=("$1"); shift ;;
  esac
done

[ -n "$TAG" ] || die "--tag is required"
[ -n "$REPO" ] || die "--repo is required (for example --repo mmunki/opencode)"
command -v gh >/dev/null 2>&1 || die "the gh CLI is required"

# The owner token first: it can publish into every repository of the owner,
# the workflow token is only good for the repository that runs the workflow.
# When no token is in the environment the gh CLI uses its own authentication.
TOKEN="${GH_TOKEN:-${OPENCODE_ADMIN_TOKEN:-${GITHUB_TOKEN:-}}}"
if [ -n "$TOKEN" ]; then
  export GH_TOKEN="$TOKEN"
  export GITHUB_TOKEN="$TOKEN"
else
  log "no token in the environment, the gh CLI uses its own authentication"
fi

if [ -n "$NOTES_FILE" ]; then
  [ -f "$NOTES_FILE" ] || die "notes file not found: $NOTES_FILE"
  NOTES="$(cat "$NOTES_FILE")"
fi
[ -n "$NAME" ] || NAME="$TAG"

WORK="$(mktemp -d)"
cleanup() { rm -rf "$WORK"; }
trap cleanup EXIT

SRC="$WORK/src"
STAGE="$WORK/stage"
mkdir -p "$SRC" "$STAGE"

if [ -n "$CLONE" ]; then
  log "cloning $CLONE${BRANCH:+ ($BRANCH)}"
  if [ -n "$BRANCH" ]; then
    git clone --quiet --depth 50 --branch "$BRANCH" "$CLONE" "$SRC" \
      || git clone --quiet --depth 50 "$CLONE" "$SRC"
  else
    git clone --quiet --depth 50 "$CLONE" "$SRC"
  fi
elif [ -n "$BUILD" ]; then
  log "copying the working tree into $SRC (the checkout is not touched)"
  tar -C "$(pwd)" --exclude=.git -cf - . 2>/dev/null | tar -C "$SRC" -xf -
fi

if [ -n "$BUILD" ]; then
  log "building: $BUILD"
  if [ "$DRYRUN" = 1 ]; then
    log "dry run, the build is not executed"
  else
    ( cd "$SRC" && bash -lc "$BUILD" )
  fi
fi

for a in "${ARTIFACTS[@]:-}"; do
  [ -n "$a" ] || continue
  if   [ -f "$a" ];     then src="$a"
  elif [ -f "$SRC/$a" ]; then src="$SRC/$a"
  else die "asset not found: $a"
  fi
  cp -f "$src" "$STAGE/$(basename "$src")"
  log "asset: $(basename "$src") ($(wc -c <"$STAGE/$(basename "$src")") bytes)"
done

shopt -s nullglob
files=("$STAGE"/*)
shopt -u nullglob

# The SHA-256 of every asset belongs into the notes of the release, it is
# what makes a download verifiable.  The checksums are taken from the staged
# files, they are the exact bytes that are uploaded.
CHECKSUMS=""
if [ ${#files[@]} -gt 0 ]; then
  command -v sha256sum >/dev/null 2>&1 || die "sha256sum is required"
  for f in "${files[@]}"; do
    sum="$(sha256sum "$f" | cut -d' ' -f1)"
    if [ -n "$CHECKSUMS" ]; then
      CHECKSUMS+=$'\n'
    fi
    CHECKSUMS+="- \`$(basename "$f")\`: \`$sum\`"
    log "sha256 $(basename "$f"): $sum"
  done
fi

# The checksum section of the release notes.
checksum_section() {
  [ -n "$CHECKSUMS" ] || return 0
  printf '### Checksums (SHA-256)\n\n%s\n' "$CHECKSUMS"
}

# The notes of a new release: the text of the caller and the checksums.
compose_notes() {
  if [ -n "$NOTES" ]; then
    printf '%s\n\n' "$NOTES"
  fi
  checksum_section
}

# Drops a checksum section that a previous run of this script has written.
strip_checksums() {
  awk '
    /^### Checksums \(SHA-256\)$/ { skip = 1; next }
    skip && /^### / { skip = 0 }
    !skip { print }
  '
}

if [ "$DRYRUN" = 1 ]; then
  log "dry run, nothing was published"
  exit 0
fi

if gh release view "$TAG" --repo "$REPO" >/dev/null 2>&1; then
  log "release $TAG already exists, uploading the assets"
  if [ ${#files[@]} -gt 0 ]; then
    gh release upload "$TAG" "${files[@]}" --repo "$REPO" --clobber
  fi
  if [ -n "$CHECKSUMS" ]; then
    # the checksums of the assets that were just uploaded, the notes of the
    # release itself are kept
    gh release view "$TAG" --repo "$REPO" --json body --jq .body \
      | strip_checksums > "$WORK/notes.md"
    printf '\n' >> "$WORK/notes.md"
    checksum_section >> "$WORK/notes.md"
    gh release edit "$TAG" --repo "$REPO" --notes-file "$WORK/notes.md"
  fi
else
  log "creating the release $TAG in $REPO"
  args=(release create "$TAG" --repo "$REPO" --title "$NAME" --notes "$(compose_notes)")
  [ "$DRAFT" = 1 ] && args+=(--draft)
  [ "$PRERELEASE" = 1 ] && args+=(--prerelease)
  if [ ${#files[@]} -gt 0 ]; then
    gh "${args[@]}" "${files[@]}"
  else
    gh "${args[@]}"
  fi
fi

url="$(gh release view "$TAG" --repo "$REPO" --json url --jq .url)"
log "published: $url"
printf '%s\n' "$url"
