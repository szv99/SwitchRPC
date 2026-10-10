//
//  Discord.swift
//  SwitchRPC
//
//  Created by Lakhan Lothiyi on 30/07/2025.
//

enum DiscordConsts {
	static let ClientID: UInt = 1_249_119_754_522_857_616
	static let scope = "openid sdk.social_layer_presence"
}

import libnx

class Discord {
	init() { }
	var auth: AuthResponse?
	
	func me() throws(DiscordError) -> JSONValue {
		let auth = try checkAuth()
		
		var res: Response
		do {
			res = try Request(url: "https://gaming-sdk.com/api/v9/oauth2/@me")
				.settingMethod(.get)
				.addingHeader("Authorization", value: "Bearer \(auth.accessToken)")
				.perform()
		} catch {
			throw .requestError(error)
		}
		
		return .parse(from: res.data) ?? JSONValue.object([:])
	}

	struct MeUser {
		var id: String
		var username: String
	}

	/// The id and username of the account that just authenticated.
	func meUser() throws(DiscordError) -> MeUser {
		let user = try me()["user"]
		return MeUser(id: user["id"].string ?? "", username: user["username"].string ?? "")
	}

	// MARK: - Device authorization
	// The console asks Discord for a short code, the user approves it on their
	// phone at discord.com/activate, and the console polls until it gets the token.
	// Nothing has to reach the Switch, so no local server or redirect page.

	struct DeviceAuthorization {
		var deviceCode: String
		var userCode: String
		var verificationURL: String  // opens discord.com/activate with the code filled in
		var expiresIn: Int  // seconds
		var interval: Int  // seconds to wait between polls
	}

	enum DeviceTokenStatus {
		case pending
		case slowDown  // polling too fast, wait longer
		case authorized(AuthResponse)
	}

	func requestDeviceAuthorization() throws(DiscordError) -> DeviceAuthorization {
		let json = try postForm(
			"https://gaming-sdk.com/api/v9/oauth2/device/authorize",
			[
				"client_id": DiscordConsts.ClientID.description,
				"scope": DiscordConsts.scope,
			]
		)
		guard let deviceCode = json["device_code"].string,
			let userCode = json["user_code"].string,
			let url = json["verification_uri_complete"].string
		else {
			throw .authorizationFailed(json["error"].string ?? "unexpected response")
		}
		return DeviceAuthorization(
			deviceCode: deviceCode,
			userCode: userCode,
			verificationURL: url,
			expiresIn: json["expires_in"].int ?? 300,
			interval: json["interval"].int ?? 5
		)
	}

	func pollDeviceToken(_ authorization: DeviceAuthorization) throws(DiscordError) -> DeviceTokenStatus {
		let json = try postForm(
			"https://gaming-sdk.com/api/v9/oauth2/token",
			[
				"grant_type": "urn:ietf:params:oauth:grant-type:device_code",
				"device_code": authorization.deviceCode,
				"client_id": DiscordConsts.ClientID.description,
			]
		)

		switch json["error"].string {
		case nil: break
		case "authorization_pending"?: return .pending
		case "slow_down"?: return .slowDown
		case let error?: throw .authorizationFailed(error)
		}

		let auth: AuthResponse
		do {
			auth = try AuthResponse(json: json)
		} catch {
			throw .decodingError(error)
		}
		self.auth = auth
		return .authorized(auth)
	}

	/// POSTs a form and returns the JSON body, whatever the status code.
	private func postForm(_ url: String, _ form: [String: String]) throws(DiscordError) -> JSONValue {
		let res: Response
		do {
			res = try Request(url: url)
				.settingMethod(.post)
				.addingHeader("Content-Type", value: "application/x-www-form-urlencoded")
				.settingData(form.toQueryString())
				.settingTimeout(seconds: 10)  // keeps B responsive while polling
				.perform()
		} catch {
			throw .requestError(error)
		}
		// not JSON = something in between failed (e.g. a 502 page), treat it like a dropped request
		guard let json = JSONValue.parse(from: res.data) else {
			throw .requestError(.unknownError)
		}
		return json
	}

	struct AuthResponse: Decodable {
		var refreshToken: String
		var accessToken: String
		
		init(json: JSONValue) throws(DecodingError) {
			guard let refreshToken = json["refresh_token"].string else {
				throw .missingKey("refresh_token")
			}
			guard let accessToken = json["access_token"].string else {
				throw .missingKey("access_token")
			}
			self.refreshToken = refreshToken
			self.accessToken = accessToken
		}
	}
	
	// Helpers

	enum DiscordError: LocalizedError {
		case requestError(Request.RequestError)
		case decodingError(DecodingError)
		case authorizationFailed(String)  // OAuth2 error code from Discord
		case unauthenticated

		var errorDescription: String? {
			switch self {
			case .unauthenticated:
				return "You must authenticate first before making this request."
			case .authorizationFailed("access_denied"):
				return "The request was denied on Discord."
			case .authorizationFailed("expired_token"):
				return "The code expired, try again."
			case .authorizationFailed(let error):
				return "Discord returned an error: \(error)"
			case .requestError(let error):
				return error.errorDescription
			case .decodingError(let error):
				return error.errorDescription
			}
		}
	}
	
	func checkAuth() throws(DiscordError) -> AuthResponse {
		guard let auth = self.auth else {
			throw DiscordError.unauthenticated
		}
		if auth.accessToken.isEmpty || auth.refreshToken.isEmpty {
			throw DiscordError.unauthenticated
		}
		return auth
	}
}
