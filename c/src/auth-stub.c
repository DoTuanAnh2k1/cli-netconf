/*
 * auth-stub.c — No-op stubs cho auth functions khi build direct mode.
 *
 * Direct mode không có mgt-service nên backup/history đơn giản bị bỏ qua.
 * Link file này thay cho auth.c khi build cli-netconf-c-direct.
 */
#include <stdbool.h>
#include "cli.h"

void auth_save_history(cli_session_t *s, const char *cmd, long ms) {
    (void)s; (void)cmd; (void)ms;
    /* no-op: không có mgt-service trong direct mode */
}

bool auth_save_backup(cli_session_t *s, const char *config_xml, int *out_remote_id) {
    (void)s; (void)config_xml;
    if (out_remote_id) *out_remote_id = -1;
    return false;
}

bool auth_list_backups(cli_session_t *s) {
    (void)s;
    return false;
}

bool auth_get_backup(cli_session_t *s, int remote_id, char **out_xml) {
    (void)s; (void)remote_id; (void)out_xml;
    return false;
}

/* auth_login / auth_list_ne không dùng trong direct mode nhưng cần để link */
bool auth_login(cli_session_t *s, const char *username, const char *password) {
    (void)s; (void)username; (void)password;
    return false;
}

bool auth_list_ne(cli_session_t *s) {
    (void)s;
    return false;
}
