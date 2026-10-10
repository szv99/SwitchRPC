# SwitchRPC
Discord Rich Presence for the Nintendo Switch

Does not rely on anything other than your Switch and a mobile device for logging in. Runs in the background and keeps your rich presence up to date with whatever title is open.

The rich presence is handled on the Switch itself, without the need for external tools on other computers.

## Profiles

Every Switch user profile can be linked to its own Discord account. Presence goes to the account of whoever is playing, and moves over when a different user starts a game.

There's also a Default link, used for any profile that isn't linked on its own. Leave it empty and unlinked profiles don't show any presence. Upgrading from an older version turns your existing login into the Default link, so nothing changes until you link profiles individually.

In the config app, pick a profile and press A to link it (or relink it to another account), X to unlink it. Changes apply right away, no reboot needed.

> [!TIP]
> Like this project? Own a Mac for developing? You might like [XRPC, Xcode Rich Presence done right](https://github.com/llsc12/XRPC).

## Installing
Head over to [the releases](https://github.com/szv99/SwitchRPC/releases/latest) and follow the instructions to download the latest version.

## Nerd details

Relies on Nintendo's CDN for game artworks. Uses Discord Social SDK endpoints for auth and its scope allow pushing presence data.

Written in Embedded Swift!

## Privacy

Linking uses Discord's device authorization flow, the same one consoles use. The config app asks Discord for a short code and shows it as a QR code (generated on the Switch) for `discord.com/activate`. Scan it with your phone, or type the code there on any device, and approve. Meanwhile the Switch polls Discord and receives the tokens once you've approved.

Only the Switch, your phone and Discord are involved, there's no third party page, and your phone doesn't need to be on the same network.

Refresh tokens are stored per profile in `sdmc:/config/switchrpc_profiles.json` and are kept out of the sysmodule's log.

## Credits

- libnx innit
- also using libcurl libjson-c ykyk
- switchpresence-rewritten with the process checking
- eembedded swift is rly cool man
- ty toby you made me actually think about the rpc process lol

# Retrospect
The application is written in Swift with several swifty abstractions over lots of C stuff. It'd be nice to turn the horrific makefile into a swiftpm plugin!<br>
The sysmodule was written in Swift originally but rewritten in C++ because I was trying to fix a crashing bug, however it was just an issue with stack vs heap allocations. 

## Risks

Should be minimal probably, I use API routes from the Discord Social SDK. Their domain is `gaming-sdk.com` :sob: anyways it also uses discord headless sessions.
Headers n stuff are in place.
