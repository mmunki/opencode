#define _GNU_SOURCE

#include "sipolicy.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int simple_call(uint32_t type, int op, const char *path,
                       const char *name, int print_data)
{
    struct sip_msg msg;
    struct sip_reply reply;
    char reason[SIP_MAX_REASON];
    char data[SIP_MAX_PAYLOAD];
    size_t plen = 0;
    char payload[SIP_MAX_PATH + SIP_MAX_NAME];
    const char *sock = sip_socket_path();

    memset(&msg, 0, sizeof(msg));
    memset(&reply, 0, sizeof(reply));
    msg.type = type;
    msg.op = (uint32_t)op;
    msg.pid = (int)getpid();
    msg.uid = (int)getuid();

    if (path) {
        msg.path_len = (uint32_t)strlen(path) + 1;
        if (msg.path_len > SIP_MAX_PATH) {
            fprintf(stderr, "sipolicy: path too long\n");
            return 2;
        }
        memcpy(payload + plen, path, msg.path_len);
        plen += msg.path_len;
    }
    if (name) {
        msg.name_len = (uint32_t)strlen(name) + 1;
        if (msg.name_len > SIP_MAX_NAME) {
            fprintf(stderr, "sipolicy: name too long\n");
            return 2;
        }
        memcpy(payload + plen, name, msg.name_len);
        plen += msg.name_len;
    }

    if (sip_call(sock, &msg, payload, plen, &reply, reason, sizeof(reason),
                 data, sizeof(data)) != 0) {
        fprintf(stderr, "sipolicy: cannot reach sipolicyd at %s: %s\n",
                sock, strerror(errno));
        return 2;
    }

    if (type == SIP_MSG_CHECK) {
        printf("%s", reply.allowed ? "ALLOW" : "DENY");
        if (reason[0])
            printf(" (%s)", reason);
        printf("\n");
        return reply.allowed ? 0 : 1;
    }

    if (reason[0])
        printf("%s\n", reason);
    if (print_data && data[0])
        printf("%s", data);
    return reply.allowed ? 0 : 1;
}

static int cmd_run(int argc, char **argv)
{
    char self[PATH_MAX];
    char runner[PATH_MAX];
    char **run_argv;
    ssize_t n;
    char *slash;
    int i;

    if (argc < 1) {
        fprintf(stderr, "usage: sipolicy run <command> [args...]\n");
        return 2;
    }

    run_argv = calloc((size_t)argc + 2, sizeof(*run_argv));
    if (!run_argv)
        return 127;
    run_argv[0] = (char *)"sipolicy-run";
    for (i = 0; i < argc; i++)
        run_argv[i + 1] = argv[i];

    n = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (n > 0) {
        self[n] = '\0';
        slash = strrchr(self, '/');
        if (slash) {
            size_t dlen;
            static const char name[] = "/sipolicy-run";
            *slash = '\0';
            dlen = strlen(self);
            if (dlen + sizeof(name) <= sizeof(runner)) {
                memcpy(runner, self, dlen);
                memcpy(runner + dlen, name, sizeof(name));
                if (access(runner, X_OK) == 0)
                    execv(runner, run_argv);
            }
        }
    }

    execvp("sipolicy-run", run_argv);
    fprintf(stderr, "sipolicy: cannot execute sipolicy-run: %s\n",
            strerror(errno));
    return 127;
}

static int cmd_log(int argc, char **argv)
{
    char path[SIP_MAX_PATH];
    FILE *fp;
    char **lines = NULL;
    size_t count = 0;
    size_t cap = 0;
    size_t keep = 50;
    char buf[4096];
    size_t i;

    if (argc >= 2 && strcmp(argv[0], "-n") == 0)
        keep = (size_t)strtoul(argv[1], NULL, 10);
    else if (argc >= 1)
        keep = (size_t)strtoul(argv[0], NULL, 10);
    if (keep == 0)
        keep = 50;
    if (keep > 10000)
        keep = 10000;

    snprintf(path, sizeof(path), "%s/sipolicy.log", SIP_DATA_DIR);
    fp = fopen(path, "r");
    if (!fp) {
        fprintf(stderr, "sipolicy: cannot open %s: %s\n", path, strerror(errno));
        return 1;
    }

    while (fgets(buf, sizeof(buf), fp)) {
        char *copy;
        if (count == cap) {
            size_t ncap = cap ? cap * 2 : 64;
            char **next = realloc(lines, ncap * sizeof(*next));
            if (!next)
                break;
            lines = next;
            cap = ncap;
        }
        copy = strdup(buf);
        if (!copy)
            break;
        if (count == keep) {
            free(lines[0]);
            memmove(lines, lines + 1, (keep - 1) * sizeof(*lines));
            count--;
        }
        lines[count++] = copy;
    }
    fclose(fp);

    for (i = 0; i < count; i++) {
        fputs(lines[i], stdout);
        free(lines[i]);
    }
    free(lines);
    return 0;
}

static void usage(void)
{
    fprintf(stderr,
            "sipolicy %s - kernel-assisted command policy\n"
            "\n"
            "usage:\n"
            "  sipolicy status              show daemon state\n"
            "  sipolicy reload              reload all sipolicy.rule files\n"
            "  sipolicy rules               list loaded rule files\n"
            "  sipolicy check <path>        ask the daemon about one executable\n"
            "  sipolicy run <cmd> [args]    run a command under seccomp supervision\n"
            "  sipolicy log [-n N]          show the last N log lines (default 50)\n"
            "  sipolicy version             print the version\n",
            SIPOLICY_VERSION);
}

int main(int argc, char **argv)
{
    const char *cmd;

    if (argc < 2) {
        usage();
        return 2;
    }
    cmd = argv[1];
    argc -= 2;
    argv += 2;

    if (strcmp(cmd, "version") == 0 || strcmp(cmd, "--version") == 0) {
        printf("sipolicy %s\n", SIPOLICY_VERSION);
        return 0;
    }
    if (strcmp(cmd, "status") == 0)
        return simple_call(SIP_MSG_STATUS, 0, NULL, NULL, 1);
    if (strcmp(cmd, "reload") == 0)
        return simple_call(SIP_MSG_RELOAD, 0, NULL, NULL, 0);
    if (strcmp(cmd, "ping") == 0)
        return simple_call(SIP_MSG_PING, 0, NULL, NULL, 1);
    if (strcmp(cmd, "rules") == 0)
        return simple_call(SIP_MSG_RULES, 0, NULL, NULL, 1);
    if (strcmp(cmd, "check") == 0) {
        if (argc < 1) {
            fprintf(stderr, "usage: sipolicy check <path>\n");
            return 2;
        }
        return simple_call(SIP_MSG_CHECK, SIP_OP_EXEC, argv[0],
                           sip_basename(argv[0]), 0);
    }
    if (strcmp(cmd, "run") == 0)
        return cmd_run(argc, argv);
    if (strcmp(cmd, "log") == 0)
        return cmd_log(argc, argv);
    if (strcmp(cmd, "help") == 0 || strcmp(cmd, "--help") == 0 ||
        strcmp(cmd, "-h") == 0) {
        usage();
        return 0;
    }

    fprintf(stderr, "sipolicy: unknown command: %s\n", cmd);
    usage();
    return 2;
}
