#define _GNU_SOURCE

#include "sipolicy.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/audit.h>
#include <linux/seccomp.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define MAX_LISTENERS 128
#define LOG_ROTATE_SIZE (2 * 1024 * 1024)

#ifndef MSG_CMSG_CLOEXEC
#define MSG_CMSG_CLOEXEC 0x40000000
#endif

struct listener {
    int fd;
    pid_t pid;
    time_t since;
};

static struct sip_ruleset g_rules;
static const char *g_data_dir = SIP_DATA_DIR;
static const char *g_socket_path = SIP_SOCKET_PATH;
static volatile sig_atomic_t g_running = 1;
static volatile sig_atomic_t g_reload = 0;
static struct listener g_listeners[MAX_LISTENERS];
static int g_listener_count;
static time_t g_started;

static void on_term(int sig)
{
    (void)sig;
    g_running = 0;
}

static void on_hup(int sig)
{
    (void)sig;
    g_reload = 1;
}

static void log_msg(const char *fmt, ...)
{
    char path[SIP_MAX_PATH];
    char line[2048];
    char stamp[64];
    struct stat st;
    struct tm tm;
    time_t now;
    va_list ap;
    int fd;
    int n;

    snprintf(path, sizeof(path), "%s/sipolicy.log", g_data_dir);
    if (stat(path, &st) == 0 && st.st_size > LOG_ROTATE_SIZE) {
        char old[SIP_MAX_PATH + 4];
        snprintf(old, sizeof(old), "%s.1", path);
        rename(path, old);
    }

    fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd < 0)
        return;

    now = time(NULL);
    gmtime_r(&now, &tm);
    strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%SZ", &tm);

    n = snprintf(line, sizeof(line), "%s sipolicyd[%d]: ", stamp, (int)getpid());
    if (n < 0 || (size_t)n >= sizeof(line)) {
        close(fd);
        return;
    }
    va_start(ap, fmt);
    vsnprintf(line + n, sizeof(line) - (size_t)n, fmt, ap);
    va_end(ap);

    (void)sip_write_full(fd, line, strlen(line));
    (void)sip_write_full(fd, "\n", 1);
    close(fd);
}

static void write_state(const char *extra)
{
    char path[SIP_MAX_PATH];
    char tmp[SIP_MAX_PATH];
    char text[4096];
    char stamp[64];
    struct tm tm;
    time_t now;
    int fd;
    int n;

    now = time(NULL);
    gmtime_r(&now, &tm);
    strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%SZ", &tm);

    snprintf(path, sizeof(path), "%s/state", g_data_dir);
    snprintf(tmp, sizeof(tmp), "%s/state.tmp", g_data_dir);

    n = snprintf(text, sizeof(text),
                 "version=%s\n"
                 "pid=%d\n"
                 "socket=%s\n"
                 "rules=%zu\n"
                 "rule_files=%zu\n"
                 "default=%s\n"
                 "started=%s\n"
                 "updated=%s\n",
                 SIPOLICY_VERSION, (int)getpid(), g_socket_path,
                 g_rules.count, g_rules.file_count,
                 g_rules.default_action == SIP_ACTION_DENY ? "deny" : "allow",
                 stamp, stamp);
    if (extra && *extra) {
        if (n > 0 && (size_t)n < sizeof(text))
            snprintf(text + n, sizeof(text) - (size_t)n, "note=%s\n", extra);
    }

    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        return;
    (void)sip_write_full(fd, text, strlen(text));
    close(fd);
    rename(tmp, path);
}

static int send_reply(int fd, int allowed, int error,
                      const char *reason, const char *data)
{
    struct sip_reply r;
    size_t rlen = reason ? strlen(reason) + 1 : 0;
    size_t dlen = data ? strlen(data) + 1 : 0;

    if (rlen > SIP_MAX_REASON)
        rlen = SIP_MAX_REASON;
    if (dlen > SIP_MAX_PAYLOAD)
        dlen = SIP_MAX_PAYLOAD;

    memset(&r, 0, sizeof(r));
    r.magic = SIP_MAGIC;
    r.allowed = allowed;
    r.error = error;
    r.reason_len = (uint32_t)rlen;
    r.data_len = (uint32_t)dlen;

    if (sip_write_full(fd, &r, sizeof(r)) != 0)
        return -1;
    if (rlen && sip_write_full(fd, reason, rlen) != 0)
        return -1;
    if (dlen && sip_write_full(fd, data, dlen) != 0)
        return -1;
    return 0;
}

static int reload_rules(void)
{
    struct sip_ruleset next;
    char err[SIP_MAX_REASON];

    memset(err, 0, sizeof(err));
    if (sip_rules_init(&next) != 0)
        return -1;
    if (sip_rules_load(&next, g_data_dir, err, sizeof(err)) != 0) {
        sip_rules_free(&next);
        log_msg("rule reload failed: %s", err[0] ? err : "unknown error");
        write_state("rule reload failed");
        return -1;
    }

    sip_rules_free(&g_rules);
    g_rules = next;
    log_msg("loaded %zu rules from %zu file(s), default=%s%s%s",
            g_rules.count, g_rules.file_count,
            g_rules.default_action == SIP_ACTION_DENY ? "deny" : "allow",
            err[0] ? ", note=" : "", err[0] ? err : "");
    write_state(err);
    return 0;
}

static int arch_ptr_size(uint32_t arch)
{
    return (arch & __AUDIT_ARCH_64BIT) ? 8 : 4;
}

static ssize_t read_mem(pid_t pid, uint64_t addr, void *buf, size_t len)
{
#ifdef __NR_process_vm_readv
    struct iovec local;
    struct iovec remote;
    ssize_t n;

    local.iov_base = buf;
    local.iov_len = len;
    remote.iov_base = (void *)(uintptr_t)addr;
    remote.iov_len = len;
    n = syscall(__NR_process_vm_readv, pid, &local, 1, &remote, 1, 0);
    if (n > 0)
        return n;
#endif
    {
        char path[64];
        int fd;
        ssize_t n;

        snprintf(path, sizeof(path), "/proc/%d/mem", (int)pid);
        fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0)
            return -1;
        n = pread(fd, buf, len, (off_t)addr);
        close(fd);
        return n;
    }
}

static int read_cstr(pid_t pid, uint64_t addr, char *out, size_t outlen)
{
    size_t done = 0;

    if (!addr || !out || outlen == 0)
        return -1;
    out[0] = '\0';

    while (done + 1 < outlen) {
        char chunk[256];
        size_t want = sizeof(chunk);
        ssize_t n;
        size_t i;

        if (want > outlen - 1 - done)
            want = outlen - 1 - done;
        n = read_mem(pid, addr + done, chunk, want);
        if (n <= 0)
            break;
        for (i = 0; i < (size_t)n; i++) {
            if (chunk[i] == '\0') {
                out[done] = '\0';
                return 0;
            }
            out[done++] = chunk[i];
        }
        if ((size_t)n < want)
            break;
    }
    out[done] = '\0';
    return done ? 0 : -1;
}

static int read_ptr(pid_t pid, uint64_t addr, uint64_t *value, int ptr_size)
{
    if (ptr_size == 8) {
        uint64_t v;
        if (read_mem(pid, addr, &v, sizeof(v)) != (ssize_t)sizeof(v))
            return -1;
        *value = v;
        return 0;
    } else {
        uint32_t v;
        if (read_mem(pid, addr, &v, sizeof(v)) != (ssize_t)sizeof(v))
            return -1;
        *value = v;
        return 0;
    }
}

static void read_arg(pid_t pid, uint64_t argv_ptr, int index, int ptr_size,
                     char *out, size_t outlen)
{
    uint64_t ptr = 0;

    out[0] = '\0';
    if (!argv_ptr)
        return;
    if (read_ptr(pid, argv_ptr + (uint64_t)index * (uint64_t)ptr_size,
                 &ptr, ptr_size) != 0)
        return;
    if (ptr)
        (void)read_cstr(pid, ptr, out, outlen);
}

static int is_shell(const char *path)
{
    const char *b = sip_basename(path);

    return strcmp(b, "sh") == 0 || strcmp(b, "bash") == 0 ||
           strcmp(b, "mksh") == 0 || strcmp(b, "ash") == 0 ||
           strcmp(b, "dash") == 0;
}

static int tracee_uid(pid_t pid)
{
    char path[64];
    struct stat st;

    snprintf(path, sizeof(path), "/proc/%d", (int)pid);
    if (stat(path, &st) == 0)
        return (int)st.st_uid;
    return -1;
}

/*
 * Build the check from the notification and ask the rule engine.  The
 * kernel blocks the tracee in execve() until the response is sent.
 */
static void handle_seccomp(struct listener *l)
{
    for (;;) {
        struct seccomp_notif req;
        struct seccomp_notif_resp resp;
        char path[SIP_MAX_CMD];
        char name[SIP_MAX_NAME];
        char a1[64];
        char a2[SIP_MAX_CMD];
        const struct sip_rule *rule = NULL;
        int ptr_size;
        int op = SIP_OP_EXEC;
        int uid;
        int action;
        int allowed;
        int is_execveat = 0;
        uint64_t fname_ptr;
        uint64_t argv_ptr;

        memset(&req, 0, sizeof(req));
        if (ioctl(l->fd, SECCOMP_IOCTL_NOTIF_RECV, &req) != 0) {
            if (errno == EINTR)
                continue;
            break;
        }

        ptr_size = arch_ptr_size(req.data.arch);
        path[0] = '\0';
        name[0] = '\0';
        a1[0] = '\0';
        a2[0] = '\0';

#ifdef __NR_execveat
        if ((long)req.data.nr == (long)__NR_execveat)
            is_execveat = 1;
#endif
        if (is_execveat) {
            fname_ptr = req.data.args[1];
            argv_ptr = req.data.args[2];
        } else {
            fname_ptr = req.data.args[0];
            argv_ptr = req.data.args[1];
        }

        (void)read_cstr((pid_t)req.pid, fname_ptr, path, sizeof(path));
        if (path[0] == '\0' && is_execveat && (req.data.args[4] & AT_EMPTY_PATH)) {
            char link[64];
            ssize_t n;
            snprintf(link, sizeof(link), "/proc/%u/fd/%d",
                     req.pid, (int)req.data.args[0]);
            n = readlink(link, path, sizeof(path) - 1);
            if (n > 0) {
                path[n] = '\0';
            } else {
                path[0] = '\0';
            }
        }

        /*
         * Match cmd rules against the real executable basename, never
         * against argv[0], which the caller controls.
         */
        if (path[0])
            snprintf(name, sizeof(name), "%s", sip_basename(path));

        if (path[0] && is_shell(path)) {
            read_arg((pid_t)req.pid, argv_ptr, 1, ptr_size, a1, sizeof(a1));
            if (strcmp(a1, "-c") == 0) {
                read_arg((pid_t)req.pid, argv_ptr, 2, ptr_size,
                         a2, sizeof(a2));
                if (a2[0]) {
                    op = SIP_OP_SHELL;
                    snprintf(path, sizeof(path), "%s", a2);
                    sip_shell_name(a2, name, sizeof(name));
                }
            }
        }

        uid = tracee_uid((pid_t)req.pid);
        if (sip_is_own_tool(path, name)) {
            allowed = 1;
            action = SIP_ACTION_ALLOW;
        } else {
            action = sip_rules_decide(&g_rules, op, path, name, uid, &rule);
            allowed = (action == SIP_ACTION_ALLOW);
        }

        memset(&resp, 0, sizeof(resp));
        resp.id = req.id;
        if (allowed) {
            resp.flags = SECCOMP_USER_NOTIF_FLAG_CONTINUE;
        } else {
            resp.val = 0;
            resp.error = -EACCES;
            resp.flags = 0;
        }
        if (ioctl(l->fd, SECCOMP_IOCTL_NOTIF_SEND, &resp) != 0) {
            if (errno == ENOENT)
                continue;
            log_msg("seccomp response failed for pid=%u: %s",
                    req.pid, strerror(errno));
            continue;
        }

        if (rule) {
            log_msg("%s uid=%d pid=%u op=%s target=%s rule=%s:%d",
                    allowed ? "ALLOW" : "DENY", uid, req.pid,
                    op == SIP_OP_SHELL ? "shell" : "exec",
                    path[0] ? path : "(unreadable)",
                    rule->source ? rule->source : "?", rule->line);
        } else if (!allowed) {
            log_msg("DENY uid=%d pid=%u op=%s target=%s default=%s",
                    uid, req.pid, op == SIP_OP_SHELL ? "shell" : "exec",
                    path[0] ? path : "(unreadable)",
                    g_rules.default_action == SIP_ACTION_DENY ? "deny" : "allow");
        }
    }
}

static int recv_fd(int sock)
{
    char buf[1];
    struct iovec iov;
    struct msghdr msg;
    struct cmsghdr *cmsg;
    char cbuf[CMSG_SPACE(sizeof(int))];
    ssize_t n;

    iov.iov_base = buf;
    iov.iov_len = sizeof(buf);
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cbuf;
    msg.msg_controllen = sizeof(cbuf);

    n = recvmsg(sock, &msg, MSG_CMSG_CLOEXEC);
    if (n < 0)
        return -1;
    for (cmsg = CMSG_FIRSTHDR(&msg); cmsg; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
        if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
            int fd;
            memcpy(&fd, CMSG_DATA(cmsg), sizeof(fd));
            return fd;
        }
    }
    return -1;
}

static int add_listener(int fd, pid_t pid)
{
    struct timeval tv;

    if (g_listener_count >= MAX_LISTENERS)
        return -1;
    tv.tv_sec = 5;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    g_listeners[g_listener_count].fd = fd;
    g_listeners[g_listener_count].pid = pid;
    g_listeners[g_listener_count].since = time(NULL);
    g_listener_count++;
    return 0;
}

static void remove_listener(int index)
{
    if (index < 0 || index >= g_listener_count)
        return;
    close(g_listeners[index].fd);
    g_listeners[index] = g_listeners[g_listener_count - 1];
    g_listener_count--;
}

static void control_connection(int fd)
{
    struct sip_msg m;
    struct ucred cred;
    socklen_t cred_len = sizeof(cred);
    int have_cred;
    struct timeval tv;

    tv.tv_sec = 3;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (sip_read_full(fd, &m, sizeof(m)) != 0)
        return;
    if (m.magic != SIP_MAGIC)
        return;

    have_cred = getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &cred_len) == 0;

    switch (m.type) {
    case SIP_MSG_CHECK: {
        char *payload;
        char *path;
        char *name;
        char reason[SIP_MAX_REASON];
        const struct sip_rule *rule = NULL;
        int uid;
        int action;
        int allowed;

        if (m.path_len == 0 || m.path_len > SIP_MAX_PATH ||
            m.name_len == 0 || m.name_len > SIP_MAX_NAME ||
            (size_t)m.path_len + m.name_len > SIP_MAX_PAYLOAD)
            return;
        payload = malloc((size_t)m.path_len + m.name_len);
        if (!payload)
            return;
        if (sip_read_full(fd, payload, (size_t)m.path_len + m.name_len) != 0) {
            free(payload);
            return;
        }
        path = payload;
        name = payload + m.path_len;
        if (path[m.path_len - 1] != '\0' || name[m.name_len - 1] != '\0') {
            free(payload);
            return;
        }
        uid = have_cred ? (int)cred.uid : m.uid;
        if (sip_is_own_tool(path, name)) {
            action = SIP_ACTION_ALLOW;
            allowed = 1;
            snprintf(reason, sizeof(reason), "sipolicy tooling is always allowed");
        } else {
            action = sip_rules_decide(&g_rules, (int)m.op, path, name, uid, &rule);
            allowed = (action == SIP_ACTION_ALLOW);
            if (rule)
                snprintf(reason, sizeof(reason), "%s by %s:%d",
                         allowed ? "allowed" : "denied",
                         rule->source ? rule->source : "?", rule->line);
            else
                snprintf(reason, sizeof(reason), "%s by default policy",
                         allowed ? "allowed" : "denied");
        }
        log_msg("%s uid=%d pid=%d op=%s target=%s (%s)",
                allowed ? "ALLOW" : "DENY", uid, m.pid,
                m.op == SIP_OP_SHELL ? "shell" : "exec", path, reason);
        (void)send_reply(fd, allowed, allowed ? 0 : EACCES, reason, NULL);
        free(payload);
        break;
    }
    case SIP_MSG_SUPERVISE: {
        int listener = recv_fd(fd);
        char reason[128];
        if (listener < 0) {
            (void)send_reply(fd, 0, EINVAL, "missing listener fd", NULL);
            break;
        }
        if (add_listener(listener, (pid_t)m.pid) != 0) {
            close(listener);
            (void)send_reply(fd, 0, ENOSPC, "listener table full", NULL);
            break;
        }
        log_msg("supervising pid=%d via seccomp user notification", m.pid);
        snprintf(reason, sizeof(reason), "supervising pid %d", m.pid);
        (void)send_reply(fd, 1, 0, reason, NULL);
        break;
    }
    case SIP_MSG_RELOAD: {
        int ok = reload_rules();
        char reason[SIP_MAX_REASON];
        snprintf(reason, sizeof(reason), "%s (%zu rules in %zu files)",
                 ok == 0 ? "rules reloaded" : "rule reload failed",
                 g_rules.count, g_rules.file_count);
        (void)send_reply(fd, ok == 0 ? 1 : 0, ok == 0 ? 0 : EINVAL,
                         reason, NULL);
        break;
    }
    case SIP_MSG_STATUS: {
        char data[2048];
        snprintf(data, sizeof(data),
                 "sipolicy %s\n"
                 "pid: %d\n"
                 "socket: %s\n"
                 "rules: %zu\n"
                 "rule files: %zu\n"
                 "default: %s\n"
                 "supervised listeners: %d\n"
                 "uptime: %ld seconds\n",
                 SIPOLICY_VERSION, (int)getpid(), g_socket_path,
                 g_rules.count, g_rules.file_count,
                 g_rules.default_action == SIP_ACTION_DENY ? "deny" : "allow",
                 g_listener_count, (long)(time(NULL) - g_started));
        (void)send_reply(fd, 1, 0, "status", data);
        break;
    }
    case SIP_MSG_RULES: {
        char data[8192];
        size_t used = 0;
        size_t i;
        data[0] = '\0';
        for (i = 0; i < g_rules.file_count && used < sizeof(data); i++) {
            int n = snprintf(data + used, sizeof(data) - used, "%s\n",
                             g_rules.files[i]);
            if (n < 0)
                break;
            used += (size_t)n;
        }
        (void)send_reply(fd, 1, 0, "rule files", data);
        break;
    }
    case SIP_MSG_PING:
        (void)send_reply(fd, 1, 0, "pong", SIPOLICY_VERSION);
        break;
    default:
        (void)send_reply(fd, 0, EINVAL, "unknown message", NULL);
        break;
    }
}

static void usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s [--data DIR] [--socket PATH]\n"
            "       %s --version\n"
            "The daemon runs in the foreground; service.sh starts it in the\n"
            "background.  It answers execve decisions from the kernel through\n"
            "seccomp user notification and enforces sipolicy.rule files.\n",
            argv0, argv0);
}

int main(int argc, char **argv)
{
    char pid_path[SIP_MAX_PATH];
    char sock_path[SIP_MAX_PATH];
    struct sockaddr_un sa;
    struct flock lock;
    int lock_fd;
    int listen_fd;
    int socket_explicit = 0;
    int i = 1;

    while (i < argc) {
        if (strcmp(argv[i], "--data") == 0 && i + 1 < argc) {
            g_data_dir = argv[++i];
        } else if (strcmp(argv[i], "--socket") == 0 && i + 1 < argc) {
            g_socket_path = argv[++i];
            socket_explicit = 1;
        } else if (strcmp(argv[i], "--version") == 0) {
            printf("sipolicyd %s\n", SIPOLICY_VERSION);
            return 0;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            usage(argv[0]);
            return 2;
        }
        i++;
    }

    if (sip_mkdir_p(g_data_dir, 0700) != 0) {
        fprintf(stderr, "sipolicyd: cannot create %s: %s\n",
                g_data_dir, strerror(errno));
        return 1;
    }
    {
        char rules_dir[SIP_MAX_PATH];
        snprintf(rules_dir, sizeof(rules_dir), "%s/rules", g_data_dir);
        (void)sip_mkdir_p(rules_dir, 0700);
        chmod(rules_dir, 0700);
    }
    chmod(g_data_dir, 0700);

    snprintf(pid_path, sizeof(pid_path), "%s/sipolicyd.pid", g_data_dir);
    lock_fd = open(pid_path, O_RDWR | O_CREAT, 0600);
    if (lock_fd < 0) {
        fprintf(stderr, "sipolicyd: cannot open %s: %s\n",
                pid_path, strerror(errno));
        return 1;
    }
    memset(&lock, 0, sizeof(lock));
    lock.l_type = F_WRLCK;
    lock.l_whence = SEEK_SET;
    if (fcntl(lock_fd, F_SETLK, &lock) != 0) {
        fprintf(stderr, "sipolicyd: another instance is already running\n");
        close(lock_fd);
        return 0;
    }
    if (ftruncate(lock_fd, 0) != 0) {
        /* not fatal */
    }
    {
        char pidbuf[32];
        int n = snprintf(pidbuf, sizeof(pidbuf), "%d\n", (int)getpid());
        (void)sip_write_full(lock_fd, pidbuf, (size_t)n);
    }

    signal(SIGPIPE, SIG_IGN);
    signal(SIGTERM, on_term);
    signal(SIGINT, on_term);
    signal(SIGHUP, on_hup);

    if (!socket_explicit) {
        snprintf(sock_path, sizeof(sock_path), "%s/sipolicy.sock", g_data_dir);
        g_socket_path = sock_path;
    }
    if (strlen(g_socket_path) >= sizeof(sa.sun_path)) {
        fprintf(stderr, "sipolicyd: socket path too long\n");
        return 1;
    }

    if (sip_rules_init(&g_rules) != 0) {
        fprintf(stderr, "sipolicyd: out of memory\n");
        return 1;
    }
    reload_rules();

    unlink(g_socket_path);
    listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        fprintf(stderr, "sipolicyd: socket: %s\n", strerror(errno));
        return 1;
    }
    fcntl(listen_fd, F_SETFD, FD_CLOEXEC);
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    strcpy(sa.sun_path, g_socket_path);
    if (bind(listen_fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        fprintf(stderr, "sipolicyd: bind %s: %s\n",
                g_socket_path, strerror(errno));
        close(listen_fd);
        return 1;
    }
    chmod(g_socket_path, 0600);
    if (listen(listen_fd, 16) != 0) {
        fprintf(stderr, "sipolicyd: listen: %s\n", strerror(errno));
        close(listen_fd);
        return 1;
    }

    g_started = time(NULL);
    log_msg("started %s, data=%s, socket=%s, default=%s",
            SIPOLICY_VERSION, g_data_dir, g_socket_path,
            g_rules.default_action == SIP_ACTION_DENY ? "deny" : "allow");
    write_state(NULL);

    while (g_running) {
        struct pollfd fds[MAX_LISTENERS + 1];
        int active;
        int nfds = 1;
        int rc;

        if (g_reload) {
            g_reload = 0;
            reload_rules();
        }

        /*
         * A new listener can be added while the accept above is handled.
         * Only the listeners that were present before poll() have a valid
         * entry in fds[], so remember that count for the cleanup pass.
         */
        active = g_listener_count;
        fds[0].fd = listen_fd;
        fds[0].events = POLLIN;
        fds[0].revents = 0;
        for (i = 0; i < active; i++) {
            fds[nfds].fd = g_listeners[i].fd;
            fds[nfds].events = POLLIN;
            fds[nfds].revents = 0;
            nfds++;
        }

        rc = poll(fds, (nfds_t)nfds, 500);
        if (rc < 0) {
            if (errno == EINTR)
                continue;
            log_msg("poll failed: %s", strerror(errno));
            break;
        }
        if (rc == 0)
            continue;

        if (fds[0].revents & POLLIN) {
            int client = accept(listen_fd, NULL, NULL);
            if (client >= 0) {
                fcntl(client, F_SETFD, FD_CLOEXEC);
                control_connection(client);
                close(client);
            }
        }

        for (i = active - 1; i >= 0; i--) {
            short re = fds[i + 1].revents;
            if (re & POLLIN) {
                handle_seccomp(&g_listeners[i]);
            }
            if (re & (POLLHUP | POLLERR | POLLNVAL)) {
                log_msg("listener for pid=%d closed",
                        (int)g_listeners[i].pid);
                remove_listener(i);
            }
        }
    }

    log_msg("stopping");
    unlink(g_socket_path);
    {
        char state[SIP_MAX_PATH];
        snprintf(state, sizeof(state), "%s/state", g_data_dir);
        unlink(state);
    }
    unlink(pid_path);
    close(listen_fd);
    for (i = 0; i < g_listener_count; i++)
        close(g_listeners[i].fd);
    sip_rules_free(&g_rules);
    close(lock_fd);
    return 0;
}
