/*
 * sipolicy - kernel-assisted command policy for Android
 *
 * Shared protocol and constants used by the daemon, the CLI, the seccomp
 * launcher and the LD_PRELOAD guard.
 */
#ifndef SIPOLICY_H
#define SIPOLICY_H

#include <stddef.h>
#include <stdint.h>

#define SIPOLICY_VERSION      "0.109"
#define SIPOLICY_VERSION_CODE 109

#define SIP_DATA_DIR    "/data/adb/sipolicy"
#define SIP_SOCKET_PATH SIP_DATA_DIR "/sipolicy.sock"
#define SIP_PID_PATH    SIP_DATA_DIR "/sipolicyd.pid"
#define SIP_LOG_PATH    SIP_DATA_DIR "/sipolicy.log"
#define SIP_STATE_PATH  SIP_DATA_DIR "/state"

#define SIP_MAGIC 0x5349504cU /* "SIPL" */

#define SIP_MAX_PATH    4096
#define SIP_MAX_CMD     8192
#define SIP_MAX_NAME    512
#define SIP_MAX_REASON  512
#define SIP_MAX_PAYLOAD 65536
#define SIP_MAX_RULES   4096

enum sip_msg_type {
    SIP_MSG_CHECK     = 1,
    SIP_MSG_STATUS    = 2,
    SIP_MSG_RELOAD    = 3,
    SIP_MSG_RULES     = 4,
    SIP_MSG_SUPERVISE = 5,
    SIP_MSG_PING      = 6
};

enum sip_op {
    SIP_OP_EXEC  = 1,
    SIP_OP_SHELL = 2
};

enum sip_action {
    SIP_ACTION_DEFAULT = 0,
    SIP_ACTION_ALLOW   = 1,
    SIP_ACTION_DENY    = 2
};

struct sip_msg {
    uint32_t magic;
    uint32_t type;
    uint32_t op;
    int32_t  pid;
    int32_t  uid;
    uint32_t path_len;
    uint32_t name_len;
    uint32_t argc;
    uint32_t reserved;
};

struct sip_reply {
    uint32_t magic;
    int32_t  allowed;
    int32_t  error;
    uint32_t reason_len;
    uint32_t data_len;
    uint32_t reserved;
};

_Static_assert(sizeof(struct sip_msg) == 36, "sip_msg size");
_Static_assert(sizeof(struct sip_reply) == 24, "sip_reply size");

/* util.c */
int  sip_write_full(int fd, const void *buf, size_t len);
int  sip_read_full(int fd, void *buf, size_t len);
int  sip_connect(const char *socket_path);
const char *sip_socket_path(void);
const char *sip_basename(const char *path);
void sip_trim(char *s);
int  sip_mkdir_p(const char *path, unsigned int mode);
int  sip_is_own_tool(const char *path, const char *name);
void sip_shell_name(const char *command, char *out, size_t outlen);
int  sip_call(const char *socket_path, struct sip_msg *msg,
              const char *payload, size_t payload_len,
              struct sip_reply *reply, char *reason, size_t reason_len,
              char *data, size_t data_len);

/* rules.c */
enum sip_rule_type {
    SIP_RULE_ALL   = 1,
    SIP_RULE_EXEC  = 2,
    SIP_RULE_CMD   = 3,
    SIP_RULE_PATH  = 4,
    SIP_RULE_SHELL = 5,
    SIP_RULE_UID   = 6
};

struct sip_rule {
    int  action;
    int  type;
    char *pattern;
    long uid_lo;
    long uid_hi;
    char *source;
    int  line;
};

struct sip_ruleset {
    int  default_action;
    struct sip_rule *rules;
    size_t count;
    size_t cap;
    char **files;
    size_t file_count;
    size_t file_cap;
};

int  sip_rules_init(struct sip_ruleset *rs);
void sip_rules_free(struct sip_ruleset *rs);
int  sip_rules_load_file(struct sip_ruleset *rs, const char *path,
                         char *err, size_t errlen);
int  sip_rules_load(struct sip_ruleset *rs, const char *data_dir,
                    char *err, size_t errlen);
int  sip_rules_decide(const struct sip_ruleset *rs, int op,
                      const char *path, const char *name, int uid,
                      const struct sip_rule **matched);

#endif
