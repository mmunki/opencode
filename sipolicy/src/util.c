#define _GNU_SOURCE

#include "sipolicy.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

int sip_write_full(int fd, const void *buf, size_t len)
{
    const unsigned char *p = (const unsigned char *)buf;
    size_t done = 0;

    while (done < len) {
        ssize_t n = write(fd, p + done, len - done);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
            return -1;
        done += (size_t)n;
    }
    return 0;
}

int sip_read_full(int fd, void *buf, size_t len)
{
    unsigned char *p = (unsigned char *)buf;
    size_t done = 0;

    while (done < len) {
        ssize_t n = read(fd, p + done, len - done);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
            return -1;
        done += (size_t)n;
    }
    return 0;
}

const char *sip_socket_path(void)
{
    const char *env = getenv("SIPOLICY_SOCKET");
    if (env && *env)
        return env;
    return SIP_SOCKET_PATH;
}

int sip_connect(const char *socket_path)
{
    int fd;
    struct sockaddr_un sa;
    struct timeval tv;

    if (!socket_path || !*socket_path)
        socket_path = sip_socket_path();

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    if (strlen(socket_path) >= sizeof(sa.sun_path)) {
        close(fd);
        errno = ENAMETOOLONG;
        return -1;
    }
    strcpy(sa.sun_path, socket_path);

    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }

    tv.tv_sec = 3;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    return fd;
}

const char *sip_basename(const char *path)
{
    const char *slash;

    if (!path)
        return "";
    slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

void sip_trim(char *s)
{
    char *start = s;
    char *end;

    if (!s)
        return;
    while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n')
        start++;
    if (start != s)
        memmove(s, start, strlen(start) + 1);

    end = s + strlen(s);
    while (end > s) {
        char c = end[-1];
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
            break;
        end--;
    }
    *end = '\0';
}

int sip_mkdir_p(const char *path, unsigned int mode)
{
    char tmp[SIP_MAX_PATH];
    size_t len;
    size_t i;

    if (!path || !*path)
        return -1;
    len = strlen(path);
    if (len >= sizeof(tmp)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(tmp, path, len + 1);

    for (i = 1; i < len; i++) {
        if (tmp[i] == '/') {
            tmp[i] = '\0';
            if (mkdir(tmp, (mode_t)mode) != 0 && errno != EEXIST)
                return -1;
            tmp[i] = '/';
        }
    }
    if (mkdir(tmp, (mode_t)mode) != 0 && errno != EEXIST)
        return -1;
    return 0;
}

int sip_is_own_tool(const char *path, const char *name)
{
    static const char *const tools[] = {
        "sipolicyd", "sipolicy", "sipolicy-run", NULL
    };
    int i;

    if (path && strncmp(path, SIP_DATA_DIR "/", strlen(SIP_DATA_DIR) + 1) == 0)
        return 1;
    for (i = 0; tools[i]; i++) {
        if (name && strcmp(name, tools[i]) == 0)
            return 1;
        if (path && strcmp(sip_basename(path), tools[i]) == 0)
            return 1;
    }
    return 0;
}

/*
 * First word of a shell command, skipping leading VAR=value assignments.
 * Used for "deny cmd su" against "sh -c 'su -c id'".
 */
void sip_shell_name(const char *command, char *out, size_t outlen)
{
    const char *p = command;
    const char *start;
    size_t n;

    if (!out || outlen == 0)
        return;
    out[0] = '\0';
    if (!command)
        return;

    for (;;) {
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '\0')
            return;
        start = p;
        while (*p && *p != ' ' && *p != '\t')
            p++;
        n = (size_t)(p - start);
        if (memchr(start, '=', n) && n > 0)
            continue;
        {
            char token[512];
            size_t copy = n < sizeof(token) - 1 ? n : sizeof(token) - 1;
            memcpy(token, start, copy);
            token[copy] = '\0';
            snprintf(out, outlen, "%s", sip_basename(token));
        }
        return;
    }
}

int sip_call(const char *socket_path, struct sip_msg *msg,
             const char *payload, size_t payload_len,
             struct sip_reply *reply, char *reason, size_t reason_len,
             char *data, size_t data_len)
{
    int fd;
    int rc = -1;
    struct sip_reply r;

    if (!msg || !reply)
        return -1;
    if (reason && reason_len)
        reason[0] = '\0';
    if (data && data_len)
        data[0] = '\0';

    fd = sip_connect(socket_path);
    if (fd < 0)
        return -1;

    msg->magic = SIP_MAGIC;
    if (sip_write_full(fd, msg, sizeof(*msg)) != 0)
        goto out;
    if (payload_len && sip_write_full(fd, payload, payload_len) != 0)
        goto out;

    if (sip_read_full(fd, &r, sizeof(r)) != 0)
        goto out;
    if (r.magic != SIP_MAGIC || r.reason_len > SIP_MAX_REASON ||
        r.data_len > SIP_MAX_PAYLOAD)
        goto out;

    if (r.reason_len) {
        char tmp[SIP_MAX_REASON + 1];
        if (sip_read_full(fd, tmp, r.reason_len) != 0)
            goto out;
        tmp[r.reason_len] = '\0';
        if (reason && reason_len)
            snprintf(reason, reason_len, "%s", tmp);
    }
    if (r.data_len) {
        char tmp[SIP_MAX_PAYLOAD + 1];
        if (sip_read_full(fd, tmp, r.data_len) != 0)
            goto out;
        tmp[r.data_len] = '\0';
        if (data && data_len)
            snprintf(data, data_len, "%s", tmp);
    }

    *reply = r;
    rc = 0;
out:
    close(fd);
    return rc;
}
