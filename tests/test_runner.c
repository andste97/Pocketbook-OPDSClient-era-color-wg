#include "opds_app.h"
#include <arpa/inet.h>
#include <curl/curl.h>
#include <pthread.h>
#include <sys/socket.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

AppState current_state;
int server_count;
int current_server_index;
int editing_server_index;
int selected_entry_index;
OPDSServer servers[MAX_SERVERS];
OPDSServer temp_server;
OPDSEntry current_entries[MAX_ENTRIES];
int entry_count;
char current_host[MAX_STR_LEN];
int sys_width;
int sys_height;

static int tests_run;
static int tests_failed;

typedef enum {
    MOCK_AUTH_SUCCESS,
    MOCK_AUTH_BAD_PASSWORD,
    MOCK_AUTH_NO_TOTP,
    MOCK_AUTH_RATE_LIMITED,
    MOCK_AUTH_BAD_TOTP
} MockAuthMode;

typedef struct {
    int socket_fd;
    int port;
    int expected_requests;
    MockAuthMode mode;
} MockAuthServer;

#define ASSERT_TRUE(expr) do { \
    tests_run++; \
    if (!(expr)) { \
        tests_failed++; \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
    } \
} while (0)

#define ASSERT_EQ_INT(expected, actual) ASSERT_TRUE((expected) == (actual))
#define ASSERT_STREQ(expected, actual) ASSERT_TRUE(strcmp((expected), (actual)) == 0)

void ShowDownloadProgress(long long total, long long current) { (void)total; (void)current; }
int CheckDownloadCancel(void) { return 0; }

static void write_mock_response(int fd, int status, const char *headers, const char *body) {
    char response[4096];
    const char *reason = status == 200 ? "OK" :
                         status == 401 ? "Unauthorized" :
                         status == 403 ? "Forbidden" :
                         status == 429 ? "Too Many Requests" : "Error";
    int length = snprintf(response, sizeof(response),
                          "HTTP/1.1 %d %s\r\n"
                          "Content-Type: application/json\r\n"
                          "Content-Length: %zu\r\n"
                          "Connection: close\r\n"
                          "%s"
                          "\r\n"
                          "%s",
                          status, reason, strlen(body), headers ? headers : "", body);
    if (length > 0) send(fd, response, (size_t)length, 0);
}

static void *mock_auth_server_main(void *userdata) {
    MockAuthServer *server = (MockAuthServer *)userdata;

    for (int request_index = 0; request_index < server->expected_requests; request_index++) {
        int client = accept(server->socket_fd, NULL, NULL);
        if (client < 0) break;

        char request[8192] = {0};
        recv(client, request, sizeof(request) - 1, 0);

        if (strstr(request, "POST /api/firstfactor ")) {
            if (server->mode == MOCK_AUTH_BAD_PASSWORD) {
                write_mock_response(client, 401, NULL, "{\"status\":\"KO\"}");
            } else if (server->mode == MOCK_AUTH_RATE_LIMITED) {
                write_mock_response(client, 429, "Retry-After: 60\r\n", "{\"status\":\"KO\"}");
            } else {
                ASSERT_TRUE(strstr(request, "\"keepMeLoggedIn\":true") != NULL);
                ASSERT_TRUE(strstr(request, "\"username\":\"alice\"") != NULL);
                write_mock_response(client, 200,
                                    "Set-Cookie: authelia_session=one; Path=/; HttpOnly\r\n",
                                    "{\"status\":\"OK\",\"data\":{}}");
            }
        } else if (strstr(request, "GET /api/user/info ")) {
            ASSERT_TRUE(strstr(request, "Cookie: authelia_session=one") != NULL);
            if (server->mode == MOCK_AUTH_NO_TOTP) {
                write_mock_response(client, 200, NULL,
                                    "{\"status\":\"OK\",\"data\":{\"has_totp\":false}}");
            } else {
                write_mock_response(client, 200, NULL,
                                    "{\"status\":\"OK\",\"data\":{\"has_totp\":true,\"method\":\"totp\"}}");
            }
        } else if (strstr(request, "POST /api/secondfactor/totp ")) {
            ASSERT_TRUE(strstr(request, "Cookie: authelia_session=one") != NULL);
            ASSERT_TRUE(strstr(request, "\"token\":\"123456\"") != NULL);
            if (server->mode == MOCK_AUTH_BAD_TOTP) {
                write_mock_response(client, 403, NULL, "{\"status\":\"KO\"}");
            } else {
                write_mock_response(client, 200,
                                    "Set-Cookie: authelia_session=two; Path=/; HttpOnly\r\n",
                                    "{\"status\":\"OK\",\"data\":{}}");
            }
        } else if (strstr(request, "POST /api/logout ")) {
            write_mock_response(client, 200,
                                "Set-Cookie: authelia_session=; Path=/; Max-Age=0\r\n",
                                "{\"status\":\"OK\"}");
        } else if (strstr(request, "GET /opds ")) {
            ASSERT_TRUE(strstr(request, "Cookie: authelia_session=two") != NULL);
            write_mock_response(client, 200, "Content-Type: application/atom+xml\r\n",
                                "<feed xmlns=\"http://www.w3.org/2005/Atom\"><title>Protected</title></feed>");
        } else if (strstr(request, "GET /external ")) {
            ASSERT_TRUE(strstr(request, "Cookie:") == NULL);
            write_mock_response(client, 200, "Content-Type: application/atom+xml\r\n",
                                "<feed xmlns=\"http://www.w3.org/2005/Atom\"><title>External</title></feed>");
        } else {
            write_mock_response(client, 404, NULL, "{\"status\":\"KO\"}");
        }

        close(client);
    }

    close(server->socket_fd);
    return NULL;
}

static int start_mock_auth_server(MockAuthServer *server, MockAuthMode mode, int expected_requests,
                                  pthread_t *thread) {
    struct sockaddr_in address;
    socklen_t address_length = sizeof(address);

    memset(server, 0, sizeof(*server));
    server->mode = mode;
    server->expected_requests = expected_requests;
    server->socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server->socket_fd < 0) return -1;

    int reuse = 1;
    setsockopt(server->socket_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;

    if (bind(server->socket_fd, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        getsockname(server->socket_fd, (struct sockaddr *)&address, &address_length) != 0 ||
        listen(server->socket_fd, 4) != 0) {
        close(server->socket_fd);
        return -1;
    }

    server->port = ntohs(address.sin_port);
    return pthread_create(thread, NULL, mock_auth_server_main, server);
}

static void test_server_normalization(void) {
    OPDSServer server;
    memset(&server, 0, sizeof(server));
    strcpy(server.user, "legacy");
    server.catalog_rows = 99;
    server.fetch_thumbs = 7;

    NormalizeServerSettings(&server, 0);

    ASSERT_EQ_INT(AUTH_MODE_BASIC, server.auth_mode);
    ASSERT_EQ_INT(10, server.catalog_rows);
    ASSERT_EQ_INT(1, server.fetch_thumbs);

    server.auth_mode = 99;
    server.catalog_rows = 0;
    NormalizeServerSettings(&server, 1);
    ASSERT_EQ_INT(AUTH_MODE_NONE, server.auth_mode);
    ASSERT_EQ_INT(4, server.catalog_rows);
}

static void test_auth_mode_cycle(void) {
    ASSERT_EQ_INT(AUTH_MODE_BASIC, NextAuthMode(AUTH_MODE_NONE));
    ASSERT_EQ_INT(AUTH_MODE_AUTHELIA_COOKIE, NextAuthMode(AUTH_MODE_BASIC));
    ASSERT_EQ_INT(AUTH_MODE_NONE, NextAuthMode(AUTH_MODE_AUTHELIA_COOKIE));
    ASSERT_STREQ("Authelia Cookie", AuthModeLabel(AUTH_MODE_AUTHELIA_COOKIE));
}

static void test_auth_flow(void) {
    AuthFlowContext flow;
    AuthFlowStart(&flow);
    ASSERT_EQ_INT(AUTH_FLOW_USERNAME, flow.state);
    ASSERT_EQ_INT(0, AuthFlowSetUsername(&flow, "alice"));
    ASSERT_EQ_INT(AUTH_FLOW_PASSWORD, flow.state);
    ASSERT_EQ_INT(0, AuthFlowSetPassword(&flow, "secret"));
    ASSERT_EQ_INT(AUTH_FLOW_TOTP, flow.state);
    ASSERT_EQ_INT(-1, AuthFlowSetTOTP(&flow, "12ab56"));
    ASSERT_EQ_INT(AUTH_FLOW_TOTP, flow.state);
    ASSERT_EQ_INT(0, AuthFlowSetTOTP(&flow, "123456"));
    ASSERT_EQ_INT(AUTH_FLOW_IDLE, flow.state);
    AuthFlowCancel(&flow);
    ASSERT_TRUE(flow.username[0] == '\0');
    ASSERT_TRUE(flow.password[0] == '\0');
    ASSERT_TRUE(flow.totp[0] == '\0');

    AuthPendingAction pending;
    AuthPendingClear(&pending);
    ASSERT_EQ_INT(AUTH_PENDING_NONE, pending.type);
    AuthPendingSetCatalog(&pending, "https://books.example/opds");
    ASSERT_EQ_INT(AUTH_PENDING_CATALOG, pending.type);
    ASSERT_STREQ("https://books.example/opds", pending.url);
    AuthPendingSetBook(&pending, 2);
    ASSERT_EQ_INT(AUTH_PENDING_BOOK, pending.type);
    ASSERT_EQ_INT(2, pending.format_index);
    AuthPendingSetSearch(&pending, "science fiction");
    ASSERT_EQ_INT(AUTH_PENDING_SEARCH, pending.type);
    ASSERT_STREQ("science fiction", pending.url);
    AuthPendingClear(&pending);
    ASSERT_EQ_INT(-1, pending.format_index);
}

static void test_cookie_jar_indexing(void) {
    char directory[128];
    char first[256];
    char second[256];
    char third[256];
    char shifted[256];
    char shifted_second[256];
    char staging_second[256];
    char staging_shifted[256];
    FILE *file;

    snprintf(directory, sizeof(directory), "/tmp/opds-auth-tests-%ld", (long)getpid());
    mkdir(directory, 0700);
    AuthSetCookieDirectory(directory);

    ASSERT_EQ_INT(0, AuthGetCookieJarPath(0, first, sizeof(first)));
    ASSERT_EQ_INT(0, AuthGetCookieJarPath(1, second, sizeof(second)));
    ASSERT_EQ_INT(0, AuthGetCookieJarPath(2, third, sizeof(third)));
    ASSERT_TRUE(strstr(first, "server_0.cookies") != NULL);
    ASSERT_TRUE(strstr(second, "server_1.cookies") != NULL);

    file = fopen(first, "w");
    ASSERT_TRUE(file != NULL);
    if (file) {
        fputs("first", file);
        fclose(file);
    }
    file = fopen(second, "w");
    ASSERT_TRUE(file != NULL);
    if (file) {
        fputs("second", file);
        fclose(file);
    }
    file = fopen(third, "w");
    ASSERT_TRUE(file != NULL);
    if (file) {
        fputs("third", file);
        fclose(file);
    }
    snprintf(staging_second, sizeof(staging_second), "%s/server_1.login.cookies", directory);
    file = fopen(staging_second, "w");
    ASSERT_TRUE(file != NULL);
    if (file) {
        fputs("staging", file);
        fclose(file);
    }

    ASSERT_EQ_INT(0, AuthShiftCookieJarsAfterDelete(0, 3));
    ASSERT_EQ_INT(0, AuthGetCookieJarPath(0, shifted, sizeof(shifted)));
    ASSERT_EQ_INT(0, AuthGetCookieJarPath(1, shifted_second, sizeof(shifted_second)));
    ASSERT_TRUE(access(shifted, F_OK) == 0);
    ASSERT_TRUE(access(shifted_second, F_OK) == 0);
    ASSERT_TRUE(access(third, F_OK) != 0);
    char shifted_contents[32] = {0};
    file = fopen(shifted, "r");
    ASSERT_TRUE(file != NULL);
    if (file) {
        fread(shifted_contents, 1, sizeof(shifted_contents) - 1, file);
        fclose(file);
    }
    ASSERT_STREQ("second", shifted_contents);
    memset(shifted_contents, 0, sizeof(shifted_contents));
    file = fopen(shifted_second, "r");
    ASSERT_TRUE(file != NULL);
    if (file) {
        fread(shifted_contents, 1, sizeof(shifted_contents) - 1, file);
        fclose(file);
    }
    ASSERT_STREQ("third", shifted_contents);
    snprintf(staging_shifted, sizeof(staging_shifted), "%s/server_0.login.cookies", directory);
    ASSERT_TRUE(access(staging_shifted, F_OK) != 0);
    ASSERT_EQ_INT(0, AuthCancelLogin(0));
    ASSERT_TRUE(access(staging_shifted, F_OK) != 0);
    ASSERT_TRUE(access(shifted, F_OK) == 0);

    ASSERT_EQ_INT(0, AuthDeleteCookieJar(0));
    ASSERT_EQ_INT(0, AuthDeleteCookieJar(1));
    rmdir(directory);
    AuthSetCookieDirectory(NULL);
}

static void setup_mock_server_config(OPDSServer *server, int port) {
    memset(server, 0, sizeof(*server));
    server->auth_mode = AUTH_MODE_AUTHELIA_COOKIE;
    snprintf(server->auth_url, sizeof(server->auth_url), "http://127.0.0.1:%d", port);
    snprintf(server->url, sizeof(server->url), "http://127.0.0.1:%d/opds", port);
}

static void test_authelia_success_flow(void) {
    char directory[128];
    char cookie_path[256];
    char cookie_contents[4096] = {0};
    MockAuthServer mock;
    OPDSServer server;
    pthread_t thread;

    snprintf(directory, sizeof(directory), "/tmp/opds-auth-http-%ld", (long)getpid());
    mkdir(directory, 0700);
    AuthSetCookieDirectory(directory);
    ASSERT_EQ_INT(0, start_mock_auth_server(&mock, MOCK_AUTH_SUCCESS, 7, &thread));
    setup_mock_server_config(&server, mock.port);

    InitNetwork();
    ASSERT_EQ_INT(AUTHELIA_OK, AutheliaFirstFactor(&server, 0, "alice", "secret"));
    ASSERT_EQ_INT(AUTHELIA_OK, AutheliaCompleteTOTP(&server, 0, "123456"));
    ASSERT_TRUE(AuthCookieJarExists(0));

    ASSERT_EQ_INT(0, AuthGetCookieJarPath(0, cookie_path, sizeof(cookie_path)));
    FILE *file = fopen(cookie_path, "r");
    ASSERT_TRUE(file != NULL);
    if (file) {
        fread(cookie_contents, 1, sizeof(cookie_contents) - 1, file);
        fclose(file);
    }
    ASSERT_TRUE(strstr(cookie_contents, "authelia_session") != NULL);
    ASSERT_TRUE(strstr(cookie_contents, "two") != NULL);

    struct MemoryStruct feed = {0};
    ASSERT_EQ_INT(NETWORK_OK, FetchFeed(server.url, &server, 0, &feed));
    ASSERT_TRUE(strstr(feed.memory, "Protected") != NULL);
    free(feed.memory);

    char external_url[256];
    snprintf(external_url, sizeof(external_url), "http://localhost:%d/external", mock.port);
    ASSERT_EQ_INT(NETWORK_OK, FetchFeed(external_url, &server, 0, &feed));
    ASSERT_TRUE(strstr(feed.memory, "External") != NULL);
    free(feed.memory);

    ASSERT_EQ_INT(AUTHELIA_OK, AutheliaLogout(&server, 0));
    ASSERT_TRUE(!AuthCookieJarExists(0));
    pthread_join(thread, NULL);
    rmdir(directory);
    AuthSetCookieDirectory(NULL);
    CleanupNetwork();
}

static void test_network_helpers(void) {
    char output[256];
    char long_request[768];
    OPDSServer server;
    memset(&server, 0, sizeof(server));
    server.auth_mode = AUTH_MODE_AUTHELIA_COOKIE;
    strcpy(server.url, "https://books.example.com/opds");
    strcpy(server.auth_url, "https://auth.example.com");

    RedactHTTPHeader("Authorization: Basic abc", output, sizeof(output));
    ASSERT_STREQ("Authorization: [REDACTED]", output);
    RedactHTTPHeader("Cookie: authelia_session=secret", output, sizeof(output));
    ASSERT_STREQ("Cookie: [REDACTED]", output);
    RedactHTTPHeader("Content-Type: application/atom+xml", output, sizeof(output));
    ASSERT_STREQ("Content-Type: application/atom+xml", output);
    RedactHTTPHeader("GET /opds?token=hidden HTTP/1.1", output, sizeof(output));
    ASSERT_STREQ("GET /opds?[REDACTED] HTTP/1.1", output);
    RedactHTTPHeader("GET /opds?token=hidden", output, sizeof(output));
    ASSERT_STREQ("GET /opds?[REDACTED]", output);
    memcpy(long_request, "GET /", 5);
    memset(long_request + 5, 'a', 600);
    strcpy(long_request + 605, "?token=hidden HTTP/1.1");
    RedactHTTPHeader(long_request, output, sizeof(output));
    ASSERT_STREQ("GET [TRUNCATED AND REDACTED] HTTP/1.1", output);
    RedactHTTPHeader("Location: https://auth.example.com/callback?code=hidden",
                     output, sizeof(output));
    ASSERT_STREQ("Location: https://auth.example.com/callback?[REDACTED]", output);
    RedactURLForLog("https://user@books.example.com/opds?token=secret#fragment",
                    output, sizeof(output));
    ASSERT_STREQ("https://[REDACTED]@books.example.com/opds?[REDACTED]", output);
    RedactURLForLog("https://books.example.com/opds#private", output, sizeof(output));
    ASSERT_STREQ("https://books.example.com/opds#[REDACTED]", output);

    ASSERT_TRUE(URLsHaveSameOrigin("https://books.example.com/a", "https://books.example.com/b"));
    ASSERT_TRUE(URLsHaveSameOrigin("https://books.example.com", "https://books.example.com:443/path"));
    ASSERT_TRUE(URLsHaveSameOrigin("http://books.example.com", "http://books.example.com:80/path"));
    ASSERT_TRUE(!URLsHaveSameOrigin("https://books.example.com:444", "https://books.example.com"));
    ASSERT_TRUE(!URLsHaveSameOrigin("https://books.example.com", "http://books.example.com"));
    ASSERT_TRUE(!URLsHaveSameOrigin("https://books.example.com", "https://auth.example.com"));
    ASSERT_TRUE(RequestUsesServerCredentials(&server, "https://books.example.com/file"));
    ASSERT_TRUE(!RequestUsesServerCredentials(&server, "https://cdn.example.com/file"));

    ASSERT_EQ_INT(NETWORK_AUTH_REQUIRED,
                  ClassifyNetworkResponse(CURLE_OK, 200,
                                          "https://books.example.com/opds",
                                          "https://auth.example.com/?rd=x", &server));
    ASSERT_EQ_INT(NETWORK_AUTH_REQUIRED,
                  ClassifyNetworkResponse(CURLE_OK, 401,
                                          "https://books.example.com/opds",
                                          "https://books.example.com/opds", &server));
    ASSERT_EQ_INT(NETWORK_HTTP_ERROR,
                  ClassifyNetworkResponse(CURLE_OK, 401,
                                          "https://cdn.example.com/book",
                                          "https://cdn.example.com/book", &server));
    ASSERT_EQ_INT(NETWORK_HTTP_ERROR,
                  ClassifyNetworkResponse(CURLE_OK, 500,
                                          "https://books.example.com/opds",
                                          "https://books.example.com/opds", &server));
    ASSERT_EQ_INT(NETWORK_HTTP_ERROR,
                  ClassifyNetworkResponse(CURLE_OK, 403,
                                          "https://books.example.com/opds",
                                          "https://books.example.com/opds", &server));
    ASSERT_EQ_INT(NETWORK_TLS_ERROR,
                  ClassifyNetworkResponse(CURLE_PEER_FAILED_VERIFICATION, 0,
                                          "https://books.example.com/opds", NULL, &server));
}

static void test_authelia_failures(void) {
    char directory[128];
    MockAuthServer mock;
    OPDSServer server;
    pthread_t thread;

    snprintf(directory, sizeof(directory), "/tmp/opds-auth-fail-%ld", (long)getpid());
    mkdir(directory, 0700);
    AuthSetCookieDirectory(directory);
    InitNetwork();

    char persistent_path[256];
    ASSERT_EQ_INT(0, AuthGetCookieJarPath(0, persistent_path, sizeof(persistent_path)));
    FILE *old_session = fopen(persistent_path, "w");
    ASSERT_TRUE(old_session != NULL);
    if (old_session) {
        fputs("old-session", old_session);
        fclose(old_session);
    }

    ASSERT_EQ_INT(0, start_mock_auth_server(&mock, MOCK_AUTH_BAD_PASSWORD, 1, &thread));
    setup_mock_server_config(&server, mock.port);
    ASSERT_EQ_INT(AUTHELIA_INVALID_CREDENTIALS,
                  AutheliaFirstFactor(&server, 0, "alice", "wrong"));
    pthread_join(thread, NULL);
    char old_contents[64] = {0};
    old_session = fopen(persistent_path, "r");
    ASSERT_TRUE(old_session != NULL);
    if (old_session) {
        fread(old_contents, 1, sizeof(old_contents) - 1, old_session);
        fclose(old_session);
    }
    ASSERT_STREQ("old-session", old_contents);

    ASSERT_EQ_INT(0, start_mock_auth_server(&mock, MOCK_AUTH_RATE_LIMITED, 1, &thread));
    setup_mock_server_config(&server, mock.port);
    ASSERT_EQ_INT(AUTHELIA_RATE_LIMITED,
                  AutheliaFirstFactor(&server, 0, "alice", "secret"));
    pthread_join(thread, NULL);

    ASSERT_EQ_INT(0, start_mock_auth_server(&mock, MOCK_AUTH_NO_TOTP, 2, &thread));
    setup_mock_server_config(&server, mock.port);
    ASSERT_EQ_INT(AUTHELIA_TOTP_UNAVAILABLE,
                  AutheliaFirstFactor(&server, 0, "alice", "secret"));
    pthread_join(thread, NULL);
    char staging_path[256];
    snprintf(staging_path, sizeof(staging_path), "%s/server_0.login.cookies", directory);
    ASSERT_TRUE(access(staging_path, F_OK) != 0);

    ASSERT_EQ_INT(0, start_mock_auth_server(&mock, MOCK_AUTH_BAD_TOTP, 3, &thread));
    setup_mock_server_config(&server, mock.port);
    ASSERT_EQ_INT(AUTHELIA_OK, AutheliaFirstFactor(&server, 0, "alice", "secret"));
    ASSERT_EQ_INT(AUTHELIA_INVALID_TOTP, AutheliaCompleteTOTP(&server, 0, "123456"));
    pthread_join(thread, NULL);
    ASSERT_TRUE(access(staging_path, F_OK) != 0);
    old_contents[0] = '\0';
    old_session = fopen(persistent_path, "r");
    ASSERT_TRUE(old_session != NULL);
    if (old_session) {
        fread(old_contents, 1, sizeof(old_contents) - 1, old_session);
        fclose(old_session);
    }
    ASSERT_STREQ("old-session", old_contents);

    ASSERT_EQ_INT(AUTHELIA_INVALID_TOTP, AutheliaCompleteTOTP(&server, 0, "abc"));
    ASSERT_STREQ("Invalid TOTP code.", AutheliaResultMessage(AUTHELIA_INVALID_TOTP));

    AuthDeleteCookieJar(0);
    rmdir(directory);

    char invalid_directory[128];
    snprintf(invalid_directory, sizeof(invalid_directory), "/tmp/opds-auth-file-%ld", (long)getpid());
    FILE *not_a_directory = fopen(invalid_directory, "w");
    ASSERT_TRUE(not_a_directory != NULL);
    if (not_a_directory) fclose(not_a_directory);
    AuthSetCookieDirectory(invalid_directory);
    ASSERT_EQ_INT(AUTHELIA_COOKIE_ERROR,
                  AutheliaFirstFactor(&server, 0, "alice", "secret"));
    remove(invalid_directory);

    AuthSetCookieDirectory(NULL);
    CleanupNetwork();
}

static void test_parser(void) {
    const char *xml =
        "<?xml version=\"1.0\"?>"
        "<feed xmlns=\"http://www.w3.org/2005/Atom\">"
        "<title>Books</title>"
        "<link rel=\"next\" href=\"page2.xml\"/>"
        "<link rel=\"search\" type=\"application/atom+xml\" href=\"search?q={searchTerms}\"/>"
        "<entry><title>Example</title><author><name>Alice</name></author>"
        "<link rel=\"http://opds-spec.org/image/thumbnail\" type=\"image/jpeg\" href=\"cover.jpg\"/>"
        "<link rel=\"http://opds-spec.org/acquisition\" type=\"application/epub+zip\" href=\"book.epub\"/>"
        "</entry>"
        "<entry><title>Folder</title>"
        "<link rel=\"subsection\" type=\"application/atom+xml\" href=\"folder.xml\"/>"
        "</entry></feed>";

    ASSERT_EQ_INT(0, ParseOPDSFeed(xml, "https://books.example/catalog/"));
    ASSERT_EQ_INT(2, entry_count);
    ASSERT_STREQ("Example", current_entries[0].title);
    ASSERT_STREQ("Alice", current_entries[0].author);
    ASSERT_EQ_INT(1, current_entries[0].format_count);
    ASSERT_STREQ("https://books.example/catalog/book.epub", current_entries[0].formats[0].url);
    ASSERT_STREQ("https://books.example/catalog/cover.jpg", current_entries[0].thumb_url);
    ASSERT_STREQ("https://books.example/catalog/page2.xml", next_page_url);
    ASSERT_TRUE(strstr(current_search_url, "search?q=") == current_search_url);
    ASSERT_TRUE(strstr(current_search_url, "searchTerms") != NULL);
    ASSERT_EQ_INT(0, current_entries[1].is_book);
    ASSERT_STREQ("https://books.example/catalog/folder.xml", current_entries[1].nav_url);
    ASSERT_EQ_INT(-1, ParseOPDSFeed(NULL, "https://books.example/catalog/"));
}

int main(void) {
    test_server_normalization();
    test_auth_mode_cycle();
    test_auth_flow();
    test_cookie_jar_indexing();
    test_authelia_success_flow();
    test_authelia_failures();
    test_network_helpers();
    test_parser();

    if (tests_failed) {
        fprintf(stderr, "%d of %d assertions failed\n", tests_failed, tests_run);
        return EXIT_FAILURE;
    }

    printf("All %d assertions passed\n", tests_run);
    return EXIT_SUCCESS;
}
