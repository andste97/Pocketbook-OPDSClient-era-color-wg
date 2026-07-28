#include "opds_app.h"
#include <curl/curl.h>
#include <json-c/json.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define AUTH_RESPONSE_LIMIT 65536
#define DEVICE_CA_BUNDLE "/ebrmain/share/ssl/certs/ca-certificates.crt"

typedef struct {
    char *data;
    size_t size;
    int overflow;
} AuthResponse;

static char cookie_directory[MAX_STR_LEN] = AUTH_DIR;

static int GetStagingCookieJarPath(int server_index, char *out, size_t out_size) {
    int written;
    if (server_index < 0 || server_index >= MAX_SERVERS || !out || out_size == 0) return -1;
    written = snprintf(out, out_size, "%sserver_%d.login.cookies", cookie_directory, server_index);
    return written > 0 && (size_t)written < out_size ? 0 : -1;
}

static int GetShiftCookieJarPath(int server_index, char *out, size_t out_size) {
    int written;
    if (server_index < 0 || server_index >= MAX_SERVERS || !out || out_size == 0) return -1;
    written = snprintf(out, out_size, "%sserver_%d.cookies.shift", cookie_directory, server_index);
    return written > 0 && (size_t)written < out_size ? 0 : -1;
}

static int CookieJarIsUsable(const char *path) {
    struct stat info;
    return path && stat(path, &info) == 0 && info.st_size > 0 && access(path, R_OK | W_OK) == 0;
}

static size_t AuthWriteCallback(void *contents, size_t size, size_t nmemb, void *userdata) {
    AuthResponse *response = (AuthResponse *)userdata;
    size_t bytes = size * nmemb;

    if (bytes > AUTH_RESPONSE_LIMIT - response->size) {
        response->overflow = 1;
        return 0;
    }

    char *updated = realloc(response->data, response->size + bytes + 1);
    if (!updated) return 0;

    response->data = updated;
    memcpy(response->data + response->size, contents, bytes);
    response->size += bytes;
    response->data[response->size] = '\0';
    return bytes;
}

static size_t DiscardWriteCallback(void *contents, size_t size, size_t nmemb, void *userdata) {
    (void)contents;
    (void)userdata;
    return size * nmemb;
}

static int IsTLSError(CURLcode code) {
    return code == CURLE_PEER_FAILED_VERIFICATION ||
           code == CURLE_SSL_CONNECT_ERROR ||
           code == CURLE_SSL_CERTPROBLEM ||
           code == CURLE_SSL_CIPHER ||
           code == CURLE_SSL_CACERT_BADFILE;
}

static int IsHTTPSURL(const char *url) {
#ifdef UNIT_TEST
    if (url && strncmp(url, "http://127.0.0.1", 16) == 0) return 1;
#endif
    return url && strncmp(url, "https://", 8) == 0;
}

static int BuildAuthURL(const OPDSServer *server, const char *path, char *out, size_t out_size) {
    size_t base_len;
    int written;

    if (!server || !path || !out || out_size == 0 || !IsHTTPSURL(server->auth_url)) return -1;

    base_len = strlen(server->auth_url);
    while (base_len > 0 && server->auth_url[base_len - 1] == '/') base_len--;

    written = snprintf(out, out_size, "%.*s%s", (int)base_len, server->auth_url, path);
    return written > 0 && (size_t)written < out_size ? 0 : -1;
}

static AutheliaResult ClassifyTransportError(CURLcode code) {
    if (IsTLSError(code)) return AUTHELIA_TLS_ERROR;
    return AUTHELIA_HTTP_ERROR;
}

static AutheliaResult ParseOKResponse(const AuthResponse *response) {
    json_object *root;
    json_object *status;
    const char *status_text;

    if (!response || response->overflow || !response->data) return AUTHELIA_INVALID_RESPONSE;

    root = json_tokener_parse(response->data);
    if (!root) return AUTHELIA_INVALID_RESPONSE;

    if (!json_object_object_get_ex(root, "status", &status) ||
        json_object_get_type(status) != json_type_string) {
        json_object_put(root);
        return AUTHELIA_INVALID_RESPONSE;
    }

    status_text = json_object_get_string(status);
    AutheliaResult result = strcmp(status_text, "OK") == 0 ? AUTHELIA_OK : AUTHELIA_INVALID_RESPONSE;
    json_object_put(root);
    return result;
}

static AutheliaResult PerformAuthRequest(const OPDSServer *server, int server_index,
                                         const char *path, const char *method,
                                         const char *json_body, const char *cookie_path,
                                         long *response_code,
                                         AuthResponse *response) {
    CURL *curl = NULL;
    struct curl_slist *headers = NULL;
    CURLcode code;
    char url[MAX_STR_LEN * 2];

    if (!server || !path || !method || !cookie_path || !cookie_path[0] ||
        !response_code || !response) {
        LogMessage(LOG_LEVEL_ERROR, "Invalid Authelia request configuration");
        return AUTHELIA_CONFIG_ERROR;
    }
    if (BuildAuthURL(server, path, url, sizeof(url)) != 0) return AUTHELIA_CONFIG_ERROR;
    LogMessage(LOG_LEVEL_INFO, "Authelia request started: server=%d method=%s path=%s",
               server_index, method, path);

    if (mkdir(cookie_directory, 0700) != 0 && errno != EEXIST) {
        LogMessage(LOG_LEVEL_ERROR, "Unable to create cookie directory: errno=%d", errno);
        return AUTHELIA_COOKIE_ERROR;
    }

    response->data = malloc(1);
    if (!response->data) return AUTHELIA_HTTP_ERROR;
    response->data[0] = '\0';
    response->size = 0;
    response->overflow = 0;

    curl = curl_easy_init();
    if (!curl) {
        free(response->data);
        response->data = NULL;
        return AUTHELIA_HTTP_ERROR;
    }

    headers = curl_slist_append(headers, "Accept: application/json");
    if (json_body) headers = curl_slist_append(headers, "Content-Type: application/json");
    if (!headers) {
        curl_easy_cleanup(curl);
        free(response->data);
        response->data = NULL;
        return AUTHELIA_HTTP_ERROR;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, APP_USER_AGENT);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    if (access(DEVICE_CA_BUNDLE, R_OK) == 0) {
        curl_easy_setopt(curl, CURLOPT_CAINFO, DEVICE_CA_BUNDLE);
    }
    curl_easy_setopt(curl, CURLOPT_COOKIEFILE, cookie_path);
    curl_easy_setopt(curl, CURLOPT_COOKIEJAR, cookie_path);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, AuthWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, response);

    if (strcmp(method, "POST") == 0) {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body ? json_body : "{}");
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(json_body ? json_body : "{}"));
    } else if (strcmp(method, "GET") != 0) {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
        if (json_body) {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body);
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(json_body));
        }
    }

    code = curl_easy_perform(curl);
    if (code == CURLE_OK) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, response_code);
    curl_easy_setopt(curl, CURLOPT_COOKIELIST, "FLUSH");

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (access(cookie_path, F_OK) == 0 && chmod(cookie_path, 0600) != 0) {
        SecureZero(response->data, response->size);
        free(response->data);
        response->data = NULL;
        LogMessage(LOG_LEVEL_ERROR, "Unable to restrict cookie-jar permissions: errno=%d",
                   errno);
        return AUTHELIA_COOKIE_ERROR;
    }

    if (code != CURLE_OK) {
        AutheliaResult result = ClassifyTransportError(code);
        SecureZero(response->data, response->size);
        free(response->data);
        response->data = NULL;
        LogMessage(LOG_LEVEL_ERROR,
                   "Authelia request failed: server=%d path=%s curl=%d (%s) result=%d",
                   server_index, path, (int)code, curl_easy_strerror(code), (int)result);
        return result;
    }

    LogMessage(LOG_LEVEL_INFO,
               "Authelia request finished: server=%d path=%s status=%ld bytes=%lu",
               server_index, path, *response_code, (unsigned long)response->size);
    return AUTHELIA_OK;
}

static AutheliaResult VerifyAndCommitSession(const OPDSServer *server, int server_index,
                                             const char *staging_path) {
    CURL *curl;
    CURLcode code;
    long http_code = 0;
    char *effective_url = NULL;
    char persistent_path[MAX_STR_LEN * 2];

    if (!server || !CookieJarIsUsable(staging_path) ||
        AuthGetCookieJarPath(server_index, persistent_path, sizeof(persistent_path)) != 0) {
        LogMessage(LOG_LEVEL_ERROR, "Cannot verify staged session for server=%d", server_index);
        return AUTHELIA_COOKIE_ERROR;
    }
    LogMessage(LOG_LEVEL_INFO, "Verifying staged Authelia session for server=%d", server_index);

    curl = curl_easy_init();
    if (!curl) return AUTHELIA_HTTP_ERROR;

    curl_easy_setopt(curl, CURLOPT_URL, server->url);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, APP_USER_AGENT);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    if (access(DEVICE_CA_BUNDLE, R_OK) == 0) {
        curl_easy_setopt(curl, CURLOPT_CAINFO, DEVICE_CA_BUNDLE);
    }
    curl_easy_setopt(curl, CURLOPT_COOKIEFILE, staging_path);
    curl_easy_setopt(curl, CURLOPT_COOKIEJAR, staging_path);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, DiscardWriteCallback);

    code = curl_easy_perform(curl);
    if (code == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &effective_url);
        if (http_code >= 300 && http_code < 400) {
            curl_easy_getinfo(curl, CURLINFO_REDIRECT_URL, &effective_url);
        }
    }
    curl_easy_setopt(curl, CURLOPT_COOKIELIST, "FLUSH");

    int redirected_to_auth = effective_url && server->auth_url[0] &&
                             URLsHaveSameOrigin(effective_url, server->auth_url) &&
                             !URLsHaveSameOrigin(server->url, server->auth_url);
    curl_easy_cleanup(curl);

    if (code != CURLE_OK) {
        AutheliaResult result = ClassifyTransportError(code);
        LogMessage(LOG_LEVEL_ERROR, "Session verification transport failed: curl=%d result=%d",
                   (int)code, (int)result);
        return result;
    }
    if (http_code == 401 || redirected_to_auth) {
        LogMessage(LOG_LEVEL_ERROR,
                   "Session verification rejected: status=%ld redirected_to_auth=%d",
                   http_code, redirected_to_auth);
        return AUTHELIA_COOKIE_ERROR;
    }
    if (http_code < 200 || http_code >= 300) {
        LogMessage(LOG_LEVEL_ERROR, "Session verification returned HTTP %ld", http_code);
        return AUTHELIA_HTTP_ERROR;
    }
    if (!CookieJarIsUsable(staging_path) || chmod(staging_path, 0600) != 0) {
        return AUTHELIA_COOKIE_ERROR;
    }
    if (rename(staging_path, persistent_path) != 0 || chmod(persistent_path, 0600) != 0) {
        LogMessage(LOG_LEVEL_ERROR, "Unable to commit session cookie for server=%d: errno=%d",
                   server_index, errno);
        return AUTHELIA_COOKIE_ERROR;
    }

    LogMessage(LOG_LEVEL_INFO, "Authelia session committed for server=%d", server_index);
    return AUTHELIA_OK;
}

static char *JSONToString(json_object *root) {
    const char *serialized;
    char *copy;
    size_t length;

    if (!root) return NULL;
    serialized = json_object_to_json_string_ext(root, JSON_C_TO_STRING_PLAIN);
    if (!serialized) return NULL;
    length = strlen(serialized);
    copy = malloc(length + 1);
    if (copy) memcpy(copy, serialized, length + 1);
    return copy;
}

void AuthSetCookieDirectory(const char *path) {
    if (!path || !path[0]) {
        strncpy(cookie_directory, AUTH_DIR, sizeof(cookie_directory) - 1);
    } else {
        strncpy(cookie_directory, path, sizeof(cookie_directory) - 1);
    }
    cookie_directory[sizeof(cookie_directory) - 1] = '\0';

    size_t len = strlen(cookie_directory);
    if (len > 0 && cookie_directory[len - 1] != '/' && len + 1 < sizeof(cookie_directory)) {
        cookie_directory[len] = '/';
        cookie_directory[len + 1] = '\0';
    }
}

int AuthGetCookieJarPath(int server_index, char *out, size_t out_size) {
    int written;
    if (server_index < 0 || server_index >= MAX_SERVERS || !out || out_size == 0) return -1;
    written = snprintf(out, out_size, "%sserver_%d.cookies", cookie_directory, server_index);
    return written > 0 && (size_t)written < out_size ? 0 : -1;
}

int AuthCookieJarExists(int server_index) {
    char path[MAX_STR_LEN * 2];
    return AuthGetCookieJarPath(server_index, path, sizeof(path)) == 0 && access(path, F_OK) == 0;
}

int AuthDeleteCookieJar(int server_index) {
    char path[MAX_STR_LEN * 2];
    char staging_path[MAX_STR_LEN * 2];
    if (AuthGetCookieJarPath(server_index, path, sizeof(path)) != 0) return -1;
    if (GetStagingCookieJarPath(server_index, staging_path, sizeof(staging_path)) != 0) return -1;
    if (remove(path) != 0 && errno != ENOENT) return -1;
    if (remove(staging_path) != 0 && errno != ENOENT) return -1;
    LogMessage(LOG_LEVEL_INFO, "Cleared saved and staged sessions for server=%d", server_index);
    return 0;
}

int AuthCancelLogin(int server_index) {
    char staging_path[MAX_STR_LEN * 2];
    if (GetStagingCookieJarPath(server_index, staging_path, sizeof(staging_path)) != 0) return -1;
    if (remove(staging_path) != 0 && errno != ENOENT) return -1;
    return 0;
}

int AuthShiftCookieJarsAfterDelete(int deleted_index, int previous_count) {
    if (deleted_index < 0 || previous_count < 1 || deleted_index >= previous_count) return -1;
    LogMessage(LOG_LEVEL_INFO,
               "Reindexing cookie jars after deleting server=%d previous_count=%d",
               deleted_index, previous_count);

    for (int i = 0; i < previous_count; i++) {
        if (AuthCancelLogin(i) != 0) return -1;
    }

    for (int i = deleted_index; i < previous_count; i++) {
        char source[MAX_STR_LEN * 2];
        char temporary[MAX_STR_LEN * 2];
        if (AuthGetCookieJarPath(i, source, sizeof(source)) != 0 ||
            GetShiftCookieJarPath(i, temporary, sizeof(temporary)) != 0) {
            goto rollback_staging;
        }
        remove(temporary);
        if (access(source, F_OK) == 0 && rename(source, temporary) != 0) {
            goto rollback_staging;
        }
    }

    for (int i = deleted_index + 1; i < previous_count; i++) {
        char temporary[MAX_STR_LEN * 2];
        char target[MAX_STR_LEN * 2];
        if (GetShiftCookieJarPath(i, temporary, sizeof(temporary)) != 0 ||
            AuthGetCookieJarPath(i - 1, target, sizeof(target)) != 0 ||
            (access(temporary, F_OK) == 0 && rename(temporary, target) != 0)) {
            for (int j = i - 1; j > deleted_index; j--) {
                char moved[MAX_STR_LEN * 2];
                char original[MAX_STR_LEN * 2];
                AuthGetCookieJarPath(j - 1, moved, sizeof(moved));
                AuthGetCookieJarPath(j, original, sizeof(original));
                if (access(moved, F_OK) == 0) rename(moved, original);
            }
            goto rollback_staging;
        }
    }

    char deleted_temporary[MAX_STR_LEN * 2];
    if (GetShiftCookieJarPath(deleted_index, deleted_temporary, sizeof(deleted_temporary)) != 0) {
        goto rollback_shifted;
    }
    if (remove(deleted_temporary) != 0 && errno != ENOENT) {
        goto rollback_shifted;
    }
    LogMessage(LOG_LEVEL_INFO, "Cookie-jar reindex completed");
    return 0;

rollback_shifted:
    for (int i = previous_count - 1; i > deleted_index; i--) {
        char moved[MAX_STR_LEN * 2];
        char original[MAX_STR_LEN * 2];
        AuthGetCookieJarPath(i - 1, moved, sizeof(moved));
        AuthGetCookieJarPath(i, original, sizeof(original));
        if (access(moved, F_OK) == 0) rename(moved, original);
    }

rollback_staging:
    for (int i = deleted_index; i < previous_count; i++) {
        char temporary[MAX_STR_LEN * 2];
        char original[MAX_STR_LEN * 2];
        if (GetShiftCookieJarPath(i, temporary, sizeof(temporary)) == 0 &&
            AuthGetCookieJarPath(i, original, sizeof(original)) == 0 &&
            access(temporary, F_OK) == 0) {
            rename(temporary, original);
        }
    }
    LogMessage(LOG_LEVEL_ERROR, "Cookie-jar reindex failed and was rolled back");
    return -1;
}

AutheliaResult AutheliaFirstFactor(const OPDSServer *server, int server_index,
                                   const char *username, const char *password) {
    json_object *root = NULL;
    char *payload = NULL;
    AuthResponse response = {0};
    long code = 0;
    AutheliaResult result;
    char staging_path[MAX_STR_LEN * 2];

    LogMessage(LOG_LEVEL_INFO, "Starting Authelia first-factor flow for server=%d", server_index);
    if (!server || server->auth_mode != AUTH_MODE_AUTHELIA_COOKIE ||
        !username || !username[0] || !password || !password[0] ||
        !IsHTTPSURL(server->url)) {
        LogMessage(LOG_LEVEL_ERROR, "Authelia first-factor configuration is invalid");
        return AUTHELIA_CONFIG_ERROR;
    }
    if (GetStagingCookieJarPath(server_index, staging_path, sizeof(staging_path)) != 0) {
        return AUTHELIA_COOKIE_ERROR;
    }
    if (remove(staging_path) != 0 && errno != ENOENT) return AUTHELIA_COOKIE_ERROR;

    root = json_object_new_object();
    if (!root) return AUTHELIA_HTTP_ERROR;
    json_object_object_add(root, "username", json_object_new_string(username));
    json_object_object_add(root, "password", json_object_new_string(password));
    json_object_object_add(root, "targetURL", json_object_new_string(server->url));
    json_object_object_add(root, "requestMethod", json_object_new_string("GET"));
    json_object_object_add(root, "keepMeLoggedIn", json_object_new_boolean(1));
    payload = JSONToString(root);
    json_object_put(root);
    if (!payload) return AUTHELIA_HTTP_ERROR;

    result = PerformAuthRequest(server, server_index, "/api/firstfactor", "POST", payload,
                                staging_path, &code, &response);
    SecureZero(payload, strlen(payload));
    free(payload);
    if (result != AUTHELIA_OK) {
        remove(staging_path);
        return result;
    }

    if (code == 401 || code == 403) result = AUTHELIA_INVALID_CREDENTIALS;
    else if (code == 429) result = AUTHELIA_RATE_LIMITED;
    else if (code < 200 || code >= 300) result = AUTHELIA_HTTP_ERROR;
    else result = ParseOKResponse(&response);
    SecureZero(response.data, response.size);
    free(response.data);
    if (result != AUTHELIA_OK || !CookieJarIsUsable(staging_path)) {
        remove(staging_path);
        return result != AUTHELIA_OK ? result : AUTHELIA_COOKIE_ERROR;
    }

    response = (AuthResponse){0};
    result = PerformAuthRequest(server, server_index, "/api/user/info", "GET", NULL,
                                staging_path, &code, &response);
    if (result != AUTHELIA_OK) {
        remove(staging_path);
        return result;
    }
    if (code == 401 || code == 403) result = AUTHELIA_INVALID_CREDENTIALS;
    else if (code < 200 || code >= 300) result = AUTHELIA_HTTP_ERROR;
    else {
        json_object *info = json_tokener_parse(response.data);
        json_object *data = NULL;
        json_object *has_totp = NULL;
        if (!info ||
            !json_object_object_get_ex(info, "data", &data) ||
            !json_object_object_get_ex(data, "has_totp", &has_totp)) {
            result = AUTHELIA_INVALID_RESPONSE;
        } else {
            result = json_object_get_boolean(has_totp) ? AUTHELIA_OK : AUTHELIA_TOTP_UNAVAILABLE;
        }
        if (info) json_object_put(info);
    }

    SecureZero(response.data, response.size);
    free(response.data);
    if (result != AUTHELIA_OK) remove(staging_path);
    LogMessage(result == AUTHELIA_OK ? LOG_LEVEL_INFO : LOG_LEVEL_ERROR,
               "Authelia first-factor flow finished: server=%d result=%d",
               server_index, (int)result);
    return result;
}

AutheliaResult AutheliaCompleteTOTP(const OPDSServer *server, int server_index, const char *totp) {
    json_object *root;
    char *payload;
    AuthResponse response = {0};
    long code = 0;
    AutheliaResult result;
    size_t len;
    char staging_path[MAX_STR_LEN * 2];

    LogMessage(LOG_LEVEL_INFO, "Starting Authelia TOTP flow for server=%d", server_index);
    if (!server || server->auth_mode != AUTH_MODE_AUTHELIA_COOKIE || !totp) {
        LogMessage(LOG_LEVEL_ERROR, "Authelia TOTP configuration is invalid");
        return AUTHELIA_CONFIG_ERROR;
    }
    len = strlen(totp);
    if (len != 6 && len != 8) return AUTHELIA_INVALID_TOTP;
    for (size_t i = 0; i < len; i++) {
        if (totp[i] < '0' || totp[i] > '9') return AUTHELIA_INVALID_TOTP;
    }
    if (GetStagingCookieJarPath(server_index, staging_path, sizeof(staging_path)) != 0 ||
        !CookieJarIsUsable(staging_path)) {
        return AUTHELIA_COOKIE_ERROR;
    }

    root = json_object_new_object();
    if (!root) return AUTHELIA_HTTP_ERROR;
    json_object_object_add(root, "token", json_object_new_string(totp));
    json_object_object_add(root, "targetURL", json_object_new_string(server->url));
    payload = JSONToString(root);
    json_object_put(root);
    if (!payload) return AUTHELIA_HTTP_ERROR;

    result = PerformAuthRequest(server, server_index, "/api/secondfactor/totp", "POST", payload,
                                staging_path, &code, &response);
    SecureZero(payload, strlen(payload));
    free(payload);
    if (result != AUTHELIA_OK) {
        remove(staging_path);
        return result;
    }

    if (code == 401 || code == 403) result = AUTHELIA_INVALID_TOTP;
    else if (code == 429) result = AUTHELIA_RATE_LIMITED;
    else if (code < 200 || code >= 300) result = AUTHELIA_HTTP_ERROR;
    else result = ParseOKResponse(&response);

    SecureZero(response.data, response.size);
    free(response.data);
    if (result != AUTHELIA_OK) {
        remove(staging_path);
        return result;
    }

    result = VerifyAndCommitSession(server, server_index, staging_path);
    if (result != AUTHELIA_OK) remove(staging_path);
    LogMessage(result == AUTHELIA_OK ? LOG_LEVEL_INFO : LOG_LEVEL_ERROR,
               "Authelia TOTP flow finished: server=%d result=%d",
               server_index, (int)result);
    return result;
}

AutheliaResult AutheliaLogout(const OPDSServer *server, int server_index) {
    AuthResponse response = {0};
    long code = 0;
    AutheliaResult result = AUTHELIA_OK;
    char cookie_path[MAX_STR_LEN * 2];

    LogMessage(LOG_LEVEL_INFO, "Starting Authelia logout for server=%d", server_index);
    if (AuthGetCookieJarPath(server_index, cookie_path, sizeof(cookie_path)) != 0) {
        return AUTHELIA_COOKIE_ERROR;
    }

    if (server && server->auth_mode == AUTH_MODE_AUTHELIA_COOKIE &&
        IsHTTPSURL(server->auth_url) && CookieJarIsUsable(cookie_path)) {
        result = PerformAuthRequest(server, server_index, "/api/logout", "POST", "{}",
                                    cookie_path, &code, &response);
        if (result == AUTHELIA_OK && (code < 200 || code >= 300)) result = AUTHELIA_HTTP_ERROR;
        if (response.data) {
            SecureZero(response.data, response.size);
            free(response.data);
        }
    }
    if (AuthDeleteCookieJar(server_index) != 0) return AUTHELIA_COOKIE_ERROR;
    LogMessage(result == AUTHELIA_OK ? LOG_LEVEL_INFO : LOG_LEVEL_WARNING,
               "Authelia logout finished: server=%d result=%d", server_index, (int)result);
    return result;
}

const char *AutheliaResultMessage(AutheliaResult result) {
    switch (result) {
        case AUTHELIA_OK: return "Authentication succeeded.";
        case AUTHELIA_INVALID_CREDENTIALS: return "Invalid Authelia username or password.";
        case AUTHELIA_INVALID_TOTP: return "Invalid TOTP code.";
        case AUTHELIA_RATE_LIMITED: return "Authelia rate limit reached. Try again later.";
        case AUTHELIA_TOTP_UNAVAILABLE: return "This account has no TOTP method enrolled.";
        case AUTHELIA_TLS_ERROR: return "Unable to verify the Authelia TLS certificate.";
        case AUTHELIA_INVALID_RESPONSE: return "Authelia returned an unsupported response.";
        case AUTHELIA_COOKIE_ERROR: return "Unable to store the Authelia session cookie.";
        case AUTHELIA_CONFIG_ERROR: return "Authelia URL and OPDS URL must use HTTPS.";
        default: return "Unable to communicate with Authelia.";
    }
}
