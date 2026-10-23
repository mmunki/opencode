# Changelog

## 0.109

- Initial release.
- Kernel-assisted `execve`/`execveat` policy using seccomp user notification.
- `sipolicyd` daemon with a first-match rule engine, SIGHUP/reload support and
  rotating log.
- `sipolicy` CLI: `status`, `reload`, `rules`, `check`, `run`, `log`, `version`.
- `sipolicy-run` launcher with fail-closed daemon handshake.
- `libsipolicy.so` LD_PRELOAD guard for kernels without seccomp user
  notification.
- Native payloads for `arm64-v8a`, `armeabi-v7a`, `x86_64` and `x86`.
- Central, per-directory and per-module `sipolicy.rule` files.
- Magisk installer, boot service, uninstall hook and default policy.
- Host self-tests for the rule engine and shell-command parsing.
