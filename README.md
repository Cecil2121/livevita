# LiveVita

An unofficial Twitch viewer for the PS Vita and PS TV (homebrew: HENkaku / Ensō / h-encore).

> ### 🤖 Vibe coded
> This project was **vibe coded** — written end-to-end in conversation with an AI assistant (Claude), from prompts rather than hand-written line by line. It has been compile-checked against the real VitaSDK headers and its logic unit-tested on a PC, but it has **not** been run on real Vita hardware by the author. Treat it as a starting point: read it, test it, and fix what breaks. No warranty. See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for reused components.

- **Watch live streams** with the Vita's hardware H.264 decoder at **720p60**, 720p30, 480p30 or 360p30, or audio only — plus an **experimental 1080p60** option for channels that offer source quality (see the note below). LiveVita asks Twitch for plain MPEG-TS HLS streams and also plays fragmented-MP4 HLS if Twitch sends that instead.
- **Live chat:** read any channel's chat right away, even signed out of chat. It shows name colors, badges (broadcaster, mod, VIP, sub...), timestamps and emotes — including **third-party emotes (BTTV and FFZ)**, the ones people actually type. Send messages with the on-screen keyboard. The last 100 messages are kept in memory.
- **Theater mode:** video on the left (70%) and chat on the right (30%). Square switches to full screen.
- **Following, Featured (top live), Games (browse by category) and Search.** Cards show preview thumbnails, LIVE and viewer counts. The player shows the title, category, viewers and uptime.
- **Two layouts, picked automatically:**
  - **Handheld (PS Vita), in the style of the classic Vita app:** the first screen has big purple menu buttons on the left and a mosaic on the right.
    - **Signed in, "Followed Channels":** Profile, Search, Following (with how many are live), Games and Channels. The mosaic shows your live followed channels: one big tile, smaller ones, and "..." for all of them.
    - **Signed out, "Top Channels":** Search, Games, Channels and Log In. Log In shows a code to enter at twitch.tv/activate. Twitch's official API only lists channels for signed-in users, so the tiles fill in once you log in. Channel and Games pages scroll sideways, with the name and "playing <game>" over each picture. Touch and swipes work everywhere.
    - **Following:** "Followed Channels" in one sliding row, with the selected channel bigger and framed. **START** opens Sorting Options: **Live** (newest streams first), **Offline** (followed channels that aren't live, A-Z, with their profile pictures) and **View Count** (most viewers first).
    - **Log In:** a two-step screen (go to twitch.tv/activate, enter the code shown on the right).
    - **Games:** box art in two rows. Picking one opens **"Channels Playing <game>"**, with big channel tiles and a Follow button.
    - **Player (full screen):** the title and "playing <game> for N viewers" at the top. The film icon (Left) opens **Highlights & Past Broadcasts**, which you can play. The heart (Right) shows whether you follow the channel, or asks you to log in. The **i** (Down) opens the channel panel with its picture, description, followers and viewers; its gear opens the quality menu.
    - **Following:** Twitch shut down following and unfollowing for third-party apps in 2021. The heart and the Follow button show your follow status and point you to twitch.tv or the Twitch app to change it.
  - **TV (PS TV, or a Vita with a DualShock connected), in the style of today's Twitch TV apps:** a dark look with tabs across the top (Home, Browse, Search, Profile).
    - **Home:** the top third previews whatever you highlight (a large picture of the stream, with name, title, game, viewers and uptime). Below it are rows that scroll sideways: Live Channels You Follow, Top Live Channels and Top Categories. Up/Down moves between rows.
    - **Browse:** a grid of categories. Picking one shows its live channels under the same preview.
    - **Search:** a search box and a grid of results. **Profile:** your account, Channels You Follow, Categories You Follow and settings.
    - **Player (full screen):** **SELECT** opens the channel panel — the channel's picture, description, follower count, live viewers, uptime and category, with **Follow**, **Clips**, **Stats** and **Close** buttons. **Clips** opens a grid of the channel's top clips (with duration and view counts) that you can play.
  - Override in **Settings → Layout** (Auto / Handheld / TV).
- **Sign in with a code.** The Vita shows a code; you enter it at twitch.tv/activate on your phone or PC.

Not affiliated with Twitch. Streams play as Twitch serves them, ads included.

### Commercial breaks (ads)

LiveVita connects straight to Twitch and plays each stream exactly as Twitch sends it. During a commercial break Twitch splices the ad segments into the same HLS stream, so the Vita simply plays them, then the live feed resumes on its own — the app doesn't need to do anything, and there's nothing to "skip."

LiveVita does **not** implement any ad-removal proxy. The ad-free setups you may have seen on other clients route the playlist request through an external HTTPS proxy (a server that fetches the stream on your behalf from a region or path without mid-rolls and hands back a cleaned playlist). That is a separate service, not part of this app: it means trusting a third-party server with your requests, it breaks Twitch's Terms of Service, and it takes ad revenue away from the streamers you're watching. LiveVita deliberately leaves it out and talks to Twitch directly. If you ever point the app at such a proxy yourself, that's entirely your own call and outside what this project does.

## 1. Get a Twitch Client ID (2 minutes, free)

1. Go to <https://dev.twitch.tv/console> and sign in. Twitch asks you to turn on two-factor authentication for developer apps.
2. Click **Register Your Application** and fill in:
   - **Name:** anything unique, for example `LiveVita-yourname`
   - **OAuth Redirect URLs:** `http://localhost`
   - **Category:** Application Integration
   - **Client Type:** **Public**. This matters: sign-in with a code needs a public client.
3. Click **Create**, then **Manage**, and copy the **Client ID**. You don't need a client secret.

## 2. Build the VPK

### Option A: on your Windows PC with Build-VPK.bat

1. Unzip the LiveVita folder somewhere, for example on your Desktop.
2. Double-click **Build-VPK.bat**.
   - The first time, it offers to install WSL, Microsoft's free Ubuntu-inside-Windows. Say yes, restart your PC, set a Linux username and password when Ubuntu opens, then run the .bat again.
   - Paste your Client ID when it asks (right-click pastes). It's remembered for next time.
   - The first build also downloads the Vita build tools (10-20 minutes). Later builds take 1-2 minutes.
3. When it finishes, File Explorer opens with **LiveVita.vpk** selected.

### Option B: on GitHub (nothing to install)

1. Create a free account at <https://github.com> and a **New repository**.
2. Upload this whole folder, including the `.github` folder. On a Mac, press Cmd+Shift+. in Finder to see it.
3. Optional: **Settings → Secrets and variables → Actions → Variables → New repository variable**, with name `TWITCH_CLIENT_ID` and your Client ID as the value.
4. **Actions → Build VPK → Run workflow**. After about 5 minutes, download **LiveVita-vpk** from the finished run.

## 3. Install and run

1. Copy `LiveVita.vpk` to your Vita (VitaShell over USB or FTP) and install it from VitaShell.
2. If the Client ID isn't built in, create `ux0:data/LiveVita/client_id.txt` containing only the Client ID.
3. Start LiveVita, select **Log In**, and enter the code it shows at twitch.tv/activate.

**Updating from version 1:** sign out and back in once (Settings → Account) so Twitch adds the chat permissions. Until you do, chat works in reading-only mode.

## Controls

**Home (handheld layout)**

| Button | Action |
|---|---|
| D-pad / left stick | Move between tiles. Right past the mosaic slides on to **Games** and then **Top Channels**; Left at the start goes to the menu |
| L / R | Jump to the previous / next section of the home screen |
| X or tap | Open the menu entry / watch the channel / open the category |
| O | Back (to the first screen) |
| Square | New search (Search page) |
| Triangle | Refresh |
| Swipe left/right | Slide the home screen or scroll a page |
| START | Options |

**Home (TV layout):** the same, plus Left on the first column (or O) opens the sidebar. Up/Down picks a section, and Right/X goes back to the grid.

**Player**

| Button | Action |
|---|---|
| O | Back |
| Square | Theater mode (video + chat) ⇄ full screen |
| Triangle (or tap the chat box) | Type a chat message |
| Up / Down (or drag the chat) | Scroll chat back / return to live |
| L / R | Lower / higher quality |
| START | Quality menu (1080p60 experimental, 720p60, 720p30, 480p30, 360p30, Audio only) |
| SELECT | Playback stats |

## Profile and followed categories

- **Profile** (the handheld menu button, or the TV Profile tab) shows your picture, name, bio, followers, how many channels you follow and when you joined, your followed channels (saved on the Vita, with their profile pictures) and your followed categories. START opens Options (the settings); O on Options returns to your profile.
- **Following categories:** on a category's page, press **Follow** (handheld) or **Square** (TV). Twitch has no public way for apps to follow categories on your account, so LiveVita saves them on your Vita (`ux0:data/LiveVita/categories.txt`). Followed categories appear first in Games with a "Following" tag, in their own row on the TV Home screen, and on your profile.

## Following channels on the Vita

Twitch removed the ability for apps to follow channels on your account in 2021, so the heart in the player offers **Follow on this Vita** instead. Those channels are saved in `ux0:data/LiveVita/channels.txt` and show up in your Following lists (live and offline) and on the TV Home screen with a "Saved" tag, next to the channels you follow on Twitch. They aren't added to your Twitch account. If you already follow a channel on Twitch, the heart says so.

## Settings

- **Video quality:** 720p60, 720p30, 480p30 (default), 360p30 or Audio only, set in Options or from the in-player quality menu (START). 60 fps is only used when you pick a 60 fps option.
- **Experimental 1080p60:** source quality, for channels that broadcast it. It uses the Vita's HD decoder path (1920×1088). It's marked experimental because the Vita can drop frames decoding 1080p60 in real time, and some channels' 1080p segments are refused by Twitch on the device (you'll see segment errors) while 720p plays fine. If 1080p is rough, drop to 720p60 or 720p30 — L on the pad lowers the quality on the fly.
- **Layout:** Auto (PS TV or a DualShock → TV layout, otherwise Handheld), Handheld or TV.
- **Chat:** On / Off.
- **Account:** sign out.

## Troubleshooting

- **Stutters:** choose 360p30 or 480p30, and stay close to your router.
- **"This channel only streams above 720p":** the channel has no smaller versions (Twitch only adds them for some channels). The Vita can't decode 1080p. Try Audio only.
- **Chat says "reading only":** sign out and back in once to grant the chat permission.
- **"Getting a sign-in code" fails:** check the Client ID, and that the Twitch app's Client Type is **Public**.
- **Anything else:** LiveVita writes a log to `ux0:data/LiveVita/log.txt`, and the previous run's log to `log_prev.txt`.
- **Subscriber-only streams** don't play. The player fetches streams the way a logged-out viewer would.

## How it works

- `src/twitch.c`: sign-in and lists through Twitch's official API (Helix), using the device code flow. For video it gets a playback token the way Twitch's website player does (the official API has no call for this, so Twitch could change it). Then it reads the HLS master playlist and picks an H.264 variant for the chosen quality (up to an experimental 1080p60), or audio only.
- `src/chat.c`: an IRC client for `irc.chat.twitch.tv:6697` over TLS on its own thread. It uses IRCv3 tags, reconnects automatically, keeps a 100-message ring buffer, and reads anonymously (`justinfanNNNNN`) when not signed in.
- `src/imgcache.c`: downloads thumbnails, box art and emotes on a background thread. Textures are made and freed on the UI thread, with a least-recently-used limit of about 10 MB.
- `src/home.c`, `src/playerui.c`, `src/ui.c`: the two layouts, the player and chat panel, input (buttons, DualShock ports, touch) and console detection (`sceKernelIsPSVitaTV`, `sceCtrlGetControllerPortInfo`).
- `src/player/`: the video player engine from [Vita IPTV](https://github.com/emrhky/VitaIPTV) (MIT License). It handles HLS and MPEG-TS, the Vita's hardware H.264 decoder and modern TLS. LiveVita adds fragmented-MP4 HLS support. See `THIRD_PARTY_NOTICES.md`.

**Security note:** like Vita IPTV, LiveVita encrypts all connections with its own TLS but does not check server certificates, because the Vita has no up-to-date root certificates. Use it on networks you trust.

## License

MIT (see `LICENSE`). Third-party parts are under their own licenses (see `THIRD_PARTY_NOTICES.md`).
