# sipolicy 0.109

`sipolicy` is a Magisk module that puts a policy daemon between the kernel and
`execve()`.  A supervised process is blocked inside the kernel until
`sipolicyd` has evaluated the command against one or more `sipolicy.rule`
files.  The kernel only continues the call when the daemon allows it; a denied
command fails with `EACCES` and never starts.

The module ships native code for the four Android ABIs:

| ABI | Directory in the zip |
| --- | --- |
| `arm64-v8a` | `native/arm64-v8a/` |
| `armeabi-v7a` | `native/armeabi-v7a/` |
| `x86_64` | `native/x86_64/` |
| `x86` | `native/x86/` |

Each ABI contains:

- `sipolicyd` – the policy daemon.
- `sipolicy` – the command line interface.
- `sipolicy-run` – installs the seccomp filter and hands the listener to the
  daemon.
- `libsipolicy.so` – the `LD_PRELOAD` guard used on kernels without seccomp
  user notification.

`customize.sh` installs only the ABI that matches the device.  No build output
is committed to the source tree.

## How it works

### Kernel path (recommended)

```text
sipolicy run <command>
        |
        |  seccomp(SECCOMP_SET_MODE_FILTER,
        |          SECCOMP_FILTER_FLAG_NEW_LISTENER)
        |  execve/execveat -> SECCOMP_RET_USER_NOTIF
        v
   sipolicyd  <-- kernel blocks the tracee and sends a notification
        |
        |  sipolicy.rule evaluation
        v
  ALLOW -> SECCOMP_USER_NOTIF_FLAG_CONTINUE
  DENY  -> -EACCES
```

The filter is inherited by every child of the supervised process, so the whole
process tree is covered.  `sipolicy-run` refuses to start a command when the
daemon is not reachable (fail closed).

`sh -c '...'` invocations are reconstructed from `argv`, so `deny shell` and
`deny cmd` rules also match commands that a shell would otherwise start.

### Userspace path (fallback and app tooling)

`libsipolicy.so` interposes `execve`, `execveat`, `execv`, `execvp`,
`execvpe`, `fexecve`, `system`, `popen`, `posix_spawn` and `posix_spawnp`.
Every call asks the daemon first and returns `EACCES` / `NULL` on a denial.
It is used automatically by `sipolicy-run` when the kernel does not support
seccomp user notification (Android kernels older than 5.0), and can be
preloaded explicitly:

```sh
LD_PRELOAD=/system/lib64/libsipolicy.so sh -c '...'
```

## Requirements

- Magisk 20.4 or newer.
- Root access to run the daemon and the command line tools.
- Kernel 5.0+ for the kernel path.  Older kernels use the LD_PRELOAD path.

## Install

Flash `sipolicy-0.109.zip` in the Magisk app or from recovery, then reboot.

The installer:

1. selects the device ABI,
2. installs the binaries into `system/bin`,
3. installs `libsipolicy.so` into `system/lib` or `system/lib64`,
4. seeds `/data/adb/sipolicy/sipolicy.rule` if it does not exist,
5. leaves any existing user policy untouched.

After the reboot:

```sh
sipolicy status
sipolicy rules
```

## Quick start

Enable a few examples in `/data/adb/sipolicy/sipolicy.rule`:

```text
deny exec /system/bin/su
deny cmd magisk
deny exec /data/local/tmp/*
deny path */frida-server*
deny shell *rm -rf /*
```

Then reload and test:

```sh
sipolicy reload
sipolicy check /system/bin/su
sipolicy run su -c id
```

`check` asks the daemon without executing anything.  `run` executes under
kernel supervision.

## Rule format

Rules are evaluated in file order.  The first match wins.  If no rule matches,
the `set default` action is used.

```text
set default allow

allow all
allow|deny exec  <glob>    full executable path
allow|deny cmd   <glob>    executable basename, or first word of a shell command
allow|deny path  <glob>    full path (exec and shell targets)
allow|deny shell <glob>    whole "sh -c" command
allow|deny uid   <n>|<lo>-<hi>|<lo>+
```

`<glob>` uses POSIX `fnmatch` (`*`, `?`, `[...]`).  Matching is case sensitive.
Comments start with `#`.

Order matters:

```text
set default allow
allow cmd safe-tool
deny  cmd safe-tool      # unreachable: the allow above wins
```

To build a whitelist, put the default at the top and allow the commands the
device needs:

```text
set default deny
allow cmd sh
allow cmd toybox
allow exec /system/bin/*
allow uid 0
```

## Rules from other modules

Any installed Magisk module can ship a policy fragment in its module root:

```text
/data/adb/modules/<module-id>/sipolicy.rule
```

Additional files can be placed in:

```text
/data/adb/sipolicy/rules/*.rule
```

The daemon loads, in order:

1. `/data/adb/sipolicy/sipolicy.rule`
2. `/data/adb/sipolicy/rules/*.rule` (sorted)
3. `/data/adb/modules/*/sipolicy.rule` (sorted)

This lets a module add or override rules without touching the central file.
Run `sipolicy reload` after changing a rule file.

## Command line

```text
sipolicy status              show daemon state
sipolicy reload              reload all rule files
sipolicy rules               list loaded rule files
sipolicy check <path>        ask the daemon about one executable
sipolicy run <cmd> [args]    run a command under seccomp supervision
sipolicy log [-n N]          show the last N log lines (default 50)
sipolicy version             print the version
```

Logs are written to `/data/adb/sipolicy/sipolicy.log` and rotate at 2 MiB.

## Security model and failure modes

- The daemon socket is created with mode `0600` inside a `0700` data directory,
  so only root can issue checks or ask to be supervised.
- The daemon always allows its own binaries (`sipolicyd`, `sipolicy`,
  `sipolicy-run`), which prevents an accidental lockout of the policy controls.
- `sipolicy run` is fail closed: no daemon, no execution.
- The preloaded library is fail open by default, so a broken daemon cannot make
  the whole device unusable.  For strict deployments create the marker file
  `/data/adb/sipolicy/fail_closed`, or set `SIPOLICY_FAIL_CLOSED=1` in the
  environment.  The guard then denies execution when the daemon is unreachable.
- The seccomp filter uses `PR_SET_NO_NEW_PRIVS`, which is required for an
  unprivileged process to install a filter.  It does not change the daemon's
  own privileges.
- The kernel notification path requires `CAP_SYS_PTRACE` (or equivalent root
  privileges) to read the tracee's `execve` arguments.  If the arguments cannot
  be read, the daemon allows the call and logs it rather than guessing.

## Build

The build script cross-compiles with the Android NDK and writes the flashable
zip into `sipolicy/dist/`:

```sh
sipolicy/build.sh
```

It looks for the NDK in `ANDROID_NDK_HOME`, `ANDROID_NDK_ROOT`,
`ANDROID_HOME/ndk`, `ANDROID_SDK_ROOT/ndk` and the common SDK locations.
The host rule-engine self-tests run before the cross-compile.  The zip is
verified with `unzip -t` before the script returns.

To publish from this repository, use the release helper (build output is never
committed):

```sh
.github/scripts/publish-release.sh \
    --repo mmunki/opencode --tag sipolicy-0.109 \
    --build "bash sipolicy/build.sh" \
    --artifact sipolicy/dist/sipolicy-0.109.zip
```

The helper appends the SHA-256 of every published asset to the release notes.

## Layout

```text
sipolicy/
  module.prop
  customize.sh
  post-fs-data.sh
  service.sh
  uninstall.sh
  rules/sipolicy.rule
  src/                  daemon, CLI, runner, library, rule engine, tests
  META-INF/             Magisk installer entry points
  build.sh              NDK cross-compile + packaging
  README.md
  CHANGELOG.md
```

## Troubleshooting

| Symptom | Check |
| --- | --- |
| `sipolicy status` cannot connect | `cat /data/adb/sipolicy/sipolicy.log`; confirm `service.sh` ran and `sipolicyd` is alive |
| A denied command still runs | it was started outside `sipolicy run` and without the preload library; supervise that process tree |
| `sipolicy run` says seccomp is unavailable | the kernel predates 5.0; the LD_PRELOAD fallback is used if the library is installed |
| Rules do not take effect | run `sipolicy reload`; remember first match wins |
| Device behaves unexpectedly | set the default to allow and remove the last rule; the daemon's own tools are never blocked |
