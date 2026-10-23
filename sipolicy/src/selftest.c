#define _GNU_SOURCE

#include "sipolicy.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;

static void check(const char *what, int got, int want)
{
    if (got == want) {
        printf("PASS  %s\n", what);
    } else {
        printf("FAIL  %s (want %d, got %d)\n", what, want, got);
        failures++;
    }
}

static int decide(const struct sip_ruleset *rs, int op,
                  const char *path, const char *name, int uid)
{
    return sip_rules_decide(rs, op, path, name, uid, NULL);
}

static int write_file(const char *path, const char *text)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    size_t len = strlen(text);

    if (fd < 0)
        return -1;
    if (write(fd, text, len) != (ssize_t)len) {
        close(fd);
        return -1;
    }
    close(fd);
    return 0;
}

static void test_load_order(void)
{
    char dir[] = "/tmp/sipolicy-data-XXXXXX";
    char path[512];
    struct sip_ruleset rs;
    char err[SIP_MAX_REASON];

    if (!mkdtemp(dir)) {
        check("create load-order data dir", 1, 0);
        return;
    }
    snprintf(path, sizeof(path), "%s/rules", dir);
    mkdir(path, 0700);
    snprintf(path, sizeof(path), "%s/sipolicy.rule", dir);
    write_file(path, "set default allow\n");
    snprintf(path, sizeof(path), "%s/rules/b.rule", dir);
    write_file(path, "deny cmd b\n");
    snprintf(path, sizeof(path), "%s/rules/a.rule", dir);
    write_file(path, "deny cmd a\n");

    err[0] = '\0';
    if (sip_rules_init(&rs) != 0) {
        check("init load-order ruleset", 1, 0);
        return;
    }
    check("scan data dir", sip_rules_load(&rs, dir, err, sizeof(err)), 0);
    check("scan found three files", rs.file_count >= 3, 1);
    if (rs.file_count >= 3) {
        check("central file is first",
              strstr(rs.files[0], "/sipolicy.rule") != NULL, 1);
        check("rules dir is sorted (a)",
              strstr(rs.files[1], "/a.rule") != NULL, 1);
        check("rules dir is sorted (b)",
              strstr(rs.files[2], "/b.rule") != NULL, 1);
    }
    sip_rules_free(&rs);

    snprintf(path, sizeof(path), "%s/rules/a.rule", dir);
    unlink(path);
    snprintf(path, sizeof(path), "%s/rules/b.rule", dir);
    unlink(path);
    snprintf(path, sizeof(path), "%s/rules", dir);
    rmdir(path);
    snprintf(path, sizeof(path), "%s/sipolicy.rule", dir);
    unlink(path);
    rmdir(dir);
}

int main(void)
{
    static const char rules[] =
        "# sipolicy selftest\n"
        "set default allow\n"
        "allow cmd safe\n"
        "deny cmd safe\n"
        "deny exec /system/bin/su\n"
        "deny cmd magisk\n"
        "allow exec /system/bin/sh\n"
        "deny path */frida-server*\n"
        "deny shell *rm -rf /*\n"
        "deny uid 10000-19999\n"
        "this line is invalid and is ignored\n";
    char path[] = "/tmp/sipolicy-selftest-XXXXXX";
    struct sip_ruleset rs;
    char err[SIP_MAX_REASON];
    char name[64];
    int fd;
    int rc;

    fd = mkstemp(path);
    if (fd < 0) {
        perror("mkstemp");
        return 2;
    }
    if (write(fd, rules, sizeof(rules) - 1) != (ssize_t)(sizeof(rules) - 1)) {
        perror("write");
        close(fd);
        unlink(path);
        return 2;
    }
    close(fd);

    if (sip_rules_init(&rs) != 0) {
        unlink(path);
        return 2;
    }
    err[0] = '\0';
    rc = sip_rules_load_file(&rs, path, err, sizeof(err));
    check("rule file loads", rc, 0);
    check("valid rules were parsed", (int)rs.count, 8);
    check("default is allow", rs.default_action, SIP_ACTION_ALLOW);

    check("deny exec su",
          decide(&rs, SIP_OP_EXEC, "/system/bin/su", "su", 0),
          SIP_ACTION_DENY);
    check("deny cmd magisk",
          decide(&rs, SIP_OP_EXEC, "/system/bin/magisk", "magisk", 0),
          SIP_ACTION_DENY);
    check("allow exec sh",
          decide(&rs, SIP_OP_EXEC, "/system/bin/sh", "sh", 0),
          SIP_ACTION_ALLOW);
    check("default allow ls",
          decide(&rs, SIP_OP_EXEC, "/system/bin/ls", "ls", 0),
          SIP_ACTION_ALLOW);
    check("first match wins (allow safe)",
          decide(&rs, SIP_OP_SHELL, "safe --flag", "safe", 0),
          SIP_ACTION_ALLOW);
    check("deny shell rm -rf",
          decide(&rs, SIP_OP_SHELL, "rm -rf /", "rm", 0),
          SIP_ACTION_DENY);
    check("allow shell ls",
          decide(&rs, SIP_OP_SHELL, "ls -l /", "ls", 0),
          SIP_ACTION_ALLOW);
    check("deny path frida-server",
          decide(&rs, SIP_OP_EXEC, "/data/local/tmp/frida-server-16.0",
                 "frida-server-16.0", 0),
          SIP_ACTION_DENY);
    check("deny uid range",
          decide(&rs, SIP_OP_EXEC, "/system/bin/ls", "ls", 10123),
          SIP_ACTION_DENY);
    check("allow uid 0 outside range",
          decide(&rs, SIP_OP_EXEC, "/system/bin/ls", "ls", 0),
          SIP_ACTION_ALLOW);

    sip_shell_name("FOO=bar su -c id", name, sizeof(name));
    check("shell name skips assignments", strcmp(name, "su") == 0, 1);
    sip_shell_name("/system/bin/toybox rm -rf /", name, sizeof(name));
    check("shell name uses basename", strcmp(name, "toybox") == 0, 1);

    sip_rules_free(&rs);
    unlink(path);

    test_load_order();

    if (failures) {
        printf("%d selftest(s) failed\n", failures);
        return 1;
    }
    printf("all selftests passed\n");
    return 0;
}
