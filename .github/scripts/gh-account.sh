#!/usr/bin/env bash
# ============================================================================
# gh-account.sh -- everything the agent may do with the GitHub account
# ----------------------------------------------------------------------------
# The agent runs with a token of the owner, so it can work on every
# repository of the account.  This script wraps the account level operations
# so that they are done in one well tested place:
#
#   gh-account.sh whoami                     who am I, what can I do
#   gh-account.sh scopes                     the scopes of the token in use
#   gh-account.sh profile                    show the profile
#   gh-account.sh profile bio="..." company="..." hireable=false
#   gh-account.sh username                   show the login
#   gh-account.sh username newlogin          explain how to rename the account
#   gh-account.sh emails                     list the e-mail addresses
#   gh-account.sh ssh-keys                   list the public SSH keys
#   gh-account.sh pat --name NAME [--repo OWNER/NAME] [--out FILE]
#   gh-account.sh repo-create NAME [--public|--private] [--description TEXT]
#
# The tokens that are used:
#
#   OPENCODE_ADMIN_TOKEN  a fine grained token with "Administration: write".
#                         It is the only way to create further tokens and to
#                         change the account itself.  Without it the account
#                         level operations that need more than the "repo"
#                         scope fail with a clear message.
#   GH_TOKEN              the classic token of the owner, used as a fallback.
#   GITHUB_TOKEN          the token of the workflow, used as a last resort.
# ============================================================================
set -euo pipefail

log() { printf '==> %s\n' "$*" >&2; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

usage() {
  sed -n '3,30p' "$0" | sed 's/^# \{0,1\}//'
  exit "${1:-0}"
}

# --------------------------------------------------------------------- token
account_token() {
  for t in "${OPENCODE_ADMIN_TOKEN:-}" "${GH_TOKEN:-}" "${GITHUB_TOKEN:-}"; do
    if [ -n "$t" ]; then printf '%s' "$t"; return 0; fi
  done
  return 1
}

api() { # api <method> <path> [gh api arguments...]
  local method="$1" path="$2"; shift 2
  local token
  if token="$(account_token)"; then
    GH_TOKEN="$token" gh api -X "$method" "$path" "$@"
  else
    # no token in the environment, the gh CLI uses its own authentication
    gh api -X "$method" "$path" "$@"
  fi
}

json_escape() {
  printf '%s' "$1" | sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' -e ':a' -e 'N' -e '$!ba' -e 's/\n/\\n/g'
}

login() { api GET /user --jq .login; }

# ------------------------------------------------------------------- scopes
cmd_whoami() {
  log "the token in use:"
  gh api -i /user 2>/dev/null | grep -i '^x-oauth-scopes:' | sed 's/^/  scopes: /' || true
  api GET /user --jq '{login, id, name, email, company, blog, location, bio, plan: .plan.name, two_factor: .two_factor_authentication}'
}

cmd_scopes() {
  gh api -i /user 2>/dev/null | grep -i '^x-oauth-scopes:' || true
}

# ------------------------------------------------------------------ profile
cmd_profile() {
  if [ $# -eq 0 ]; then
    api GET /user --jq '{login, name, email, company, blog, location, bio, twitter_username, hireable}'
    return
  fi
  local body="{" first=1 k v
  for kv in "$@"; do
    k="${kv%%=*}"; v="${kv#*=}"
    [ "$k" = "$kv" ] && die "expected key=value, got: $kv"
    [ $first -eq 1 ] || body="$body,"
    first=0
    case "$k" in
      hireable)
        case "$v" in
          true|false) body="$body\"$k\":$v" ;;
          *) die "hireable must be true or false" ;;
        esac ;;
      *) body="$body\"$k\":\"$(json_escape "$v")\"" ;;
    esac
  done
  body="$body}"
  log "updating the profile"
  local tmp; tmp="$(mktemp)"
  printf '%s' "$body" > "$tmp"
  api PATCH /user --input "$tmp" --jq '{login, name, email, company, blog, location, bio, twitter_username, hireable}'
  rm -f "$tmp"
}

# ----------------------------------------------------------------- username
cmd_username() {
  local current; current="$(login)"
  if [ $# -eq 0 ]; then
    printf 'login: %s\n' "$current"
    printf 'url:   https://github.com/%s\n' "$current"
    return
  fi
  local wanted="$1"
  cat <<EOF

GitHub has no REST API to change the login of a user account.  The login can
only be changed in the browser:

  1. open https://github.com/settings
  2. in the "Public profile" section set the new name and confirm
  3. confirm the password

Everything else about the account (display name, e-mail, bio, company, blog,
location, SSH keys, tokens, repository visibility) is done through the
commands of this script.

Facts about a rename:
  * the current login is      $current
  * the requested login is    $wanted
  * after the rename the old URLs redirect, but the clone URLs that use the
    old login stop working
  * the agent rewrites every github.com remote to the token, so the git
    operations of the agent keep working after the rename
EOF
}

# ------------------------------------------------------------- emails, keys
cmd_emails()  { api GET /user/emails --jq '.[] | {email, primary, verified, visibility}'; }
cmd_ssh_keys() { api GET /user/keys --jq '.[] | {id, title, key, created_at, verified}'; }

# --------------------------------------------------------------- token (PAT)
cmd_pat() {
  local name="" out="" repos="" description="created by the opencode agent" expiry_days=90
  while [ $# -gt 0 ]; do
    case "$1" in
      --name)        name="${2:?}"; shift 2 ;;
      --out)         out="${2:?}"; shift 2 ;;
      --repo)        repos="${repos:+$repos,}${2:?}"; shift 2 ;;
      --description) description="${2:?}"; shift 2 ;;
      --expiry)      expiry_days="${2:?}"; shift 2 ;;
      -h|--help)     usage 0 ;;
      *) die "unknown option: $1" ;;
    esac
  done
  [ -n "$name" ] || die "--name is required"

  local admin="${OPENCODE_ADMIN_TOKEN:-}"
  if [ -z "$admin" ]; then
    die "creating a token needs the OPENCODE_ADMIN_TOKEN secret: a fine grained
     personal access token with the 'Administration: write' permission.
     A classic token cannot create other tokens."
  fi

  local who; who="$(login)"
  local expiration
  expiration="$(date -u -d "+$expiry_days days" +%Y-%m-%dT%H:%M:%SZ 2>/dev/null \
    || date -u -v+"$expiry_days"d +%Y-%m-%dT%H:%M:%SZ)"

  # the repositories the new token gets access to
  local list="${repos:-}"
  if [ -z "$list" ]; then
    log "no --repo given, the token gets access to every repository of $who"
    list="$(gh repo list "$who" --limit 200 --json nameWithOwner --jq '.[].nameWithOwner' | tr '\n' ',' | sed 's/,$//')"
  fi

  local perms='"contents":"write","issues":"write","pull_requests":"write","workflows":"write","actions":"write","packages":"write","metadata":"read","administration":"write"'
  local access="" r id
  local IFS=','
  for r in $list; do
    [ -n "$r" ] || continue
    id="$(api GET "/repos/$r" --jq .id)" || die "cannot read the repository $r"
    access="$access${access:+,}{\"repository_id\":$id,\"permissions\":{$perms}}"
  done
  unset IFS

  local body
  body="{\"name\":\"$(json_escape "$name")\","
  body="$body\"description\":\"$(json_escape "$description")\","
  body="$body\"expiration\":\"$expiration\","
  body="$body\"permissions\":{\"administration\":\"write\"},"
  body="$body\"repository_access\":[$access]}"

  log "creating a fine grained token for: $list"
  local tmp; tmp="$(mktemp)"
  printf '%s' "$body" > "$tmp"
  local response
  response="$(api POST "/users/$who/fine-grained-personal-access-tokens" --input "$tmp")" \
    || die "the API refused the request, read the message above"
  rm -f "$tmp"

  local token
  token="$(printf '%s' "$response" | sed -n 's/.*"token"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p')"
  [ -n "$token" ] || die "the API did not return a token"

  if [ -n "$out" ]; then
    umask 077
    printf '%s\n' "$token" > "$out"
    log "the token is in $out (mode 600)"
    printf 'store it with:  gh secret set %s --repo %s < %s\n' "${SECRET_NAME:-GITHUB_TOKEN}" "$GITHUB_REPOSITORY" "$out"
  else
    printf '%s\n' "$token"
  fi
}

# -------------------------------------------------------------------- repos
cmd_repo_create() {
  local name="" visibility="private" description="" has_description=0
  while [ $# -gt 0 ]; do
    case "$1" in
      --name)        name="${2:?}"; shift 2 ;;
      --public)      visibility="public"; shift ;;
      --private)     visibility="private"; shift ;;
      --description) description="${2:?}"; shift 2; has_description=1 ;;
      -h|--help)     usage 0 ;;
      *) die "unknown option: $1" ;;
    esac
  done
  [ -n "$name" ] || die "--name is required"
  local body="{\"name\":\"$(json_escape "$name")\",\"private\":$([ "$visibility" = private ] && echo true || echo false)"
  [ $has_description -eq 1 ] && body="$body,\"description\":\"$(json_escape "$description")\""
  body="$body}"
  log "creating the repository $visibility $name"
  local tmp; tmp="$(mktemp)"
  printf '%s' "$body" > "$tmp"
  api POST /user/repos --input "$tmp" --jq '{name, visibility, html_url, default_branch}'
  rm -f "$tmp"
}

# -------------------------------------------------------------------- main
command="${1:-}"
[ $# -gt 0 ] && shift
case "$command" in
  whoami)         cmd_whoami "$@" ;;
  scopes)         cmd_scopes "$@" ;;
  profile)        cmd_profile "$@" ;;
  username)       cmd_username "$@" ;;
  emails)         cmd_emails "$@" ;;
  ssh-keys)       cmd_ssh_keys "$@" ;;
  pat)            cmd_pat "$@" ;;
  repo-create)    cmd_repo_create "$@" ;;
  -h|--help|help) usage 0 ;;
  "")             usage 1 ;;
  *)              die "unknown command: $command (try --help)" ;;
esac
