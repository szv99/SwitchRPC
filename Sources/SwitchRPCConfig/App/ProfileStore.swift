//
//  ProfileStore.swift
//  SwitchRPC
//
//  Links between Switch user profiles and Discord accounts, stored in
//  sdmc:/config/switchrpc_profiles.json and shared with the sysmodule.
//

import devkitpro  // C stdio
import libnx

/// A Switch user profile linked to a Discord account.
struct Link {
	var switchUid: String  // "<16 hex>-<16 hex>", all zero for the Default link
	var switchNickname: String
	var discordId: String
	var discordUsername: String
	var refreshToken: String  // the sysmodule replaces it whenever Discord rotates it
}

/// A Switch user, as listed by the sysmodule in switchrpc_users.json.
struct SwitchUser {
	var uid: String
	var nickname: String
}

enum ProfileStore {
	static let profilesPath = "sdmc:/config/switchrpc_profiles.json"
	static let legacyTokenPath = "sdmc:/config/switchrpc_token"
	static let usersPath = "sdmc:/config/switchrpc_users.json"
	/// The Default link applies to every user that isn't linked on their own.
	static let defaultUid = "0000000000000000-0000000000000000"

	/// Same format as the sysmodule: "%016llx-%016llx".
	static func uidToString(_ uid: AccountUid) -> String {
		return hex16(uid.uid.0) + "-" + hex16(uid.uid.1)
	}

	private static func hex16(_ value: UInt64) -> String {
		let digits = Array("0123456789abcdef".utf8)
		var out = [UInt8](repeating: 0, count: 17)  // 16 digits + null terminator
		for i in 0..<16 {
			out[15 - i] = digits[Int((value >> (UInt64(i) * 4)) & 0xF)]
		}
		return String(cStr: out)
	}

	// MARK: - Files

	static func fileExists(_ path: String) -> Bool {
		guard let file = fopen(path, "r") else { return false }
		fclose(file)
		return true
	}

	static func readFile(_ path: String) -> String? {
		// writeFile briefly has only the .tmp file between its remove and rename
		guard let file = fopen(path, "rb") ?? fopen(path + ".tmp", "rb") else { return nil }
		defer { fclose(file) }
		var data = [UInt8]()
		var chunk = [UInt8](repeating: 0, count: 4096)
		while true {
			let n = fread(&chunk, 1, chunk.count, file)
			if n <= 0 { break }
			data.append(contentsOf: chunk[0..<n])
		}
		if data.isEmpty { return nil }
		data.append(0)
		return String(cStr: data)
	}

	/// Writes through a temp file and a rename, like the sysmodule, so neither
	/// side ever reads a half written file.
	@discardableResult
	static func writeFile(_ path: String, _ content: String) -> Bool {
		let tmp = path + ".tmp"
		guard let file = fopen(tmp, "wb") else { return false }
		let written = fputs(content, file) >= 0
		let closed = fclose(file) == 0
		guard written && closed else {
			remove(tmp)
			return false
		}
		remove(path)  // rename can't replace an existing file on the sd card
		return rename(tmp, path) == 0
	}

	// MARK: - Load / Save

	static func loadSwitchUsers() -> [SwitchUser] {
		guard let content = readFile(usersPath),
			let users = JSONValue.parse(from: content)?["users"].array
		else { return [] }

		return users.compactMap { item in
			guard let uid = item["uid"].string, !uid.isEmpty else { return nil }
			return SwitchUser(uid: uid, nickname: item["nickname"].string ?? "Unknown")
		}
	}

	static func load() -> [Link] {
		loadIfValid() ?? []
	}

	/// nil if the file exists but can't be parsed, so an update doesn't replace it with nothing.
	private static func loadIfValid() -> [Link]? {
		guard let content = readFile(profilesPath) else { return [] }
		guard let links = JSONValue.parse(from: content)?["links"].array else { return nil }

		return links.compactMap { item in
			guard let uid = item["switch_uid"].string, !uid.isEmpty else { return nil }
			return Link(
				switchUid: uid,
				switchNickname: item["switch_nickname"].string ?? "",
				discordId: item["discord_id"].string ?? "",
				discordUsername: item["discord_username"].string ?? "",
				refreshToken: item["refresh_token"].string ?? ""
			)
		}
	}

	@discardableResult
	static func save(_ links: [Link]) -> Bool {
		let entries: [JSONValue] = links.map { link in
			JSONValue([
				"switch_uid": JSONValue(link.switchUid),
				"switch_nickname": JSONValue(link.switchNickname),
				"discord_id": JSONValue(link.discordId),
				"discord_username": JSONValue(link.discordUsername),
				"refresh_token": JSONValue(link.refreshToken),
			])
		}
		let root = JSONValue([
			"version": JSONValue(1),
			"links": JSONValue(entries),
		])
		guard let json = root.stringify() else { return false }
		return writeFile(profilesPath, json)
	}

	/// Adds the link, replacing any existing one for the same Switch user.
	static func upsert(_ link: Link) -> Bool {
		guard var links = loadIfValid() else { return false }
		if let index = links.firstIndex(where: { $0.switchUid == link.switchUid }) {
			links[index] = link
		} else {
			links.append(link)
		}
		return save(links)
	}

	/// Not named `remove` so it doesn't shadow C's remove(path).
	static func removeLink(switchUid: String) -> Bool {
		guard var links = loadIfValid() else { return false }
		links.removeAll { $0.switchUid == switchUid }
		return save(links)
	}

	// MARK: - Legacy migration

	/// Older versions stored a single refresh token in switchrpc_token. Turn it into
	/// the Default link, so the existing account keeps working for every user.
	static func migrateLegacyIfNeeded() {
		if fileExists(profilesPath) { return }
		guard let file = fopen(legacyTokenPath, "r") else { return }
		var buffer = [CChar](repeating: 0, count: 1024)
		fgets(&buffer, Int32(buffer.count), file)
		fclose(file)

		var token = String(cStr: buffer)
		while let last = token.last, last == "\n" || last == "\r" || last == " " || last == "\t" {
			token.removeLast()
		}
		if token.isEmpty { return }

		let link = Link(
			switchUid: defaultUid,
			switchNickname: "Default",
			discordId: "",
			discordUsername: "",
			refreshToken: token
		)
		// only drop the old file once the new one is safely written
		if save([link]) {
			remove(legacyTokenPath)
		}
	}
}
