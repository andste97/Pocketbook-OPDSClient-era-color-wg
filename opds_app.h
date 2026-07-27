#ifndef OPDS_APP_H
#define OPDS_APP_H

#include <inkview.h>
#include <stddef.h> 

// libxml2 headers for OPDS parsing
#include <libxml/parser.h>
#include <libxml/xpath.h>
#include <libxml/xpathInternals.h>

// --- Constants ---
#define MAX_SERVERS 10
#define MAX_STR_LEN 512
#define MAX_ENTRIES 1500
#define MAX_FORMATS 5

#define APP_ROOT_DIR "/mnt/ext1/applications/OPDSClient/"
#define IMAGES_DIR APP_ROOT_DIR "images/"
#define AUTH_DIR APP_ROOT_DIR "auth/"
#define BOOKS_DIR "/mnt/ext1/Downloads/"

// New and Legacy Config File Locations
#define NEW_CFG_FILE APP_ROOT_DIR "opds_client.cfg"
#define LEGACY_CFG_FILE "/mnt/ext1/system/config/opds_client.cfg"

#define FOLDER_ICON IMAGES_DIR "folder.png"
#define BOOK_ICON IMAGES_DIR "book.png"

#define APP_USER_AGENT "PocketBook-OPDS-Expert/1.0 (PocketBook Era; Linux)"

typedef enum AppState {
    STATE_MAIN_MENU,
    STATE_SERVER_OPTIONS,
    STATE_SERVER_FORM,
    STATE_AUTH_SETTINGS,
    STATE_BROWSING,
    STATE_BOOK_DETAILS
} AppState;

typedef enum {
    AUTH_MODE_NONE = 0,
    AUTH_MODE_BASIC = 1,
    AUTH_MODE_AUTHELIA_COOKIE = 2
} AuthMode;

typedef enum {
    NETWORK_OK = 0,
    NETWORK_ERROR = -1,
    NETWORK_AUTH_REQUIRED = -2,
    NETWORK_HTTP_ERROR = -3,
    NETWORK_TLS_ERROR = -4
} NetworkResult;

typedef enum {
    AUTHELIA_OK = 0,
    AUTHELIA_INVALID_CREDENTIALS = -1,
    AUTHELIA_INVALID_TOTP = -2,
    AUTHELIA_RATE_LIMITED = -3,
    AUTHELIA_TOTP_UNAVAILABLE = -4,
    AUTHELIA_TLS_ERROR = -5,
    AUTHELIA_HTTP_ERROR = -6,
    AUTHELIA_INVALID_RESPONSE = -7,
    AUTHELIA_COOKIE_ERROR = -8,
    AUTHELIA_CONFIG_ERROR = -9
} AutheliaResult;

typedef enum {
    AUTH_FLOW_IDLE = 0,
    AUTH_FLOW_USERNAME,
    AUTH_FLOW_PASSWORD,
    AUTH_FLOW_TOTP
} AuthFlowState;

typedef enum {
    AUTH_PENDING_NONE = 0,
    AUTH_PENDING_CATALOG,
    AUTH_PENDING_BOOK,
    AUTH_PENDING_SEARCH
} AuthPendingType;

typedef struct {
    char name[MAX_STR_LEN];
    char url[MAX_STR_LEN];
    char user[MAX_STR_LEN];
    char pass[MAX_STR_LEN];
    int auth_mode;
    char auth_url[MAX_STR_LEN];
    int fetch_thumbs; 
    int catalog_rows;
} OPDSServer;

typedef struct {
    AuthFlowState state;
    char username[MAX_STR_LEN];
    char password[MAX_STR_LEN];
    char totp[16];
} AuthFlowContext;

typedef struct {
    AuthPendingType type;
    char url[MAX_STR_LEN];
    int format_index;
} AuthPendingAction;

typedef struct {
    char label[64];
    char url[MAX_STR_LEN];
} BookFormat;

typedef struct {
    char title[MAX_STR_LEN];
    char author[MAX_STR_LEN];
    char summary[1024];
    char cover_url[MAX_STR_LEN];
    char thumb_url[MAX_STR_LEN];
    char nav_url[MAX_STR_LEN];
    BookFormat formats[MAX_FORMATS];
    int format_count;
    int is_book;
} OPDSEntry;

// --- Global Variables (Externs) ---
extern AppState current_state;
extern int server_count;
extern int current_server_index;
extern int editing_server_index;
extern int selected_entry_index;
extern OPDSServer servers[MAX_SERVERS];
extern OPDSServer temp_server;
extern OPDSEntry current_entries[MAX_ENTRIES];
extern int entry_count;
extern char current_host[MAX_STR_LEN];
extern int sys_width, sys_height;
extern char current_search_url[MAX_STR_LEN];
extern char next_page_url[MAX_STR_LEN];
extern char current_feed_title[MAX_STR_LEN];
extern int total_results;

// --- Function Prototypes ---
struct MemoryStruct { char *memory; size_t size; };
void InitNetwork(void);
void CleanupNetwork(void);
void EnsureAbsoluteURL(const char *in_url, char *out_url);
void RedactHTTPHeader(const char *line, char *out, size_t out_size);
int URLsHaveSameOrigin(const char *left, const char *right);
int RequestUsesServerCredentials(const OPDSServer *server, const char *url);
NetworkResult ClassifyNetworkResponse(int curl_code, long http_code,
                                      const char *requested_url, const char *effective_url,
                                      const OPDSServer *server);
NetworkResult FetchFeed(const char *url, const OPDSServer *server, int server_index, struct MemoryStruct *chunk);
NetworkResult DownloadImage(const char *url, const char *filepath, const OPDSServer *server, int server_index);
NetworkResult DownloadBook(const char *url, const char *tmp_path, char *out_filename, const OPDSServer *server, int server_index);

void SecureZero(void *ptr, size_t len);
void NormalizeServerSettings(OPDSServer *server, int auth_mode_present);
AuthMode NextAuthMode(AuthMode mode);
const char *AuthModeLabel(AuthMode mode);
void AuthFlowStart(AuthFlowContext *flow);
int AuthFlowSetUsername(AuthFlowContext *flow, const char *username);
int AuthFlowSetPassword(AuthFlowContext *flow, const char *password);
int AuthFlowSetTOTP(AuthFlowContext *flow, const char *totp);
void AuthFlowCancel(AuthFlowContext *flow);
void AuthPendingClear(AuthPendingAction *pending);
void AuthPendingSetCatalog(AuthPendingAction *pending, const char *url);
void AuthPendingSetBook(AuthPendingAction *pending, int format_index);
void AuthPendingSetSearch(AuthPendingAction *pending, const char *query);

void AuthSetCookieDirectory(const char *path);
int AuthGetCookieJarPath(int server_index, char *out, size_t out_size);
int AuthCookieJarExists(int server_index);
int AuthDeleteCookieJar(int server_index);
int AuthCancelLogin(int server_index);
int AuthShiftCookieJarsAfterDelete(int deleted_index, int previous_count);
AutheliaResult AutheliaFirstFactor(const OPDSServer *server, int server_index, const char *username, const char *password);
AutheliaResult AutheliaCompleteTOTP(const OPDSServer *server, int server_index, const char *totp);
AutheliaResult AutheliaLogout(const OPDSServer *server, int server_index);
const char *AutheliaResultMessage(AutheliaResult result);

int ParseOPDSFeed(const char *xml_data, const char *base_url);

void DrawMainMenu();
void HandleMainMenuTouch(int x, int y);
void DrawServerOptions();
void HandleServerOptionsTouch(int x, int y);
void DrawServerForm();
void HandleServerFormTouch(int x, int y);
void DrawAuthSettings();
void HandleAuthSettingsTouch(int x, int y);
void CancelAuthUI();
void DrawBrowsingView();
void HandleBrowsingTouch(int x, int y);
void DrawBookDetails();
void HandleBookDetailsTouch(int x, int y);
void HandleHardwareButtons(int key);
void LoadCatalog(const char *url);
void Repaint();
int SaveServers();
void LoadServers();

#endif
