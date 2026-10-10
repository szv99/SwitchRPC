import curl
import json
import libnx

let sysmoduleTitleId: UInt64 = 0x3120000000000092

@main
struct App: NXApp {
	nonisolated(unsafe) static let discord = Discord()

	var menu: Menu = Menu()

	mutating func runloop(_ ctx: inout RunLoopContext) {  // runs in a loop, calling ctx.exit() ends the app
		ctx.pad.update()
		menu.setupIfNeeded()
		menu.tick(&ctx)

		if menu.wasInvalidated {
			consoleClear()
			menu.draw()
			consoleUpdate(nil)  // push new frame
			menu.wasInvalidated = false
		}

		Thread.sleep(for: .milliseconds(10))
	}

	func modulesInit(_ ctx: inout RunLoopContext) {
		consoleInit(nil)
		socketInitializeDefault()
		Request.curlInit()
		pmshellInitialize()
		pmdmntInitialize()
		pminfoInitialize()
		nsInitialize()
		smInitialize()
		accountInitialize(AccountServiceType_Application)
		setsysInitialize()
		fsdevMountSdmc()

		ctx.pad.configure(1, .standard)
	}
	func modulesExit(_ ctx: inout RunLoopContext) {
		fsdevUnmountAll()
		fsExit()
		setsysExit()
		accountExit()
		smExit()
		nsExit()
		pminfoExit()
		pmdmntExit()
		pmshellExit()
		Request.curlCleanup()
		socketExit()
		consoleExit(nil)
	}
}

/// A row in the menu: a Switch user, or the Default link used for users without their own.
struct ProfileEntry {
	var uid: String
	var nickname: String
}

struct Menu {
	var wasInvalidated = true  // redraw flag

	// Menu state variables
	var selectedIndex = 0
	var didSetup = false

	var entries: [ProfileEntry] = []
	var links: [Link] = []

	var logs: [String] = [] {
		didSet {
			// limit to the last 10 logs
			if logs.count > 10 {
				logs.removeFirst(logs.count - 10)
			}
		}
	}

	// entries plus the trailing "Exit app" row
	var rowCount: Int { entries.count + 1 }

	mutating func setupIfNeeded() {
		if didSetup { return }
		didSetup = true
		ProfileStore.migrateLegacyIfNeeded()
		reload()
	}

	mutating func reload() {
		links = ProfileStore.load()
		entries = Menu.loadProfiles()
		if selectedIndex >= rowCount { selectedIndex = 0 }
		wasInvalidated = true
	}

	/// The Switch users, then the Default entry.
	static func loadProfiles() -> [ProfileEntry] {
		// the account service isn't usable when the app runs as an applet (from the
		// album), so the sysmodule writes the user list to the sd card for us
		var result = ProfileStore.loadSwitchUsers().map {
			ProfileEntry(uid: $0.uid, nickname: $0.nickname)
		}

		// no list yet (sysmodule not running), ask the account service directly,
		// which works when launched as an application
		if result.isEmpty {
			var uids = [AccountUid](repeating: AccountUid(), count: Int(ACC_USER_LIST_SIZE))
			var total: Int32 = 0
			if accountListAllUsers(&uids, Int32(uids.count), &total).succeeded {
				for uid in uids.prefix(Int(total)) {
					result.append(
						ProfileEntry(uid: ProfileStore.uidToString(uid), nickname: nickname(of: uid) ?? "Unknown")
					)
				}
			}
		}

		result.append(ProfileEntry(uid: ProfileStore.defaultUid, nickname: "Default (unlinked users)"))
		return result
	}

	static func nickname(of uid: AccountUid) -> String? {
		var profile = AccountProfile()
		guard accountGetProfile(&profile, uid).succeeded else { return nil }
		defer { accountProfileClose(&profile) }
		var base = AccountProfileBase()
		guard accountProfileGet(&profile, nil, &base).succeeded else { return nil }
		// nickname is a fixed size char array, not always null terminated
		let bytes = withUnsafeBytes(of: base.nickname) { raw in Array(raw.prefix { $0 != 0 }) }
		let nickname = String(cStr: bytes + [0])
		return nickname.isEmpty ? nil : nickname
	}

	func link(for uid: String) -> Link? {
		links.first { $0.switchUid == uid }
	}

	mutating func tick(_ ctx: inout RunLoopContext) {
		// Handle exit request
		if ctx.pad.wasButtonPressed(.plus) {
			ctx.exit()
			return
		}

		// Move selection
		if ctx.pad.wasButtonPressed(.anyDown) {
			self.wasInvalidated = true
			selectedIndex = (selectedIndex + 1) % rowCount
		}
		if ctx.pad.wasButtonPressed(.anyUp) {
			self.wasInvalidated = true
			selectedIndex = (selectedIndex - 1 + rowCount) % rowCount
		}

		// A: link or relink, or exit on the last row
		if ctx.pad.wasButtonPressed(.a) {
			self.wasInvalidated = true
			if selectedIndex < entries.count {
				pair(entries[selectedIndex], &ctx)
			} else {
				ctx.exit()
			}
		}

		// X: unlink
		if ctx.pad.wasButtonPressed(.x), selectedIndex < entries.count {
			let entry = entries[selectedIndex]
			if link(for: entry.uid) != nil {
				let removed = ProfileStore.removeLink(switchUid: entry.uid)
				log(removed ? "Unlinked \(entry.nickname)" : "Couldn't update the profiles file on the SD card.")
				reload()
			}
		}
	}

	mutating func pair(_ entry: ProfileEntry, _ ctx: inout RunLoopContext) {
		log("Asking Discord for a code...", rd: true)
		consoleUpdate(nil)

		let device: Discord.DeviceAuthorization
		do {
			device = try App.discord.requestDeviceAuthorization()
		} catch {
			log("Failed: \(error.errorDescription ?? "Unknown error")")
			return
		}

		consoleClear()
		print("\nLink \(entry.nickname) to Discord\n")
		if let qr = QRCode.encode(device.verificationURL) {
			// the console is 45 rows, leave 6 for the text
			QRCode.render(qr, quietZone: max(1, min(4, (39 - qr.count) / 2)))
		}
		print("\nScan the QR code with your phone, or go to discord.com/activate")
		print("on any device and enter \(device.userCode). Press B to cancel.")
		consoleUpdate(nil)

		// wall clock, the polls themselves take a while
		let start = armGetSystemTick()
		func elapsedMs() -> Int { Int(armTicksToNs(armGetSystemTick() - start) / 1_000_000) }

		var auth: Discord.AuthResponse?
		var intervalMs = max(device.interval, 1) * 1000
		var nextPollMs = intervalMs
		while auth == nil {
			ctx.pad.update()
			if ctx.pad.wasButtonPressed(.b) {
				log("Linking cancelled.")
				reload()
				return
			}
			if elapsedMs() >= device.expiresIn * 1000 {
				log("The code expired, try again.")
				reload()
				return
			}

			if elapsedMs() >= nextPollMs {
				do {
					switch try App.discord.pollDeviceToken(device) {
					case .pending: break
					case .slowDown: intervalMs += 5000
					case .authorized(let token): auth = token
					}
				} catch {
					// a dropped request is retried on the next poll, anything else ends it
					guard case .requestError = error else {
						log("Failed: \(error.errorDescription ?? "Unknown error")")
						reload()
						return
					}
				}
				nextPollMs = elapsedMs() + intervalMs
			}

			Thread.sleep(for: .milliseconds(16))
		}

		guard let auth else { return }
		log("Approved, fetching your Discord account...", rd: true)
		consoleUpdate(nil)

		do {
			let user = try App.discord.meUser()
			let username = user.username.isEmpty ? "Unknown User" : user.username
			let saved = ProfileStore.upsert(
				Link(
					switchUid: entry.uid,
					switchNickname: entry.nickname,
					discordId: user.id,
					discordUsername: username,
					refreshToken: auth.refreshToken
				)
			)
			log(saved ? "Linked \(entry.nickname) to \(username)!" : "Couldn't save the link to the SD card.")
		} catch {
			log("Failed: \(error.errorDescription ?? "Unknown error")")
		}
		reload()
	}

	mutating func log(_ message: String, rd: Bool = false) {
		self.logs.append(message)
		if rd { self.forceRedraw() }
	}
}

extension Menu {
	mutating func draw() {
		print("\nSwitchRPC\n")
		print("Pick a profile and press A to link it to a Discord account.\n")

		for (i, entry) in entries.enumerated() {
			let selector = (i == selectedIndex) ? "-> " : "   "
			let status: String
			if let link = link(for: entry.uid) {
				status = link.discordUsername.isEmpty ? "linked" : link.discordUsername
			} else {
				status = "not linked"
			}
			print("  \(selector)\(entry.nickname)  [\(status)]")
		}
		let selector = (selectedIndex == entries.count) ? "-> " : "   "
		print("  \(selector)Exit app")

		print("\n")
		print("A: link/relink    X: unlink    +: exit")
		print("\nBy llsc12\n")
		print("[ Logs ]\n")
		for log in self.logs {
			print(log)
		}
	}

	mutating func forceRedraw() {
		consoleClear()
		self.draw()
	}
}
