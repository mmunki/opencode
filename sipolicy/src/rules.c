#define _GNU_SOURCE

#include "sipolicy.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fnmatch.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static void set_error(char *err, size_t errlen, const char *fmt, ...)
{
    va_list ap;

    if (!err || errlen == 0)
        return;
    va_start(ap, fmt);
    vsnprintf(err, errlen, fmt, ap);
    va_end(ap);
}

int sip_rules_init(struct sip_ruleset *rs)
{
    if (!rs)
        return -1;
    memset(rs, 0, sizeof(*rs));
    rs->default_action = SIP_ACTION_ALLOW;
    return 0;
}

void sip_rules_free(struct sip_ruleset *rs)
{
    size_t i;

    if (!rs)
        return;
    for (i = 0; i < rs->count; i++) {
        free(rs->rules[i].pattern);
        free(rs->rules[i].source);
    }
    free(rs->rules);
    for (i = 0; i < rs->file_count; i++)
        free(rs->files[i]);
    free(rs->files);
    memset(rs, 0, sizeof(*rs));
}

static int rules_push(struct sip_ruleset *rs, const struct sip_rule *rule)
{
    struct sip_rule *next;

    if (rs->count >= SIP_MAX_RULES)
        return -1;
    if (rs->count == rs->cap) {
        size_t cap = rs->cap ? rs->cap * 2 : 32;
        next = realloc(rs->rules, cap * sizeof(*next));
        if (!next)
            return -1;
        rs->rules = next;
        rs->cap = cap;
    }
    rs->rules[rs->count++] = *rule;
    return 0;
}

static int files_push(struct sip_ruleset *rs, const char *path)
{
    char **next;
    size_t i;

    for (i = 0; i < rs->file_count; i++) {
        if (strcmp(rs->files[i], path) == 0)
            return 0;
    }
    if (rs->file_count == rs->file_cap) {
        size_t cap = rs->file_cap ? rs->file_cap * 2 : 16;
        next = realloc(rs->files, cap * sizeof(*next));
        if (!next)
            return -1;
        rs->files = next;
        rs->file_cap = cap;
    }
    rs->files[rs->file_count] = strdup(path);
    if (!rs->files[rs->file_count])
        return -1;
    rs->file_count++;
    return 0;
}

static int parse_uid_range(const char *text, long *lo, long *hi)
{
    char *end;
    long a;
    long b;

    errno = 0;
    a = strtol(text, &end, 10);
    if (errno || end == text)
        return -1;
    while (*end == ' ')
        end++;
    if (*end == '\0') {
        *lo = a;
        *hi = a;
        return 0;
    }
    if (*end == '-') {
        const char *p = end + 1;
        while (*p == ' ')
            p++;
        errno = 0;
        b = strtol(p, &end, 10);
        if (errno || end == p || *end != '\0')
            return -1;
        if (b < a)
            return -1;
        *lo = a;
        *hi = b;
        return 0;
    }
    if (*end == '+') {
        end++;
        while (*end == ' ')
            end++;
        if (*end != '\0')
            return -1;
        *lo = a;
        *hi = 2147483647L;
        return 0;
    }
    return -1;
}

static int parse_line(struct sip_ruleset *rs, const char *file, int line_no,
                      char *line, char *err, size_t errlen)
{
    char *comment;
    char *action;
    char *type;
    char *rest;
    char *save = NULL;
    struct sip_rule rule;

    comment = strchr(line, '#');
    if (comment)
        *comment = '\0';
    sip_trim(line);
    if (line[0] == '\0')
        return 0;

    memset(&rule, 0, sizeof(rule));
    rule.uid_lo = -1;
    rule.uid_hi = -1;

    action = strtok_r(line, " \t", &save);
    if (!action)
        return 0;

    if (strcmp(action, "set") == 0) {
        action = strtok_r(NULL, " \t", &save);
        if (!action)
            goto bad;
    }

    if (strcmp(action, "default") == 0) {
        type = strtok_r(NULL, " \t", &save);
        if (!type)
            goto bad;
        if (strcmp(type, "allow") == 0)
            rs->default_action = SIP_ACTION_ALLOW;
        else if (strcmp(type, "deny") == 0)
            rs->default_action = SIP_ACTION_DENY;
        else
            goto bad;
        return 0;
    }

    if (strcmp(action, "allow") == 0)
        rule.action = SIP_ACTION_ALLOW;
    else if (strcmp(action, "deny") == 0)
        rule.action = SIP_ACTION_DENY;
    else
        goto bad;

    type = strtok_r(NULL, " \t", &save);
    if (!type)
        goto bad;

    if (strcmp(type, "all") == 0) {
        rule.type = SIP_RULE_ALL;
        rule.pattern = strdup("*");
        if (!rule.pattern)
            return -1;
    } else if (strcmp(type, "exec") == 0) {
        rule.type = SIP_RULE_EXEC;
    } else if (strcmp(type, "cmd") == 0) {
        rule.type = SIP_RULE_CMD;
    } else if (strcmp(type, "path") == 0) {
        rule.type = SIP_RULE_PATH;
    } else if (strcmp(type, "shell") == 0) {
        rule.type = SIP_RULE_SHELL;
    } else if (strcmp(type, "uid") == 0) {
        rule.type = SIP_RULE_UID;
    } else {
        goto bad;
    }

    if (rule.type != SIP_RULE_ALL) {
        rest = save;
        while (rest && (*rest == ' ' || *rest == '\t'))
            rest++;
        if (!rest || *rest == '\0')
            goto bad;
        sip_trim(rest);

        if (rule.type == SIP_RULE_UID) {
            if (parse_uid_range(rest, &rule.uid_lo, &rule.uid_hi) != 0)
                goto bad;
            rule.pattern = strdup(rest);
        } else {
            rule.pattern = strdup(rest);
        }
        if (!rule.pattern)
            return -1;
    }

    rule.source = strdup(file);
    if (!rule.source) {
        free(rule.pattern);
        return -1;
    }
    rule.line = line_no;

    if (rules_push(rs, &rule) != 0) {
        free(rule.pattern);
        free(rule.source);
        return -1;
    }
    return 0;

bad:
    set_error(err, errlen, "%s:%d: invalid rule", file, line_no);
    return 0;
}

int sip_rules_load_file(struct sip_ruleset *rs, const char *path,
                        char *err, size_t errlen)
{
    FILE *fp;
    char line[4096];
    int line_no = 0;
    int errors = 0;

    if (!rs || !path)
        return -1;

    fp = fopen(path, "r");
    if (!fp)
        return -1;

    while (fgets(line, sizeof(line), fp)) {
        char line_err[SIP_MAX_REASON];
        line_err[0] = '\0';
        line_no++;
        if (parse_line(rs, path, line_no, line, line_err, sizeof(line_err)) != 0) {
            fclose(fp);
            return -1;
        }
        if (line_err[0]) {
            if (errors == 0 && err && errlen)
                set_error(err, errlen, "%s", line_err);
            errors++;
        }
    }
    fclose(fp);

    if (files_push(rs, path) != 0)
        return -1;
    (void)errors;
    return 0;
}

static int cmp_str(const void *a, const void *b)
{
    const char *const *sa = (const char *const *)a;
    const char *const *sb = (const char *const *)b;
    return strcmp(*sa, *sb);
}

static int add_regular(struct sip_ruleset *rs, const char *path)
{
    struct stat st;

    if (lstat(path, &st) != 0 || !S_ISREG(st.st_mode))
        return 0;
    return files_push(rs, path);
}

static int add_rule_dir(struct sip_ruleset *rs, const char *dir)
{
    DIR *d;
    struct dirent *de;

    d = opendir(dir);
    if (!d)
        return 0;
    while ((de = readdir(d)) != NULL) {
        char path[SIP_MAX_PATH];
        size_t len;

        if (de->d_name[0] == '.')
            continue;
        len = strlen(de->d_name);
        if (len < 5 || strcmp(de->d_name + len - 5, ".rule") != 0)
            continue;
        {
            size_t dlen = strlen(dir);
            if (dlen + 1 + len + 1 > sizeof(path))
                continue;
            memcpy(path, dir, dlen);
            path[dlen] = '/';
            memcpy(path + dlen + 1, de->d_name, len + 1);
        }
        if (add_regular(rs, path) != 0) {
            closedir(d);
            return -1;
        }
    }
    closedir(d);
    return 0;
}

static int add_module_rules(struct sip_ruleset *rs, const char *modules_dir)
{
    DIR *d;
    struct dirent *de;

    d = opendir(modules_dir);
    if (!d)
        return 0;
    while ((de = readdir(d)) != NULL) {
        char path[SIP_MAX_PATH];

        {
            size_t dlen;
            size_t nlen;
            static const char suffix[] = "/sipolicy.rule";
            if (de->d_name[0] == '.')
                continue;
            dlen = strlen(modules_dir);
            nlen = strlen(de->d_name);
            if (dlen + 1 + nlen + sizeof(suffix) > sizeof(path))
                continue;
            memcpy(path, modules_dir, dlen);
            path[dlen] = '/';
            memcpy(path + dlen + 1, de->d_name, nlen);
            memcpy(path + dlen + 1 + nlen, suffix, sizeof(suffix));
        }
        if (add_regular(rs, path) != 0) {
            closedir(d);
            return -1;
        }
    }
    closedir(d);
    return 0;
}

int sip_rules_load(struct sip_ruleset *rs, const char *data_dir,
                   char *err, size_t errlen)
{
    char path[SIP_MAX_PATH];
    char rules_dir[SIP_MAX_PATH];
    struct sip_ruleset next;
    size_t i;
    size_t group_start;
    int loaded = 0;

    if (!rs)
        return -1;
    if (!data_dir)
        data_dir = SIP_DATA_DIR;

    if (sip_rules_init(&next) != 0)
        return -1;

    /*
     * Load order: central file, then the rules directory, then one
     * sipolicy.rule per Magisk module.  Each group is sorted on its own so
     * the central file always has the first and the module fragments the
     * last word.
     */
    snprintf(path, sizeof(path), "%s/sipolicy.rule", data_dir);
    if (add_regular(&next, path) != 0)
        goto fail;

    group_start = next.file_count;
    snprintf(rules_dir, sizeof(rules_dir), "%s/rules", data_dir);
    if (add_rule_dir(&next, rules_dir) != 0)
        goto fail;
    if (next.file_count - group_start > 1)
        qsort(next.files + group_start, next.file_count - group_start,
              sizeof(char *), cmp_str);

    group_start = next.file_count;
    if (add_module_rules(&next, "/data/adb/modules") != 0)
        goto fail;
    if (next.file_count - group_start > 1)
        qsort(next.files + group_start, next.file_count - group_start,
              sizeof(char *), cmp_str);

    for (i = 0; i < next.file_count; i++) {
        if (sip_rules_load_file(&next, next.files[i], err, errlen) != 0)
            goto fail;
        loaded++;
    }

    sip_rules_free(rs);
    *rs = next;
    if (err && errlen && loaded == 0)
        set_error(err, errlen, "no rule files loaded; default allow");
    return 0;

fail:
    sip_rules_free(&next);
    set_error(err, errlen, "failed to load rules");
    return -1;
}

int sip_rules_decide(const struct sip_ruleset *rs, int op,
                     const char *path, const char *name, int uid,
                     const struct sip_rule **matched)
{
    size_t i;

    if (matched)
        *matched = NULL;
    if (!rs)
        return SIP_ACTION_ALLOW;
    if (!path)
        path = "";
    if (!name)
        name = sip_basename(path);

    for (i = 0; i < rs->count; i++) {
        const struct sip_rule *r = &rs->rules[i];
        int hit = 0;

        switch (r->type) {
        case SIP_RULE_ALL:
            hit = 1;
            break;
        case SIP_RULE_EXEC:
            hit = (op == SIP_OP_EXEC &&
                   fnmatch(r->pattern, path, 0) == 0);
            break;
        case SIP_RULE_CMD:
            hit = ((op == SIP_OP_EXEC || op == SIP_OP_SHELL) &&
                   fnmatch(r->pattern, name, 0) == 0);
            break;
        case SIP_RULE_PATH:
            hit = (fnmatch(r->pattern, path, 0) == 0);
            break;
        case SIP_RULE_SHELL:
            hit = (op == SIP_OP_SHELL &&
                   fnmatch(r->pattern, path, 0) == 0);
            break;
        case SIP_RULE_UID:
            hit = ((long)uid >= r->uid_lo && (long)uid <= r->uid_hi);
            break;
        default:
            break;
        }
        if (hit) {
            if (matched)
                *matched = r;
            return r->action;
        }
    }
    return rs->default_action;
}
