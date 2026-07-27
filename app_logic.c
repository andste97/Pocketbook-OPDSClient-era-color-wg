#include "opds_app.h"
#include <ctype.h>
#include <string.h>

void SecureZero(void *ptr, size_t len) {
    volatile unsigned char *p = (volatile unsigned char *)ptr;
    while (len-- > 0) {
        *p++ = 0;
    }
}

static void CopyBounded(char *destination, size_t destination_size, const char *source) {
    size_t length;
    if (!destination || destination_size == 0 || !source) return;
    length = strlen(source);
    if (length >= destination_size) length = destination_size - 1;
    memmove(destination, source, length);
    destination[length] = '\0';
}

void NormalizeServerSettings(OPDSServer *server, int auth_mode_present) {
    if (!server) return;

    server->name[MAX_STR_LEN - 1] = '\0';
    server->url[MAX_STR_LEN - 1] = '\0';
    server->user[MAX_STR_LEN - 1] = '\0';
    server->pass[MAX_STR_LEN - 1] = '\0';
    server->auth_url[MAX_STR_LEN - 1] = '\0';

    if (server->catalog_rows < 4) server->catalog_rows = 4;
    if (server->catalog_rows > 10) server->catalog_rows = 10;
    server->fetch_thumbs = server->fetch_thumbs ? 1 : 0;

    if (!auth_mode_present) {
        server->auth_mode = server->user[0] ? AUTH_MODE_BASIC : AUTH_MODE_NONE;
    } else if (server->auth_mode < AUTH_MODE_NONE || server->auth_mode > AUTH_MODE_AUTHELIA_COOKIE) {
        server->auth_mode = AUTH_MODE_NONE;
    }
}

AuthMode NextAuthMode(AuthMode mode) {
    switch (mode) {
        case AUTH_MODE_NONE: return AUTH_MODE_BASIC;
        case AUTH_MODE_BASIC: return AUTH_MODE_AUTHELIA_COOKIE;
        default: return AUTH_MODE_NONE;
    }
}

const char *AuthModeLabel(AuthMode mode) {
    switch (mode) {
        case AUTH_MODE_BASIC: return "HTTP Basic";
        case AUTH_MODE_AUTHELIA_COOKIE: return "Authelia Cookie";
        default: return "None";
    }
}

void AuthFlowStart(AuthFlowContext *flow) {
    if (!flow) return;
    SecureZero(flow, sizeof(*flow));
    flow->state = AUTH_FLOW_USERNAME;
}

int AuthFlowSetUsername(AuthFlowContext *flow, const char *username) {
    if (!flow || flow->state != AUTH_FLOW_USERNAME || !username || !username[0]) return -1;
    if (username != flow->username) {
        CopyBounded(flow->username, sizeof(flow->username), username);
    }
    flow->state = AUTH_FLOW_PASSWORD;
    return 0;
}

int AuthFlowSetPassword(AuthFlowContext *flow, const char *password) {
    if (!flow || flow->state != AUTH_FLOW_PASSWORD || !password || !password[0]) return -1;
    if (password != flow->password) {
        CopyBounded(flow->password, sizeof(flow->password), password);
    }
    flow->state = AUTH_FLOW_TOTP;
    return 0;
}

int AuthFlowSetTOTP(AuthFlowContext *flow, const char *totp) {
    size_t len;
    if (!flow || flow->state != AUTH_FLOW_TOTP || !totp) return -1;

    len = strlen(totp);
    if (len != 6 && len != 8) return -1;
    for (size_t i = 0; i < len; i++) {
        if (!isdigit((unsigned char)totp[i])) return -1;
    }

    if (totp != flow->totp) {
        CopyBounded(flow->totp, sizeof(flow->totp), totp);
    }
    flow->state = AUTH_FLOW_IDLE;
    return 0;
}

void AuthFlowCancel(AuthFlowContext *flow) {
    if (!flow) return;
    SecureZero(flow, sizeof(*flow));
    flow->state = AUTH_FLOW_IDLE;
}

void AuthPendingClear(AuthPendingAction *pending) {
    if (!pending) return;
    SecureZero(pending, sizeof(*pending));
    pending->type = AUTH_PENDING_NONE;
    pending->format_index = -1;
}

void AuthPendingSetCatalog(AuthPendingAction *pending, const char *url) {
    if (!pending || !url || !url[0]) return;
    AuthPendingClear(pending);
    pending->type = AUTH_PENDING_CATALOG;
    CopyBounded(pending->url, sizeof(pending->url), url);
}

void AuthPendingSetBook(AuthPendingAction *pending, int format_index) {
    if (!pending || format_index < 0 || format_index >= MAX_FORMATS) return;
    AuthPendingClear(pending);
    pending->type = AUTH_PENDING_BOOK;
    pending->format_index = format_index;
}

void AuthPendingSetSearch(AuthPendingAction *pending, const char *query) {
    if (!pending || !query || !query[0]) return;
    AuthPendingClear(pending);
    pending->type = AUTH_PENDING_SEARCH;
    CopyBounded(pending->url, sizeof(pending->url), query);
}
