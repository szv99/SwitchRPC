#pragma once

#include <string>
#include <time.h>
#include <switch.h>

/**
 * Waits until the network is ready before proceeding.
*/
void waitForNetworkReady();

/**
 * Looks up the eShop square icon URL for a title from Nintendo's EU search API.
 * Returns true and sets out_url on success, false if the title isn't found.
 *
 * @param titleId  The title ID of the game (as a u64).
 * @param out_url  Output: the image_url_sq_s from the EU eShop (500×500 JPEG).
 */
bool fetchEshopIconUrl(u64 titleId, std::string& out_url);

typedef struct {
	u64 pid;          ///< process id
	u64 tid;          ///< title id
	char title_name[513];  ///< Title name 512 + 1 for null terminator
} AppInfo;

/**
 * Retrieves information about the currently running application, including its process ID, title ID, and title name.
 * 
 * @param info Pointer to an AppInfo structure that will be filled with the application information.
 * @return Returns 0 on success, or a non-zero value on error (the int holds the libnx Result).
 */
Result get_app_info(AppInfo *info);

extern PscPmModule g_pscModule;
extern Thread g_pscThread;
extern bool g_pscReady;
extern volatile bool g_pscThreadRun;
extern volatile bool g_wokeFromSleep;
extern volatile bool g_sleepPending;
extern volatile bool g_asleep;
extern UEvent g_pscWakeMain;
extern UEvent g_sleepPrepDone;

void pscThreadFunc(void*);

// the console clock can be off from real time. discord shows the elapsed timer
// relative to its own clock, so the session start timestamp has to be corrected
// with the offset learned from the Date header of discord's responses.
// token expiry and refresh intervals only care about durations and use the raw clock.

/**
 * Returns the console's clock (seconds since epoch) without corrections.
 */
u64 getRawNowSec();

/**
 * Returns the current Unix time (seconds), corrected with the offset from server Date headers.
 */
u64 getCorrectedNowSec();

/**
 * Updates the clock offset from a server's Date header value (Unix seconds).
 */
void syncTimeWithServer(time_t serverUtc);

/**
 * Reads a whole file into out. Returns false if it can't be opened.
 */
bool readFileContents(const char* path, std::string& out);

/**
 * Writes data to path through a temp file and a rename, so a crash or power
 * loss mid-write can't leave a truncated file behind.
 */
bool writeFileAtomic(const char* path, const std::string& data);

/**
 * Deletes a file written by writeFileAtomic, including a leftover temp file.
 */
void removeFileAtomic(const char* path);
