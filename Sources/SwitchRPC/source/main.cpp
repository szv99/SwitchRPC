// Include the most common headers from the C standard library
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <string>

// Include the main libnx system header, for Switch development
#include <switch.h>
#include <json-c/json.h>
#include <curl/curl.h>

#include "discord.hpp"
#include "logging.hpp"
#include "utilities.hpp"

// might rewrite this sysmodule in swift again.
// i didnt need to rewrite in c++ but i ran out of ideas to fix crashing in swift
// but it was just a big struct being put on stack in swift. 

// Size of the inner heap (adjust as necessary).
#define INNER_HEAP_SIZE 0x80000

// switch_uid of the link whose headless session is live, empty if none
static std::string g_activeUid;

static Link* activeLink() {
    return g_activeUid.empty() ? nullptr : findLink(g_activeUid);
}

static void clearPresence() {
    if (Link* link = activeLink()) discordDeleteHeadlessSession(*link);
    g_activeUid.clear();
}

#ifdef __cplusplus
extern "C" {
#endif

// Sysmodules should not use applet*.
u32 __nx_applet_type = AppletType_None;
SslServiceType __nx_ssl_service_type = SslServiceType_System;

// Sysmodules will normally only want to use one FS session.
u32 __nx_fs_num_sessions = 1;


// Newlib heap configuration function (makes malloc/free work).
void __libnx_initheap(void)
{
    static u8 inner_heap[INNER_HEAP_SIZE];
    extern void* fake_heap_start;
    extern void* fake_heap_end;

    // Configure the newlib heap.
    fake_heap_start = inner_heap;
    fake_heap_end   = inner_heap + sizeof(inner_heap);
}

// Service initialization.
void __appInit(void)
{
    Result rc;

    // Open a service manager session.
    rc = smInitialize();
    if (R_FAILED(rc))
        diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_InitFail_SM));

    // Retrieve the current version of Horizon OS.
    rc = setsysInitialize();
    if (R_SUCCEEDED(rc)) {
        SetSysFirmwareVersion fw;
        rc = setsysGetFirmwareVersion(&fw);
        if (R_SUCCEEDED(rc))
            hosversionSet(MAKEHOSVERSION(fw.major, fw.minor, fw.micro)); 
        setsysExit();
    }

    // Disable this if you don't want to use the filesystem.
    rc = fsInitialize();
    if (R_FAILED(rc))
        diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_InitFail_FS));

    timeInitialize(); // time() for log timestamps, the refresh timer and token expiry
    nifmInitialize(NifmServiceType_System);

    // Disable this if you don't want to use the SD card filesystem.
    fsdevMountSdmc();
    initLog();

    // Add other services you want to use here.
    SocketInitConfig sockConf = {
        .tcp_tx_buf_size = 0x800,
        .tcp_rx_buf_size = 0x1000,
        .tcp_tx_buf_max_size = 0x2EE0,
        .tcp_rx_buf_max_size = 0x2EE0,

        .udp_tx_buf_size = 0x1000,
        .udp_rx_buf_size = 0x1000,

        .sb_efficiency = 4,

        .num_bsd_sessions = 3,
        .bsd_service_type = BsdServiceType_System,
    };
    rc = socketInitialize(&sockConf);
    if (R_FAILED(rc))
        writeToLog("[SwitchRPC] Warning: socketInitialize failed with code 0x%08X. This might be due to memory constraints. Network features will not work. Maybe disable other sysmodules?", rc);
    curl_global_init(CURL_GLOBAL_DEFAULT);
	
	pmdmntInitialize();
	nsInitialize();
	setInitialize();
	pscmInitialize();

	// to find out which user is playing, and to list users for the config app
	rc = accountInitialize(AccountServiceType_System);
	if (R_FAILED(rc))
		writeToLog("[SwitchRPC] Warning: accountInitialize failed with 0x%08X. Every user will get the Default link.", rc);

    // Close the service manager session.
    smExit();
}

// Service deinitialization.
void __appExit(void)
{
    // stop the psc thread first, we're leaving anyway
    g_pscThreadRun = false;
    if (g_pscReady) {
        threadWaitForExit(&g_pscThread);
        threadClose(&g_pscThread);
        pscPmModuleFinalize(&g_pscModule);
        pscPmModuleClose(&g_pscModule);
    }
    pscmExit();

    clearPresence();
    writeToLog("[SwitchRPC] Sysmodule exiting. Goodbye!");

    curl_global_cleanup();

    socketExit();
    accountExit();
    setExit();
    nsExit();
    pmdmntExit();
    nifmExit();
    timeExit();

    // Close extra services you added to __appInit here.
    fsdevUnmountAll(); // Disable this if you don't want to use the SD card filesystem.
    fsExit(); // Disable this if you don't want to use the filesystem.
}

#ifdef __cplusplus
}
#endif


// the config app can't use the account service when it runs as an applet,
// so the sysmodule writes the user list for it
static void writeSwitchUsers() {
    AccountUid uids[ACC_USER_LIST_SIZE];
    s32 total = 0;
    if (R_FAILED(accountListAllUsers(uids, ACC_USER_LIST_SIZE, &total))) {
        writeToLog("[SwitchRPC] accountListAllUsers failed, not writing the user list.");
        return;
    }

    json_object* root = json_object_new_object();
    json_object* users = json_object_new_array();
    json_object_object_add(root, "users", users);

    for (s32 i = 0; i < total; i++) {
        char nickname[sizeof(AccountProfileBase::nickname) + 1] = {0};
        AccountProfile profile;
        if (R_SUCCEEDED(accountGetProfile(&profile, uids[i]))) {
            AccountProfileBase base = {};
            if (R_SUCCEEDED(accountProfileGet(&profile, NULL, &base))) {
                memcpy(nickname, base.nickname, sizeof(base.nickname));
            }
            accountProfileClose(&profile);
        }

        json_object* user = json_object_new_object();
        json_object_object_add(user, "uid", json_object_new_string(accountUidToString(uids[i]).c_str()));
        json_object_object_add(user, "nickname", json_object_new_string(nickname));
        json_object_array_add(users, user);
    }

    writeFileAtomic("sdmc:/config/switchrpc_users.json", json_object_to_json_string(root));
    json_object_put(root);
}

// the link of the user that's playing (the last one picked in the user selector)
static Link* linkForCurrentUser() {
    AccountUid uid = {};
    accountGetLastOpenedUser(&uid); // on failure uid stays zero, which resolves to the Default link
    return resolveLink(uid);
}

const int REFRESH_INTERVAL = 15 * 60;
const int RETRY_INTERVAL = 60;

int main(int argc, char* argv[])
{
    writeToLog("[SwitchRPC] Sysmodule started successfully.");

    AppInfo lastInfo = {0};
    u64 lastAttemptRaw = 0;   // raw seconds of the last session create/refresh
    u64 attemptInterval = 0;  // REFRESH_INTERVAL after a success, RETRY_INTERVAL after a failure
    u64 accumulatedAwake = 0; // seconds of awake playtime banked for the current game
    u64 chunkStartRaw = 0;    // raw local seconds when the current awake play chunk started

    loadLinks();
    writeSwitchUsers();

    waitForNetworkReady();

    // Start by cleaning up stale sessions
    discordCleanupStaleSessions();

    // register with psc and start the thread that watches for sleep/wake
    {
        ueventCreate(&g_pscWakeMain, true);
        ueventCreate(&g_sleepPrepDone, true);
        static const u32 pscDeps[] = { PscPmModuleId_Fs, PscPmModuleId_Nifm };
        Result prc = pscmGetPmModule(&g_pscModule, (PscPmModuleId)0x5250, pscDeps,
                                     sizeof(pscDeps) / sizeof(pscDeps[0]), true);
        if (R_SUCCEEDED(prc)) {
            if (R_SUCCEEDED(threadCreate(&g_pscThread, pscThreadFunc, NULL, NULL, 0x4000, 0x2C, -2))) {
                threadStart(&g_pscThread);
                g_pscReady = true;
                writeToLog("[SwitchRPC] PSC power-state monitoring active.");
            } else {
                // no ack thread = we'd stall power transitions, so unregister
                pscPmModuleFinalize(&g_pscModule);
                pscPmModuleClose(&g_pscModule);
                writeToLog("[SwitchRPC] Warning: could not start PSC thread; sleep detection disabled.");
            }
        } else {
            writeToLog("[SwitchRPC] Warning: pscmGetPmModule failed (0x%08X); sleep detection disabled.", prc);
        }
    }

    while (true) {
        // just woke up - the presence was cleared before sleeping, the loop below
        // recreates it for the same game, continuing its playtime timer
        // (the sleep period is excluded, see the sleep handler below)
        if (g_wokeFromSleep) {
            g_wokeFromSleep = false;
            g_asleep = false;
            waitForNetworkReady();
            writeSwitchUsers();
            discordCleanupStaleSessions();

            if (lastInfo.tid != 0) {
                writeToLog("[SwitchRPC] Wake from sleep (PSC). Resuming session for TID: %016llX", (unsigned long long)lastInfo.tid);
                chunkStartRaw = getRawNowSec();
            }
            attemptInterval = 0;
        }

        // asleep = presence already gone, don't poll or we'd just recreate it
        // right before the console freezes
        if (!g_asleep) {
            // picks up profiles linked, unlinked or relinked in the config app.
            // sessions of removed links are deleted by the reload itself.
            if (reloadLinksIfChanged()) {
                Link* active = activeLink();
                if (active == nullptr || active->sessionToken.empty()) g_activeUid.clear();
                attemptInterval = 0;
            }

            AppInfo info = {0};
            Result rc = get_app_info(&info);

            bool is_game_running = R_SUCCEEDED(rc) && info.tid != 0;

            if (is_game_running) {
                // user opened a game or switched a game or switched from home menu to game (tid changed)
                if (info.tid != lastInfo.tid) {
                    writeToLog("[SwitchRPC] Game state changed! New TID: %016llX, Name: %s. Previous TID: %016llX",
                               (unsigned long long)info.tid, info.title_name, (unsigned long long)lastInfo.tid);

                    // kill the previous game's session first, otherwise they stack
                    // up on discord's side (and the new game gets a fresh timer)
                    clearPresence();

                    lastInfo = info;
                    accumulatedAwake = 0;
                    chunkStartRaw = getRawNowSec();
                    attemptInterval = 0;
                    waitForNetworkReady();
                }

                Link* link = linkForCurrentUser();
                Link* active = activeLink();
                u64 now = getRawNowSec();
                u64 displayStart = chunkStartRaw - accumulatedAwake; // moved forward to skip sleep

                // a different user is playing now, or the current one got unlinked
                if (link != active && active != nullptr) {
                    writeToLog("[SwitchRPC] Playing user changed, moving presence to %s",
                               link ? link->switchUidStr.c_str() : "(no link)");
                    clearPresence();
                    active = nullptr;
                    attemptInterval = 0;
                }

                // unsigned on purpose: if the clock jumps backwards this wraps and counts as due
                if (link != nullptr && now - lastAttemptRaw >= attemptInterval) {
                    // refresh the live session (keeping the same elapsed start), or create one
                    bool refreshing = (link == active);
                    if (refreshing) {
                        writeToLog("[SwitchRPC] Periodic session refresh triggered for TID: %016llX", (unsigned long long)info.tid);
                    }
                    bool ok = discordCreateHeadlessSession(*link, info.tid, std::string(info.title_name), displayStart, refreshing);
                    if (ok) g_activeUid = link->switchUidStr;
                    lastAttemptRaw = now;
                    attemptInterval = ok ? REFRESH_INTERVAL : RETRY_INTERVAL;
                }
            } else {
                // user isn't in a game. if we had a session before, delete it since the game closed, and reset lastInfo.
                if (lastInfo.tid != 0) {
                    writeToLog("[SwitchRPC] Game closed or returned to Home Menu. Clearing session for TID: %016llX", (unsigned long long)lastInfo.tid);
                    waitForNetworkReady();

                    lastInfo = {0};
                    accumulatedAwake = 0;
                    chunkStartRaw = 0;

                    clearPresence();
                }
            }
        }

        // wait ~10s, or wake early if psc pokes us
        waitSingle(waiterForUEvent(&g_pscWakeMain), 10ULL * 1000 * 1000 * 1000);

        // about to sleep, wifi still up - kill the presence now before we freeze,
        // stop polling until we wake, then let the psc thread ack
        if (g_sleepPending) {
            g_sleepPending = false;
            writeToLog("[SwitchRPC] Sleep imminent (PSC). Clearing Discord presence before sleep.");

            // bank the awake time played so far so the timer resumes minus sleep.
            // keep lastInfo so we know which game to resume on wake.
            if (lastInfo.tid != 0) {
                accumulatedAwake += getRawNowSec() - chunkStartRaw;
            }

            clearPresence();
            g_asleep = true;

            ueventSignal(&g_sleepPrepDone);
        }
    }

    return 0;
}
