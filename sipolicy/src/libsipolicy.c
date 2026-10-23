/*
 * libsipolicy.so - LD_PRELOAD guard used when seccomp user notification is
 * not available.  Every intercepted spawn asks sipolicyd first.  A denied
 * command fails with EACCES and never reaches the kernel.
 */
#define _GNU_SOURCE

#include "sipolicy.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

typedef int (*execve_fn)(const char *, char *const[], char *const[]);
typedef int (*execveat_fn)(int, const char *, char *const[], char *const[], int);
typedef int (*execv_fn)(const char *, char *const[]);
typedef int (*execvp_fn)(const char *, char *const[]);
typedef int (*execvpe_fn)(const char *, char *const[], char *const[]);
typedef int (*fexecve_fn)(int, char *const[], char *const[]);
typedef int (*system_fn)(const char *);
typedef FILE *(*popen_fn)(const char *, const char *);
typedef int (*posix_spawn_fn)(pid_t *, const char *,
                              const posix_spawn_file_actions_t *,
                              const posix_spawnattr_t *,
                              char *const[], char *const[]);

static execve_fn real_execve;
static execveat_fn real_execveat;
static execv_fn real_execv;
static execvp_fn real_execvp;
static execvpe_fn real_execvpe;
static fexecve_fn real_fexecve;
static system_fn real_system;
static popen_fn real_popen;
static posix_spawn_fn real_posix_spawn;
static posix_spawn_fn real_posix_spawnp;

static void resolve_symbols(void)
{
    static int done;

    if (done)
        return;
    done = 1;
    real_execve = (execve_fn)dlsym(RTLD_NEXT, "execve");
    real_execveat = (execveat_fn)dlsym(RTLD_NEXT, "execveat");
    real_execv = (execv_fn)dlsym(RTLD_NEXT, "execv");
    real_execvp = (execvp_fn)dlsym(RTLD_NEXT, "execvp");
    real_execvpe = (execvpe_fn)dlsym(RTLD_NEXT, "execvpe");
    real_fexecve = (fexecve_fn)dlsym(RTLD_NEXT, "fexecve");
    real_system = (system_fn)dlsym(RTLD_NEXT, "system");
    real_popen = (popen_fn)dlsym(RTLD_NEXT, "popen");
    real_posix_spawn = (posix_spawn_fn)dlsym(RTLD_NEXT, "posix_spawn");
    real_posix_spawnp = (posix_spawn_fn)dlsym(RTLD_NEXT, "posix_spawnp");
}

__attribute__((constructor))
static void sipolicy_ctor(void)
{
    resolve_symbols();
}

static int fail_closed(void)
{
    const char *env = getenv("SIPOLICY_FAIL_CLOSED");

    if (env && *env && strcmp(env, "0") != 0)
        return 1;
    if (access(SIP_DATA_DIR "/fail_closed", F_OK) == 0)
        return 1;
    return 0;
}

static int ask_daemon(int op, const char *path, const char *name,
                      int *allowed, char *reason, size_t reason_len)
{
    struct sip_msg msg;
    struct sip_reply reply;
    char payload[SIP_MAX_PATH + SIP_MAX_NAME];
    char data[16];
    size_t plen = 0;

    if (!path || !*path)
        return -1;
    if (!name)
        name = sip_basename(path);

    memset(&msg, 0, sizeof(msg));
    memset(&reply, 0, sizeof(reply));
    msg.type = SIP_MSG_CHECK;
    msg.op = (uint32_t)op;
    msg.pid = (int)getpid();
    msg.uid = (int)getuid();

    msg.path_len = (uint32_t)strlen(path) + 1;
    if (msg.path_len > SIP_MAX_PATH)
        return -1;
    memcpy(payload, path, msg.path_len);
    plen = msg.path_len;

    msg.name_len = (uint32_t)strlen(name) + 1;
    if (msg.name_len > SIP_MAX_NAME)
        return -1;
    memcpy(payload + plen, name, msg.name_len);
    plen += msg.name_len;

    if (sip_call(sip_socket_path(), &msg, payload, plen, &reply,
                 reason, reason_len, data, sizeof(data)) != 0)
        return -1;

    *allowed = reply.allowed ? 1 : 0;
    return 0;
}

static int check_or_fail(int op, const char *path, const char *name)
{
    int allowed = 0;
    char reason[SIP_MAX_REASON];
    const char *display = path ? path : (name ? name : "");

    if (getenv("SIPOLICY_DISABLE"))
        return 1;
    if (sip_is_own_tool(path, name))
        return 1;

    reason[0] = '\0';
    if (ask_daemon(op, display, name, &allowed, reason, sizeof(reason)) != 0) {
        if (fail_closed())
            return 0;
        return 1;
    }
    if (!allowed) {
        fprintf(stderr, "sipolicy: denied %s%s%s\n", display,
                reason[0] ? ": " : "", reason);
        errno = EACCES;
        return 0;
    }
    return 1;
}

static void resolve_at(int dirfd, const char *path, char *out, size_t outlen)
{
    char link[64];
    char dir[PATH_MAX];
    ssize_t n;
    size_t dlen;
    size_t plen;

    out[0] = '\0';
    if (!path || outlen == 0)
        return;
    if (path[0] == '/' || dirfd == AT_FDCWD) {
        plen = strlen(path);
        if (plen >= outlen)
            plen = outlen - 1;
        memcpy(out, path, plen);
        out[plen] = '\0';
        return;
    }
    snprintf(link, sizeof(link), "/proc/self/fd/%d", dirfd);
    n = readlink(link, dir, sizeof(dir) - 1);
    if (n <= 0) {
        plen = strlen(path);
        if (plen >= outlen)
            plen = outlen - 1;
        memcpy(out, path, plen);
        out[plen] = '\0';
        return;
    }
    dir[n] = '\0';
    dlen = strlen(dir);
    plen = strlen(path);
    if (dlen + 1 + plen >= outlen)
        plen = outlen - dlen - 2;
    memcpy(out, dir, dlen);
    out[dlen] = '/';
    memcpy(out + dlen + 1, path, plen);
    out[dlen + 1 + plen] = '\0';
}

int execve(const char *path, char *const argv[], char *const envp[])
{
    const char *name = sip_basename(path);

    resolve_symbols();
    if (!check_or_fail(SIP_OP_EXEC, path, name)) {
        errno = EACCES;
        return -1;
    }
    if (!real_execve)
        return (int)syscall(SYS_execve, path, argv, envp);
    return real_execve(path, argv, envp);
}

int execveat(int dirfd, const char *path, char *const argv[],
             char *const envp[], int flags)
{
    char resolved[PATH_MAX];
    const char *p = (const char *)(uintptr_t)path;

    resolve_symbols();
    if ((uintptr_t)p == 0) {
        errno = EFAULT;
        return -1;
    }
    resolved[0] = '\0';
    if (p[0] == '\0' && (flags & AT_EMPTY_PATH)) {
        char link[64];
        ssize_t n;
        snprintf(link, sizeof(link), "/proc/self/fd/%d", dirfd);
        n = readlink(link, resolved, sizeof(resolved) - 1);
        if (n > 0)
            resolved[n] = '\0';
    } else {
        resolve_at(dirfd, p, resolved, sizeof(resolved));
    }
    if (!check_or_fail(SIP_OP_EXEC, resolved[0] ? resolved : p, NULL)) {
        errno = EACCES;
        return -1;
    }
    if (!real_execveat) {
        errno = ENOSYS;
        return -1;
    }
    return real_execveat(dirfd, path, argv, envp, flags);
}

int execv(const char *path, char *const argv[])
{
    resolve_symbols();
    if (!check_or_fail(SIP_OP_EXEC, path, sip_basename(path))) {
        errno = EACCES;
        return -1;
    }
    if (!real_execv)
        return (int)syscall(SYS_execve, path, argv, environ);
    return real_execv(path, argv);
}

int execvp(const char *file, char *const argv[])
{
    resolve_symbols();
    if (!check_or_fail(SIP_OP_EXEC, file, sip_basename(file))) {
        errno = EACCES;
        return -1;
    }
    if (!real_execvp)
        return (int)syscall(SYS_execve, file, argv, environ);
    return real_execvp(file, argv);
}

int execvpe(const char *file, char *const argv[], char *const envp[])
{
    resolve_symbols();
    if (!check_or_fail(SIP_OP_EXEC, file, sip_basename(file))) {
        errno = EACCES;
        return -1;
    }
    if (!real_execvpe) {
        errno = ENOSYS;
        return -1;
    }
    return real_execvpe(file, argv, envp);
}

int fexecve(int fd, char *const argv[], char *const envp[])
{
    char link[64];
    char path[PATH_MAX];
    ssize_t n;

    resolve_symbols();
    snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
    n = readlink(link, path, sizeof(path) - 1);
    if (n > 0)
        path[n] = '\0';
    else
        path[0] = '\0';
    if (!check_or_fail(SIP_OP_EXEC, path, NULL)) {
        errno = EACCES;
        return -1;
    }
    if (!real_fexecve) {
        errno = ENOSYS;
        return -1;
    }
    return real_fexecve(fd, argv, envp);
}

int system(const char *command)
{
    char name[SIP_MAX_NAME];

    resolve_symbols();
    if (command) {
        sip_shell_name(command, name, sizeof(name));
        if (!check_or_fail(SIP_OP_SHELL, command, name)) {
            errno = EACCES;
            return -1;
        }
    }
    if (!real_system) {
        errno = ENOSYS;
        return -1;
    }
    return real_system(command);
}

FILE *popen(const char *command, const char *type)
{
    char name[SIP_MAX_NAME];

    resolve_symbols();
    if (command) {
        sip_shell_name(command, name, sizeof(name));
        if (!check_or_fail(SIP_OP_SHELL, command, name)) {
            errno = EACCES;
            return NULL;
        }
    }
    if (!real_popen) {
        errno = ENOSYS;
        return NULL;
    }
    return real_popen(command, type);
}

int posix_spawn(pid_t *pid, const char *path,
                const posix_spawn_file_actions_t *file_actions,
                const posix_spawnattr_t *attrp,
                char *const argv[], char *const envp[])
{
    resolve_symbols();
    if (!check_or_fail(SIP_OP_EXEC, path, sip_basename(path)))
        return EACCES;
    if (!real_posix_spawn)
        return ENOSYS;
    return real_posix_spawn(pid, path, file_actions, attrp, argv, envp);
}

int posix_spawnp(pid_t *pid, const char *file,
                 const posix_spawn_file_actions_t *file_actions,
                 const posix_spawnattr_t *attrp,
                 char *const argv[], char *const envp[])
{
    resolve_symbols();
    if (!check_or_fail(SIP_OP_EXEC, file, sip_basename(file)))
        return EACCES;
    if (!real_posix_spawnp)
        return ENOSYS;
    return real_posix_spawnp(pid, file, file_actions, attrp, argv, envp);
}
