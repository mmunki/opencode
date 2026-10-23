/*
 * sipolicy-run - install the execve filter and hand the listener to the
 * daemon.  The kernel then asks sipolicyd before every execve/execveat in
 * this process and all of its descendants.
 */
#define _GNU_SOURCE

#include "sipolicy.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

#if defined(__aarch64__)
#define SIP_AUDIT_ARCH AUDIT_ARCH_AARCH64
#elif defined(__arm__)
#define SIP_AUDIT_ARCH AUDIT_ARCH_ARM
#elif defined(__x86_64__)
#define SIP_AUDIT_ARCH AUDIT_ARCH_X86_64
#elif defined(__i386__)
#define SIP_AUDIT_ARCH AUDIT_ARCH_I386
#else
#error "unsupported architecture"
#endif

static int install_filter(void)
{
    uint32_t action = SECCOMP_RET_USER_NOTIF;
    struct sock_fprog prog;
    struct sock_filter filter[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
                 (unsigned int)offsetof(struct seccomp_data, arch)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SIP_AUDIT_ARCH, 1, 0),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),

        BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
                 (unsigned int)offsetof(struct seccomp_data, nr)),
#ifdef __NR_execveat
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_execve, 2, 0),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_execveat, 1, 0),
#else
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_execve, 1, 0),
#endif
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_USER_NOTIF),
    };
    int fd;

    if (syscall(__NR_seccomp, SECCOMP_GET_ACTION_AVAIL, 0, &action) != 0)
        return -1;

    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0)
        return -1;

    memset(&prog, 0, sizeof(prog));
    prog.len = (unsigned short)(sizeof(filter) / sizeof(filter[0]));
    prog.filter = filter;

    fd = (int)syscall(__NR_seccomp, SECCOMP_SET_MODE_FILTER,
                      SECCOMP_FILTER_FLAG_NEW_LISTENER, &prog);
    if (fd < 0)
        return -1;
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    return fd;
}

static int send_listener(int ctl, int listener)
{
    struct sip_msg msg;
    struct sip_reply reply;
    char reason[SIP_MAX_REASON];
    char data[SIP_MAX_PAYLOAD];
    char byte = 'L';
    struct iovec iov;
    struct msghdr mh;
    struct cmsghdr *cmsg;
    char cbuf[CMSG_SPACE(sizeof(int))];
    int rc = -1;

    memset(&msg, 0, sizeof(msg));
    msg.magic = SIP_MAGIC;
    msg.type = SIP_MSG_SUPERVISE;
    msg.pid = (int)getpid();
    msg.uid = (int)getuid();
    msg.argc = 1;

    if (sip_write_full(ctl, &msg, sizeof(msg)) != 0)
        return -1;

    memset(&iov, 0, sizeof(iov));
    iov.iov_base = &byte;
    iov.iov_len = 1;
    memset(&mh, 0, sizeof(mh));
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;
    mh.msg_control = cbuf;
    mh.msg_controllen = sizeof(cbuf);

    cmsg = CMSG_FIRSTHDR(&mh);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cmsg), &listener, sizeof(listener));

    if (sendmsg(ctl, &mh, 0) < 0)
        return -1;

    if (sip_read_full(ctl, &reply, sizeof(reply)) != 0)
        return -1;
    if (reply.magic != SIP_MAGIC)
        return -1;
    if (reply.reason_len > SIP_MAX_REASON ||
        reply.data_len > SIP_MAX_PAYLOAD)
        return -1;
    if (reply.reason_len &&
        sip_read_full(ctl, reason, reply.reason_len) != 0)
        return -1;
    if (reply.data_len &&
        sip_read_full(ctl, data, reply.data_len) != 0)
        return -1;
    if (reply.allowed)
        rc = 0;
    return rc;
}

static int set_preload(void)
{
    const char *lib = getenv("SIPOLICY_LIB");
    char builtin[PATH_MAX];
    const char *old;
    char value[PATH_MAX * 2];

    if (!lib || !*lib) {
        snprintf(builtin, sizeof(builtin), "%s/libsipolicy.so",
                 sizeof(void *) == 8 ? "/system/lib64" : "/system/lib");
        lib = builtin;
    }
    if (access(lib, R_OK) != 0)
        return -1;

    old = getenv("LD_PRELOAD");
    if (old && strstr(old, lib))
        return 0;
    snprintf(value, sizeof(value), "%s%s%s",
             old && *old ? old : "", old && *old ? ":" : "", lib);
    return setenv("LD_PRELOAD", value, 1);
}

int main(int argc, char **argv)
{
    int ctl;
    int listener;

    if (argc < 2) {
        fprintf(stderr,
                "usage: sipolicy-run <command> [args...]\n"
                "Runs the command under sipolicyd supervision.\n");
        return 2;
    }

    ctl = sip_connect(sip_socket_path());
    if (ctl < 0) {
        fprintf(stderr,
                "sipolicy-run: sipolicyd is not reachable at %s: %s\n"
                "sipolicy-run refuses to start an unsupervised command\n",
                sip_socket_path(), strerror(errno));
        return 127;
    }

    listener = install_filter();
    if (listener >= 0) {
        if (send_listener(ctl, listener) != 0) {
            fprintf(stderr, "sipolicy-run: daemon rejected the listener: %s\n",
                    strerror(errno));
            close(listener);
            close(ctl);
            return 127;
        }
        close(listener);
        close(ctl);
        execvp(argv[1], &argv[1]);
        if (errno == EACCES)
            fprintf(stderr, "sipolicy-run: %s was denied by sipolicy\n",
                    argv[1]);
        else
            fprintf(stderr, "sipolicy-run: cannot exec %s: %s\n",
                    argv[1], strerror(errno));
        return 127;
    }

    /*
     * Kernels older than 5.0 do not implement SECCOMP_RET_USER_NOTIF.  Fall
     * back to the LD_PRELOAD guard, which still covers dynamically linked
     * commands.  If neither mechanism is available, refuse to run.
     */
    if (set_preload() != 0) {
        fprintf(stderr,
                "sipolicy-run: seccomp user notification is unavailable (%s)\n"
                "and %s/libsipolicy.so is missing; refusing to run\n",
                strerror(errno),
                sizeof(void *) == 8 ? "/system/lib64" : "/system/lib");
        close(ctl);
        if (getenv("SIPOLICY_ALLOW_FALLBACK"))
            goto run_anyway;
        return 127;
    }

    fprintf(stderr,
            "sipolicy-run: seccomp user notification unavailable, using "
            "LD_PRELOAD guard\n");
    close(ctl);
run_anyway:
    execvp(argv[1], &argv[1]);
    if (errno == EACCES)
        fprintf(stderr, "sipolicy-run: %s was denied by sipolicy\n", argv[1]);
    else
        fprintf(stderr, "sipolicy-run: cannot exec %s: %s\n",
                argv[1], strerror(errno));
    return 127;
}
