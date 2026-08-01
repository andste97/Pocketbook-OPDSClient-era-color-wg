#include "opds_app.h"
#include <inkview.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dlfcn.h> // Included for dynamic library loading (dlsym)

// --- Dark Mode / Capabilities Definitions ---
// We define these here just in case the older SDK headers don't have them
#ifndef APP_CAPABILITY_SUPPORT_SCREEN_INVERSION
#define APP_CAPABILITY_SUPPORT_SCREEN_INVERSION (1 << 0)
#endif

typedef void (*IvSetAppCapability_t)(int);

// Global State Variables
enum AppState current_state = STATE_MAIN_MENU; 
int server_count = 0;
int current_server_index = -1;
int editing_server_index = -1;
int selected_entry_index = -1;
OPDSServer servers[MAX_SERVERS];
OPDSServer temp_server;
OPDSEntry current_entries[MAX_ENTRIES];

// Temporary struct required to safely migrate users with legacy binary files 
// without crashing due to mismatched struct sizes
typedef struct {
    char name[MAX_STR_LEN];
    char url[MAX_STR_LEN];
    char user[MAX_STR_LEN];
    char pass[MAX_STR_LEN];
    int fetch_thumbs; 
} OldOPDSServer;

int entry_count = 0;
char current_host[MAX_STR_LEN];
int sys_width, sys_height;

// External UI Handlers
extern void DrawMainMenu();
extern void HandleMainMenuTouch(int x, int y);
extern void DrawServerOptions();
extern void HandleServerOptionsTouch(int x, int y);
extern void DrawServerForm();
extern void HandleServerFormTouch(int x, int y);
extern void DrawBrowsingView();
extern void HandleBrowsingTouch(int x, int y);
extern void DrawBookDetails();
extern void HandleBookDetailsTouch(int x, int y);
extern void HandleHardwareButtons(int key);
extern void InitNetwork();
extern void CleanupNetwork();

void Repaint() {
    ClearScreen();
    switch (current_state) {
        case STATE_MAIN_MENU:      DrawMainMenu(); break;
        case STATE_SERVER_OPTIONS: DrawServerOptions(); break;
        case STATE_SERVER_FORM:    DrawServerForm(); break;
        case STATE_AUTH_SETTINGS:  DrawAuthSettings(); break;
        case STATE_BROWSING:       DrawBrowsingView(); break;
        case STATE_BOOK_DETAILS:   DrawBookDetails(); break;
    }
    FullUpdate();
}

int SaveServers() {
    LogMessage(LOG_LEVEL_INFO, "Saving configuration for %d server(s)", server_count);
    // Ensure the application directory exists before attempting to save!
    mkdir(APP_ROOT_DIR, 0777);

    // Touch the file to ensure it exists before OpenConfig tries to read it
    FILE *f = fopen(NEW_CFG_FILE, "a");
    if (f) fclose(f);

    // Use InkView's built-in config system
    iconfig *cfg = OpenConfig(NEW_CFG_FILE, NULL);
    if (!cfg) {
        LogMessage(LOG_LEVEL_ERROR, "Failed to open configuration for saving");
        return -1;
    }

    WriteInt(cfg, "server_count", server_count);
    
    for (int i = 0; i < server_count; i++) {
        char key[64];
        
        snprintf(key, sizeof(key), "server_%d_name", i); 
        WriteString(cfg, key, servers[i].name);
        
        snprintf(key, sizeof(key), "server_%d_url", i);  
        WriteString(cfg, key, servers[i].url);
        
        snprintf(key, sizeof(key), "server_%d_user", i); 
        WriteString(cfg, key, servers[i].user);
        
        snprintf(key, sizeof(key), "server_%d_pass", i); 
        WriteString(cfg, key, servers[i].pass);

        snprintf(key, sizeof(key), "server_%d_auth_mode", i);
        WriteInt(cfg, key, servers[i].auth_mode);

        snprintf(key, sizeof(key), "server_%d_auth_url", i);
        WriteString(cfg, key, servers[i].auth_url);
        
        snprintf(key, sizeof(key), "server_%d_fetch_thumbs", i); 
        WriteInt(cfg, key, servers[i].fetch_thumbs);
        
        snprintf(key, sizeof(key), "server_%d_catalog_rows", i); 
        WriteInt(cfg, key, servers[i].catalog_rows);
    }
    
    int save_result = SaveConfig(cfg);
    CloseConfig(cfg);
    // InkView's SaveConfig reports success with a non-negative value (1 when the file is written).
    if (save_result < 0) {
        LogMessage(LOG_LEVEL_ERROR, "Failed to save configuration (save=%d errno=%d)",
                   save_result, errno);
        return -1;
    }
    LogMessage(LOG_LEVEL_INFO, "Configuration saved successfully");
    return 0;
}

void LoadServers() {
    LogMessage(LOG_LEVEL_INFO, "Loading server configuration");
    // Ensure the app directory is available immediately on boot
    mkdir(APP_ROOT_DIR, 0777);
    mkdir(AUTH_DIR, 0700);

    // --- NORMAL TEXT LOAD FROM NEW LOCATION ---
    // Explicitly check if the file exists on the filesystem first.
    if (access(NEW_CFG_FILE, F_OK) == 0) {
        iconfig *cfg = OpenConfig(NEW_CFG_FILE, NULL);
        if (cfg) {
            server_count = ReadInt(cfg, "server_count", 0);
            if (server_count > MAX_SERVERS) server_count = MAX_SERVERS;

            for (int i = 0; i < server_count; i++) {
                char key[64];
                
                snprintf(key, sizeof(key), "server_%d_name", i);
                strncpy(servers[i].name, ReadString(cfg, key, ""), MAX_STR_LEN - 1);
                
                snprintf(key, sizeof(key), "server_%d_url", i);
                strncpy(servers[i].url, ReadString(cfg, key, ""), MAX_STR_LEN - 1);
                
                snprintf(key, sizeof(key), "server_%d_user", i);
                strncpy(servers[i].user, ReadString(cfg, key, ""), MAX_STR_LEN - 1);
                
                snprintf(key, sizeof(key), "server_%d_pass", i);
                strncpy(servers[i].pass, ReadString(cfg, key, ""), MAX_STR_LEN - 1);

                snprintf(key, sizeof(key), "server_%d_auth_mode", i);
                int saved_auth_mode = ReadInt(cfg, key, -1);
                servers[i].auth_mode = saved_auth_mode;

                snprintf(key, sizeof(key), "server_%d_auth_url", i);
                strncpy(servers[i].auth_url, ReadString(cfg, key, ""), MAX_STR_LEN - 1);
                
                snprintf(key, sizeof(key), "server_%d_fetch_thumbs", i);
                servers[i].fetch_thumbs = ReadInt(cfg, key, 1); 
                
                snprintf(key, sizeof(key), "server_%d_catalog_rows", i);
                servers[i].catalog_rows = ReadInt(cfg, key, 10); 
                NormalizeServerSettings(&servers[i], saved_auth_mode != -1);
            }

            CloseConfig(cfg);
            LogMessage(LOG_LEVEL_INFO, "Loaded %d server(s) from current configuration",
                       server_count);
        } else {
            LogMessage(LOG_LEVEL_ERROR, "OpenConfig failed for existing configuration");
        }
        return; // Successfully loaded from new location
    }

    // --- MIGRATION CHECK FROM LEGACY LOCATION ---
    if (access(LEGACY_CFG_FILE, F_OK) == 0) {
        FILE *fp = fopen(LEGACY_CFG_FILE, "rb");
        if (fp) {
            int magic_check = -1;
            fread(&magic_check, sizeof(int), 1, fp);
            fclose(fp);

            if (magic_check >= 0 && magic_check <= MAX_SERVERS) {
                LogDebug("Detected old binary config format in legacy location. Migrating...");
                fp = fopen(LEGACY_CFG_FILE, "rb");
                if (fread(&server_count, sizeof(int), 1, fp) == 1) {
                    if (server_count > MAX_SERVERS) server_count = MAX_SERVERS;
                    OldOPDSServer old_servers[MAX_SERVERS];
                    fread(old_servers, sizeof(OldOPDSServer), server_count, fp);
                    for(int i = 0; i < server_count; i++) {
                        strncpy(servers[i].name, old_servers[i].name, MAX_STR_LEN);
                        strncpy(servers[i].url, old_servers[i].url, MAX_STR_LEN);
                        strncpy(servers[i].user, old_servers[i].user, MAX_STR_LEN);
                        strncpy(servers[i].pass, old_servers[i].pass, MAX_STR_LEN);
                        servers[i].auth_mode = old_servers[i].user[0] ? AUTH_MODE_BASIC : AUTH_MODE_NONE;
                        servers[i].auth_url[0] = '\0';
                        servers[i].fetch_thumbs = old_servers[i].fetch_thumbs;
                        servers[i].catalog_rows = 10; // Default new property
                        NormalizeServerSettings(&servers[i], 1);
                    }
                }
                fclose(fp);
            } else {
                LogDebug("Detected intermediate text config in legacy location. Migrating...");
                iconfig *old_cfg = OpenConfig(LEGACY_CFG_FILE, NULL);
                if (old_cfg) {
                    server_count = ReadInt(old_cfg, "server_count", 0);
                    if (server_count > MAX_SERVERS) server_count = MAX_SERVERS;

                    for (int i = 0; i < server_count; i++) {
                        char key[64];
                        snprintf(key, sizeof(key), "server_%d_name", i);
                        strncpy(servers[i].name, ReadString(old_cfg, key, ""), MAX_STR_LEN - 1);
                        snprintf(key, sizeof(key), "server_%d_url", i);
                        strncpy(servers[i].url, ReadString(old_cfg, key, ""), MAX_STR_LEN - 1);
                        snprintf(key, sizeof(key), "server_%d_user", i);
                        strncpy(servers[i].user, ReadString(old_cfg, key, ""), MAX_STR_LEN - 1);
                        snprintf(key, sizeof(key), "server_%d_pass", i);
                        strncpy(servers[i].pass, ReadString(old_cfg, key, ""), MAX_STR_LEN - 1);
                        servers[i].auth_url[0] = '\0';
                        servers[i].auth_mode = servers[i].user[0] ? AUTH_MODE_BASIC : AUTH_MODE_NONE;
                        snprintf(key, sizeof(key), "server_%d_fetch_thumbs", i);
                        servers[i].fetch_thumbs = ReadInt(old_cfg, key, 1); 
                        snprintf(key, sizeof(key), "server_%d_catalog_rows", i);
                        servers[i].catalog_rows = ReadInt(old_cfg, key, 10); 
                        NormalizeServerSettings(&servers[i], 1);
                    }
                    CloseConfig(old_cfg);
                }
            }
            
            // Backup the old file so we don't read it again
            char bak_file[MAX_STR_LEN];
            snprintf(bak_file, sizeof(bak_file), "%s.bak", LEGACY_CFG_FILE);
            rename(LEGACY_CFG_FILE, bak_file); 
            
            // Save immediately in the new robust text format at the NEW location
            if (SaveServers() != 0) {
                LogMessage(LOG_LEVEL_ERROR, "Failed to save migrated server configuration");
            } else {
                LogMessage(LOG_LEVEL_INFO, "Legacy configuration migration completed");
            }
        }
    } else {
        server_count = 0;
        LogMessage(LOG_LEVEL_INFO, "No configuration found; starting with an empty server list");
    }
}

static int main_handler(int event, int a, int b) {
    switch (event) {

        case EVT_INIT:
            LogMessage(LOG_LEVEL_INFO, "EVT_INIT received");
            sys_width = ScreenWidth();
            sys_height = ScreenHeight();
            LogMessage(LOG_LEVEL_INFO, "Screen dimensions: %dx%d", sys_width, sys_height);
            LoadServers();
            InitNetwork();
            LogMessage(LOG_LEVEL_INFO, "Application initialization completed");

            // --- Dynamic Loading for Dark Mode (FW 6.8+) ---
            // Try to open the inkview library to see if it supports native screen inversion
            void *lib_handle = dlopen("libinkview.so", RTLD_LAZY);
            if (!lib_handle) lib_handle = dlopen(NULL, RTLD_LAZY); // Fallback to global symbol space
            
            if (lib_handle) {
                IvSetAppCapability_t set_cap = (IvSetAppCapability_t)dlsym(lib_handle, "IvSetAppCapability");
                if (set_cap) {
                    LogDebug("Firmware 6.8+ detected. Enabling native dark mode.");
                    set_cap(APP_CAPABILITY_SUPPORT_SCREEN_INVERSION);
                } else {
                    LogDebug("Firmware < 6.8. Native dark mode unsupported.");
                }
                // Not calling dlclose(lib_handle) intentionally, as inkview is a core system 
                // dependency permanently linked to our app's lifetime anyway.
            } else {
                LogMessage(LOG_LEVEL_WARNING, "Unable to open libinkview for capability detection");
            }
            break;

        case EVT_SHOW:
        case EVT_FOREGROUND:
            LogMessage(LOG_LEVEL_INFO, "Application shown/foregrounded (event=%d)", event);
            // Handle app un-minimization or device waking from sleep
            SetPanelType(0); 
            Repaint();       
            break;
            
        case EVT_BACKGROUND:
            // Handle device sleep or app minimization
            LogMessage(LOG_LEVEL_INFO, "Application moved to background");
            break;

        case EVT_CONFIGCHANGED:
        case EVT_OBREEY_CONFIG_CHANGED:
            // Triggers a redraw if the user toggles Dark Mode from the system drop-down menu
            Repaint();
            break;

        case EVT_KEYPRESS:
            if (a == IV_KEY_BACK) { 
                if (current_state == STATE_MAIN_MENU) CloseApp();
                else if (current_state == STATE_AUTH_SETTINGS) {
                    CancelAuthUI();
                    current_state = STATE_SERVER_OPTIONS;
                    Repaint();
                }
                else {
                    current_state = STATE_MAIN_MENU;
                    Repaint();
                }
            } else {
                HandleHardwareButtons(a);
            }
            break;

        case EVT_POINTERUP:
            switch (current_state) {
                case STATE_MAIN_MENU:      HandleMainMenuTouch(a, b); break;
                case STATE_SERVER_OPTIONS: HandleServerOptionsTouch(a, b); break;
                case STATE_SERVER_FORM:    HandleServerFormTouch(a, b); break;
                case STATE_AUTH_SETTINGS:  HandleAuthSettingsTouch(a, b); break;
                case STATE_BROWSING:       HandleBrowsingTouch(a, b); break;
                case STATE_BOOK_DETAILS:   HandleBookDetailsTouch(a, b); break;
            }
            break;

        case EVT_EXIT:
            LogMessage(LOG_LEVEL_INFO, "EVT_EXIT received; cleaning up");
            CancelAuthUI();
            CleanupNetwork();
            break;
    }
    return 0;
}

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    InitLogger();
    InstallCrashHandlers();
    LogMessage(LOG_LEVEL_INFO, "Entering InkViewMain");
    InkViewMain(main_handler);
    LogMessage(LOG_LEVEL_INFO, "InkViewMain returned");
    ShutdownLogger();
    return 0;
}
