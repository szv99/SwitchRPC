// discord.hpp

#pragma once

#include <switch.h>
#include <string>
#include <vector>

// ------------------------------------------------------------------
// Switch user -> Discord account links
// ------------------------------------------------------------------

// One Switch user profile linked to one Discord account. Each link has its own
// OAuth credentials and its own headless session.
struct Link {
    AccountUid switchUid {};    // all zero = the "Default" link, used for unlinked users
    std::string switchUidStr;   // "%016llx-%016llx", same format as the config app
    std::string discordId;
    std::string discordUsername;
    std::string refreshToken;   // rotates on every refresh

    // runtime only, never written to disk
    bool revoked = false;       // discord rejected the refresh token, needs relinking
    std::string authToken;
    u64 authTokenExpiry = 0;    // raw clock, see getRawNowSec()
    std::string sessionToken;   // active headless session, empty if none
};

std::string accountUidToString(const AccountUid& uid);
bool accountUidFromString(const std::string& s, AccountUid& out);

/**
 * Loads links from sdmc:/config/switchrpc_profiles.json, or from the legacy
 * sdmc:/config/switchrpc_token (as a single Default link) if there's no profiles file.
 * Invalidates every Link pointer handed out before.
 */
void loadLinks();

/**
 * Re-reads the links if the config app changed them since the last load.
 * Links whose refresh token didn't change keep their auth and session tokens.
 * Sessions of links that were removed or relinked are deleted first, with the
 * old credentials. Invalidates Link pointers if it returns true.
 */
bool reloadLinksIfChanged();

/**
 * The link for a Switch user: the exact match if there is one, else the Default
 * link, else nullptr (no presence for that user).
 */
Link* resolveLink(const AccountUid& uid);

/**
 * The link with the given switch_uid string, or nullptr.
 */
Link* findLink(const std::string& switchUidStr);

// ------------------------------------------------------------------
// Headless sessions
// ------------------------------------------------------------------

/**
 * Creates or updates the link's Discord headless session to show the current game.
 *
 * @param startRawSec Raw console clock time (see getRawNowSec) the elapsed timer counts from; 0 to omit it.
 * @param includeToken Whether to update the link's existing session instead of making a new one.
 * @return true if Discord returned a session token.
 */
bool discordCreateHeadlessSession(Link& link, u64 titleId, const std::string& titleName, u64 startRawSec, bool includeToken);

/**
 * Deletes the link's active headless session, if it has one.
 */
void discordDeleteHeadlessSession(Link& link);

/**
 * Deletes every session recorded in sdmc:/config/switchrpc_sessions.json, using
 * the credentials of the link that created it, then clears the file.
 * Call on startup and after waking up.
 */
void discordCleanupStaleSessions();
