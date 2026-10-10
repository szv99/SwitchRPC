// discord.cpp

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <string>
#include <vector>
#include <utility>
#include <time.h>

#include <switch.h>
#include <json-c/json.h>
#include <curl/curl.h>

#include "discord.hpp"
#include "logging.hpp"
#include "utilities.hpp"

const char* clientId = "1249119754522857616";

#define PROFILES_PATH        "sdmc:/config/switchrpc_profiles.json"
#define LEGACY_TOKEN_PATH    "sdmc:/config/switchrpc_token"
#define SESSIONS_PATH        "sdmc:/config/switchrpc_sessions.json"
#define LEGACY_SESSIONS_PATH "sdmc:/config/switchrpc_sessions"

// where the links came from, so we can tell when the config app changes them
struct LinkSource {
    bool legacy = false;   // true = single token in switchrpc_token, no profiles.json yet
    std::string contents;

    bool operator==(const LinkSource& other) const {
        return legacy == other.legacy && contents == other.contents;
    }
};

static std::vector<Link> g_links;
static LinkSource g_linkSource;

// ------------------------------------------------------------------
// switch_uid strings
// ------------------------------------------------------------------

std::string accountUidToString(const AccountUid& uid) {
    char buf[34];
    snprintf(buf, sizeof(buf), "%016llx-%016llx",
             (unsigned long long)uid.uid[0], (unsigned long long)uid.uid[1]);
    return std::string(buf);
}

bool accountUidFromString(const std::string& s, AccountUid& out) {
    out = {};
    size_t dash = s.find('-');
    if (dash == 0 || dash == std::string::npos || dash + 1 >= s.size()) return false;
    out.uid[0] = strtoull(s.substr(0, dash).c_str(), NULL, 16);
    out.uid[1] = strtoull(s.substr(dash + 1).c_str(), NULL, 16);
    return true;
}

// ------------------------------------------------------------------
// links
// ------------------------------------------------------------------

static LinkSource readLinkSource() {
    LinkSource source;
    // once profiles.json exists it's the only source of truth, even if it's empty.
    // the config app migrates the legacy token based on the same rule.
    if (readFileContents(PROFILES_PATH, source.contents)) return source;
    if (readFileContents(LEGACY_TOKEN_PATH, source.contents)) source.legacy = true;
    return source;
}

static std::string jsonString(json_object* obj, const char* key) {
    json_object* value;
    if (!json_object_object_get_ex(obj, key, &value)) return "";
    const char* s = json_object_get_string(value);
    return s ? s : "";
}

static void parseLinks(const LinkSource& source) {
    g_links.clear();
    g_linkSource = source;

    if (source.legacy) {
        std::string token = source.contents;
        while (!token.empty() && strchr("\r\n\t ", token.back())) token.pop_back();
        if (token.empty()) return;

        Link link;
        link.switchUidStr = accountUidToString(link.switchUid);
        link.refreshToken = token;
        g_links.push_back(link);
        writeToLog("[Discord] Loaded legacy refresh token as the Default link.");
        return;
    }

    json_object* root = json_tokener_parse(source.contents.c_str());
    json_object* arr;
    if (root == NULL || !json_object_object_get_ex(root, "links", &arr) ||
        json_object_get_type(arr) != json_type_array) {
        if (!source.contents.empty()) writeToLog("[Discord] profiles.json is not valid, no links loaded.");
        json_object_put(root);
        return;
    }

    int n = json_object_array_length(arr);
    for (int i = 0; i < n; i++) {
        json_object* entry = json_object_array_get_idx(arr, i);
        Link link;
        link.switchUidStr = jsonString(entry, "switch_uid");
        link.discordId = jsonString(entry, "discord_id");
        link.discordUsername = jsonString(entry, "discord_username");
        link.refreshToken = jsonString(entry, "refresh_token");
        if (!accountUidFromString(link.switchUidStr, link.switchUid) || link.refreshToken.empty()) continue;
        g_links.push_back(link);
    }
    json_object_put(root);
    writeToLog("[Discord] Loaded %d link(s) from profiles.json", (int)g_links.size());
}

void loadLinks() {
    parseLinks(readLinkSource());
}

bool reloadLinksIfChanged() {
    LinkSource source = readLinkSource();
    if (source == g_linkSource) return false;

    writeToLog("[Discord] Links changed on disk, reloading.");
    std::vector<Link> previous;
    previous.swap(g_links);
    parseLinks(source);

    for (Link& old : previous) {
        Link* current = findLink(old.switchUidStr);
        if (current != nullptr && current->refreshToken == old.refreshToken) {
            current->revoked = old.revoked;
            current->authToken = old.authToken;
            current->authTokenExpiry = old.authTokenExpiry;
            current->sessionToken = old.sessionToken;
        } else if (!old.sessionToken.empty()) {
            // unlinked or relinked to another account, the session belongs to the old one
            discordDeleteHeadlessSession(old);
        }
    }
    return true;
}

Link* resolveLink(const AccountUid& uid) {
    Link* fallback = nullptr;
    for (Link& link : g_links) {
        if (link.switchUid.uid[0] == uid.uid[0] && link.switchUid.uid[1] == uid.uid[1]) return &link;
        if (link.switchUid.uid[0] == 0 && link.switchUid.uid[1] == 0) fallback = &link;
    }
    return fallback;
}

Link* findLink(const std::string& switchUidStr) {
    for (Link& link : g_links) {
        if (link.switchUidStr == switchUidStr) return &link;
    }
    return nullptr;
}

// Discord rotates the refresh token on every refresh, so the new one has to be
// saved or the link breaks on the next boot. Only overwrite the entry if it still
// holds the token we refreshed with - if the user unlinked or relinked the profile
// in the meantime, the file has the newer state and must win.
static void persistRotatedRefreshToken(const Link& link, const std::string& oldToken) {
    LinkSource source = readLinkSource();

    if (source.legacy) {
        std::string onDisk = source.contents;
        while (!onDisk.empty() && strchr("\r\n\t ", onDisk.back())) onDisk.pop_back();
        if (onDisk != oldToken) return;
        if (writeFileAtomic(LEGACY_TOKEN_PATH, link.refreshToken)) {
            g_linkSource.contents = link.refreshToken;
            writeToLog("[Discord] Saved rotated refresh token to legacy file.");
        }
        return;
    }

    json_object* root = json_tokener_parse(source.contents.c_str());
    json_object* arr;
    if (root == NULL || !json_object_object_get_ex(root, "links", &arr) ||
        json_object_get_type(arr) != json_type_array) {
        json_object_put(root);
        return;
    }

    bool updated = false;
    int n = json_object_array_length(arr);
    for (int i = 0; i < n; i++) {
        json_object* entry = json_object_array_get_idx(arr, i);
        if (jsonString(entry, "switch_uid") != link.switchUidStr) continue;
        if (jsonString(entry, "refresh_token") == oldToken) {
            json_object_object_add(entry, "refresh_token", json_object_new_string(link.refreshToken.c_str()));
            updated = true;
        }
        break;
    }

    if (updated) {
        std::string contents = json_object_to_json_string(root);
        if (writeFileAtomic(PROFILES_PATH, contents)) {
            // our own write, don't treat it as a config change
            g_linkSource.contents = contents;
            writeToLog("[Discord] Saved rotated refresh token for %s", link.switchUidStr.c_str());
        }
    } else {
        writeToLog("[Discord] %s was unlinked or relinked, not saving its rotated token.", link.switchUidStr.c_str());
    }
    json_object_put(root);
}

// ------------------------------------------------------------------
// sessions file
// ------------------------------------------------------------------

// switchrpc_sessions.json keeps one session token per switch_uid, so sessions left
// behind by a crash or power loss can be deleted on the next boot with the right
// account's credentials. An empty token removes the entry.
static void setSessionEntry(const std::string& uidStr, const std::string& token) {
    std::string contents;
    readFileContents(SESSIONS_PATH, contents);
    json_object* oldRoot = contents.empty() ? NULL : json_tokener_parse(contents.c_str());

    json_object* root = json_object_new_object();
    json_object* arr = json_object_new_array();
    json_object_object_add(root, "sessions", arr);

    json_object* oldArr;
    if (oldRoot != NULL && json_object_object_get_ex(oldRoot, "sessions", &oldArr) &&
        json_object_get_type(oldArr) == json_type_array) {
        int n = json_object_array_length(oldArr);
        for (int i = 0; i < n; i++) {
            json_object* entry = json_object_array_get_idx(oldArr, i);
            if (jsonString(entry, "switch_uid") == uidStr) continue;
            json_object_array_add(arr, json_object_get(entry));
        }
    }
    json_object_put(oldRoot);

    if (!token.empty()) {
        json_object* entry = json_object_new_object();
        json_object_object_add(entry, "switch_uid", json_object_new_string(uidStr.c_str()));
        json_object_object_add(entry, "token", json_object_new_string(token.c_str()));
        json_object_array_add(arr, entry);
    }

    if (json_object_array_length(arr) == 0) {
        removeFileAtomic(SESSIONS_PATH);
    } else {
        writeFileAtomic(SESSIONS_PATH, json_object_to_json_string(root));
    }
    json_object_put(root);
}

// ------------------------------------------------------------------
// requests
// ------------------------------------------------------------------

int curlDebugCallback(CURL *handle, curl_infotype type, char *data, size_t size, void *userp) {
    (void)handle; // Unused
    (void)userp;  // Unused

    const char* type_str = "";
    switch (type) {
        case CURLINFO_TEXT:         type_str = "Info"; break;
        case CURLINFO_HEADER_IN:    type_str = "Recv Header"; break;
        case CURLINFO_HEADER_OUT:   type_str = "Send Header"; break;
        default: return 0;
    }

    std::string text(data, size);

    while(!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
        text.pop_back();
    }

    if (!text.empty()) {
        writeToLog("[cURL %s] %s", type_str, text.c_str());
    }

    return 0;
}

// learns the console's clock offset from the Date header of every response
static size_t headerCallback(char* buffer, size_t size, size_t nitems, void* userdata) {
    (void)userdata;
    size_t totalSize = size * nitems;

    if (totalSize > 5 && strncasecmp(buffer, "Date:", 5) == 0) {
        std::string value(buffer + 5, totalSize - 5);
        while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) value.pop_back();
        time_t serverUtc = curl_getdate(value.c_str(), NULL);
        if (serverUtc != -1) syncTimeWithServer(serverUtc);
    }

    return totalSize;
}

// curl helper for easy requests to discord,
// specify url, method, headers, and body as needed and a pointer to a string to store the response in.
// takes ownership of headers. httpStatus, if given, receives the HTTP status (0 if there was no response).
bool sendRequest(const char* url, const char* method, struct curl_slist* headers, const char* body, std::string* response, long* httpStatus = nullptr) {
    // token requests and responses carry credentials, keep them out of the log on the sd card
    bool sensitive = strstr(url, "/oauth2/token") != NULL;

    writeToLog("[Discord] Sending %s request to %s", method, url);
    if (body != NULL && !sensitive) {
        writeToLog("[Discord] Request Body: %s", body);
    }

    CURL *curl = curl_easy_init();
    if(curl) {
        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
        curl_slist* reqHeaders = NULL;
        reqHeaders = curl_slist_append(reqHeaders, "Accept: application/json");
        // discord social sdk sends these
        reqHeaders = curl_slist_append(reqHeaders, "User-Agent: Discord Embedded/0.0.8");
        // x super props
        /*
        {
            "browser":"Discord Embedded",
            "browser_user_agent":"Discord Embedded/0.0.8",
            "browser_version":"0.0.8",
            "client_build_number":304683,
            "design_id":0,
            "os":"Unknown",
            "release_channel":"unknown"
        }
        */
        // basically static, hardcoding b64 string.
        reqHeaders = curl_slist_append(reqHeaders, "X-Super-Properties: eyJicm93c2VyIjoiRGlzY29yZCBFbWJlZGRlZCIsImJyb3dzZXJfdXNlcl9hZ2VudCI6IkRpc2NvcmQgRW1iZWRkZWQvMC4wLjgiLCJicm93c2VyX3ZlcnNpb24iOiIwLjAuOCIsImNsaWVudF9idWlsZF9udW1iZXIiOjMwNDY4MywiZGVzaWduX2lkIjowLCJvcyI6IlVua25vd24iLCJyZWxlYXNlX2NoYW5uZWwiOiJ1bmtub3duIn0K");
        // host
        if (strstr(url, "discord.com") != NULL) {
            reqHeaders = curl_slist_append(reqHeaders, "Host: discord.com");
        } else if (strstr(url, "gaming-sdk.com") != NULL) {
            reqHeaders = curl_slist_append(reqHeaders, "Host: gaming-sdk.com");
        }

        reqHeaders = curl_slist_append(reqHeaders, "Connection: close");

        // iterate through the provided headers and add them to the request, overriding any duplicates of the default headers we set above
        for (curl_slist* header = headers; header != NULL; header = header->next) {
            reqHeaders = curl_slist_append(reqHeaders, header->data);
        }
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, reqHeaders);
        if (body != NULL)
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
        // don't let a request hang forever - matters for the pre-sleep cleanup,
        // it shouldn't hold up the console going to sleep
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);

        curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, headerCallback);
        curl_easy_setopt(curl, CURLOPT_HEADERDATA, NULL);

        // temporary fix to failing dns resolution
        struct curl_slist *dns_cache = NULL;
        dns_cache = curl_slist_append(dns_cache, "discord.com:443:162.159.138.232");
        dns_cache = curl_slist_append(dns_cache, "discord.com:443:162.159.135.232");

        dns_cache = curl_slist_append(dns_cache, "gaming-sdk.com:443:104.18.41.99");
        dns_cache = curl_slist_append(dns_cache, "gaming-sdk.com:443:172.64.146.157");
        curl_easy_setopt(curl, CURLOPT_RESOLVE, dns_cache);

        curl_easy_setopt(curl, CURLOPT_VERBOSE, 0L);
        curl_easy_setopt(curl, CURLOPT_DEBUGFUNCTION, curlDebugCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, +[](char* ptr, size_t size, size_t nmemb, void* userdata) -> size_t {
            size_t total_size = size * nmemb;
            if (userdata != NULL) {
                std::string* resp = static_cast<std::string*>(userdata);
                resp->append(ptr, total_size);
            }
            return total_size;
        });
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, response);

        CURLcode res = curl_easy_perform(curl);
        if(res != CURLE_OK) {
            writeToLog("[Discord] cURL request failed: %s", curl_easy_strerror(res));
        } else if (response != NULL) {
            writeToLog("[Discord] cURL request successful. Response length: %zu", response->size());
            if (!sensitive) writeToLog("[Discord] Response Body: %s", response->c_str());
        } else {
            writeToLog("[Discord] cURL request successful. (No response expected)");
        }

        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        if (httpStatus != NULL) *httpStatus = http_code;
        bool success = (http_code >= 200 && http_code < 300);
        if (res == CURLE_OK && !success) {
            writeToLog("[Discord] Request failed with HTTP %ld", http_code);
        }

        curl_slist_free_all(reqHeaders);
        curl_slist_free_all(headers);
        curl_slist_free_all(dns_cache);
        curl_easy_cleanup(curl);
        return success;
    } else {
        writeToLog("[Discord] Failed to initialize cURL!");
        curl_slist_free_all(headers);
        return false;
    }
}

static bool refreshAuthTokenIfNeeded(Link& link) {
    if (link.revoked) return false;

    // reuse the token until 5 minutes before it expires
    if (!link.authToken.empty() && getRawNowSec() + 300 < link.authTokenExpiry) {
        return true;
    }

    writeToLog("[Discord] Refreshing auth token for %s...", link.switchUidStr.c_str());

    struct curl_slist* headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/x-www-form-urlencoded; charset=utf-8");
    std::string body = "grant_type=refresh_token&refresh_token=" + link.refreshToken + "&client_id=" + clientId;

    std::string response;
    if (!sendRequest("https://gaming-sdk.com/api/v9/oauth2/token", "POST", headers, body.c_str(), &response)) {
        // the token was revoked, or rotated by a refresh whose response got lost.
        // retrying can't fix that, so wait for the profile to be relinked.
        if (response.find("invalid_grant") != std::string::npos) {
            link.revoked = true;
            writeToLog("[Discord] Discord rejected the refresh token for %s, relink it in the config app.", link.switchUidStr.c_str());
        } else {
            writeToLog("[Discord] Auth token refresh failed for %s", link.switchUidStr.c_str());
        }
        return false;
    }

    json_object* json_response = json_tokener_parse(response.c_str());
    if (json_response == NULL) {
        writeToLog("[Discord] Failed to parse JSON response from token refresh!");
        return false;
    }

    std::string accessToken = jsonString(json_response, "access_token");
    std::string refreshToken = jsonString(json_response, "refresh_token");
    json_object* json_expires_in;
    int expiresIn = json_object_object_get_ex(json_response, "expires_in", &json_expires_in)
        ? json_object_get_int(json_expires_in) : 0;
    json_object_put(json_response);

    if (accessToken.empty()) {
        writeToLog("[Discord] Token refresh response had no access_token for %s", link.switchUidStr.c_str());
        return false;
    }
    link.authToken = accessToken;
    link.authTokenExpiry = getRawNowSec() + expiresIn;

    if (!refreshToken.empty() && refreshToken != link.refreshToken) {
        std::string oldToken = link.refreshToken;
        link.refreshToken = refreshToken;
        persistRotatedRefreshToken(link, oldToken);
    }
    return true;
}

// takes ownership of headers, like sendRequest
static bool authenticatedRequest(Link& link, const char* url, const char* method, struct curl_slist* headers, const char* body, std::string* response, long* httpStatus = nullptr) {
    if (!refreshAuthTokenIfNeeded(link)) {
        curl_slist_free_all(headers);
        return false;
    }

    std::string authHeader = "Authorization: Bearer " + link.authToken;
    headers = curl_slist_append(headers, authHeader.c_str());
    return sendRequest(url, method, headers, body, response, httpStatus);
}

static bool deleteSession(Link& link, const std::string& token) {
    json_object* json_body = json_object_new_object();
    json_object_object_add(json_body, "token", json_object_new_string(token.c_str()));

    struct curl_slist* headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json; charset=utf-8");

    bool ok = authenticatedRequest(link, "https://discord.com/api/v9/users/@me/headless-sessions/delete",
                                   "POST", headers, json_object_get_string(json_body), NULL);
    json_object_put(json_body);
    return ok;
}

// ------------------------------------------------------------------
// headless sessions
// ------------------------------------------------------------------

void discordCleanupStaleSessions() {
    // the old plaintext format has no account info, so those sessions can't be deleted
    remove(LEGACY_SESSIONS_PATH);

    std::string contents;
    if (!readFileContents(SESSIONS_PATH, contents)) return;

    // sessions that are still live or failed to delete stay in the file
    std::vector<std::pair<std::string, std::string>> keep;

    json_object* root = json_tokener_parse(contents.c_str());
    json_object* arr;
    if (root != NULL && json_object_object_get_ex(root, "sessions", &arr) &&
        json_object_get_type(arr) == json_type_array) {
        int n = json_object_array_length(arr);
        for (int i = 0; i < n; i++) {
            json_object* entry = json_object_array_get_idx(arr, i);
            std::string uidStr = jsonString(entry, "switch_uid");
            std::string token = jsonString(entry, "token");
            if (token.empty()) continue;

            Link* link = findLink(uidStr);
            if (link == nullptr) {
                writeToLog("[Discord] %s is no longer linked, can't delete its session.", uidStr.c_str());
                continue;
            }
            if (link->sessionToken == token) {
                keep.push_back({uidStr, token});
                continue;
            }

            writeToLog("[Discord] Deleting stale session for %s", uidStr.c_str());
            if (!deleteSession(*link, token)) keep.push_back({uidStr, token});
        }
    }
    json_object_put(root);

    removeFileAtomic(SESSIONS_PATH);
    for (auto& entry : keep) setSessionEntry(entry.first, entry.second);
}

bool discordCreateHeadlessSession(Link& link, u64 titleId, const std::string& titleName, u64 startRawSec, bool includeToken) {
    if (link.revoked) return false; // already logged when it happened

    // refresh first: right after boot that's the first request to discord, and
    // its Date header gives the clock offset needed for the start timestamp
    if (!refreshAuthTokenIfNeeded(link)) return false;

    writeToLog("[Discord] Creating/Updating session for TID: %016llX (%s) on %s",
               (unsigned long long)titleId, titleName.c_str(), link.switchUidStr.c_str());

    // tries to fetch eshop icon, else falls back to tinfoil media icon
    std::string iconUrl;
    if (!fetchEshopIconUrl(titleId, iconUrl)) {
        char titleIdStr[17] = {0};
        snprintf(titleIdStr, sizeof(titleIdStr), "%016llX", (unsigned long long)titleId);
        iconUrl = std::string("https://tinfoil.media/ti/") + titleIdStr + "/512/512/";
        writeToLog("[Discord] Falling back to tinfoil for icon: %s", iconUrl.c_str());
    }

    // make json body
    json_object* json_body = json_object_new_object();
    json_object* json_activities = json_object_new_array();
    json_object* json_activity = json_object_new_object();
    json_object_object_add(json_activity, "type", json_object_new_string("0"));
    json_object_object_add(json_activity, "flags", json_object_new_string("0"));
    json_object_object_add(json_activity, "application_id", json_object_new_string(clientId));
    json_object_object_add(json_activity, "platform", json_object_new_string("desktop"));
    json_object_object_add(json_activity, "name", json_object_new_string(titleName.c_str()));
    json_object_object_add(json_activity, "state", json_object_new_string("Nintendo Switch"));

    // elapsed timer: discord shows (now - start). we pass a start moved forward
    // to exclude sleep, so it counts real playtime. timestamps are in ms.
    if (startRawSec != 0) {
        json_object* json_timestamps = json_object_new_object();
        json_object_object_add(json_timestamps, "start", json_object_new_int64((int64_t)rawToRealTime(startRawSec) * 1000));
        json_object_object_add(json_activity, "timestamps", json_timestamps);
    }

    json_object* json_assets = json_object_new_object();
    json_object_object_add(json_assets, "large_image", json_object_new_string(iconUrl.c_str()));
    json_object_object_add(json_activity, "assets", json_assets);

    json_object_array_add(json_activities, json_activity);
    json_object_object_add(json_body, "activities", json_activities);

    if (includeToken && !link.sessionToken.empty()) {
        json_object_object_add(json_body, "token", json_object_new_string(link.sessionToken.c_str()));
    }

    struct curl_slist* headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json; charset=utf-8");

    std::string response;
    long status = 0;
    bool success = authenticatedRequest(link, "https://discord.com/api/v9/users/@me/headless-sessions", "POST",
                                        headers, json_object_get_string(json_body), &response, &status);
    json_object_put(json_body);

    if (!success) {
        writeToLog("[Discord] Failed to create/update headless session!");
        // a rejected update means the session expired, so start a fresh one. after a
        // timeout the update may have gone through, and a retry would leave two sessions.
        if (includeToken && status >= 400 && status < 500) {
            writeToLog("[Discord] Retrying headless session creation without session token...");
            return discordCreateHeadlessSession(link, titleId, titleName, startRawSec, false);
        }
        return false;
    }

    json_object* json_response = json_tokener_parse(response.c_str());
    std::string token = json_response ? jsonString(json_response, "token") : "";
    json_object_put(json_response);

    if (token.empty()) {
        writeToLog("[Discord] Headless session response did not contain a session token.");
        return false;
    }

    if (token != link.sessionToken) {
        link.sessionToken = token;
        setSessionEntry(link.switchUidStr, token);
    }
    writeToLog("[Discord] Session active for %s.", link.switchUidStr.c_str());
    return true;
}

void discordDeleteHeadlessSession(Link& link) {
    if (link.sessionToken.empty()) return;

    writeToLog("[Discord] Deleting headless session for %s...", link.switchUidStr.c_str());
    bool deleted = deleteSession(link, link.sessionToken);
    link.sessionToken = "";

    // if it failed (e.g. wifi already gone), the entry stays in the sessions
    // file and the next stale cleanup tries again
    if (deleted) setSessionEntry(link.switchUidStr, "");
}
