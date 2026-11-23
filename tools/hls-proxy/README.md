# LiveVita HLS bridge (local proxy)

A small, transparent local proxy that helps the PS Vita play Twitch streams by
doing the networking the Vita can't do well itself. It runs on a normal computer
or phone on the **same network** as the Vita.

It is the same idea as pointing Streamlink or mpv at a public HLS stream:

1. **TLS bridge** — the Vita talks plain HTTP to the proxy on your LAN; the proxy
   talks modern HTTPS (TLS 1.3) to Twitch's CDNs. The Vita never has to complete a
   modern TLS handshake.
2. **Codec filter** — it hides fMP4 / HEVC / AV1 variants and offers the Vita only
   the H.264 MPEG-TS it can hardware-decode.
3. **Transparent relay** — it rewrites only the URLs so segments come back through
   the proxy, and passes everything else through untouched: `#EXT-X-MEDIA-SEQUENCE`,
   `#EXT-X-DISCONTINUITY`, and the ad `#EXT-X-DATERANGE` tags all survive, so the
   player stays in sync and recovers across commercial breaks without stalling.

## It does NOT strip ads

This is a **transparent** relay by design. It does not remove ads, skip commercial
breaks, or fetch an "ad-free" feed, and it must not be modified to. Whatever Twitch
sends — a stitched ad or the silent "commercial break in progress" slate — is passed
straight through, so the streamer still earns their ad revenue. The only thing it
does with the ad tags is **report** them on `/status`, so the app can show a label or
mute the silent slate. It uses the same public playback endpoints the app already
uses and does not impersonate Twitch's first-party players.

## Run it

Requires Python 3.8+ (standard library only, no `pip install`).

```
python3 hls_proxy.py --port 8080
```

Find the computer's LAN IP (e.g. `192.168.1.20`). Then:

- Playlist for a channel: `http://192.168.1.20:8080/hls/<channel>.m3u8`
- Ad-break status (JSON): `http://192.168.1.20:8080/status?channel=<channel>`

### Test it before touching the Vita

Point VLC or mpv at the playlist URL — it should play the stream:

```
mpv "http://192.168.1.20:8080/hls/somechannel.m3u8"
```

`/status` returns, e.g.:

```json
{"ad_break": true, "ad_duration": 30.0, "updated": 1766450000.0}
```

## Pointing LiveVita at it

LiveVita builds its own playback URL in `tw_playback_url()` (does the token +
usher fetch, then picks a variant). To route through the proxy instead, fetch the
proxy's already-filtered master and let the existing variant picker run on it:

```c
// pseudocode inside tw_playback_url(), when a proxy is configured:
//   master URL  ->  http://<proxy-ip>:8080/hls/<login>.m3u8   (plain HTTP, no TLS)
// everything else is unchanged: parse the master, pick a variant (its URL already
// points back to the proxy), play. Segment fetches then go to the proxy over HTTP.
```

Because the proxy serves plain HTTP, the Vita never opens an HTTPS socket, and the
`/status` endpoint lets the app show the same "Ad break" indicator while a break
plays. Ask and this can be wired into the app as an optional setting
(`ux0:data/LiveVita/proxy.txt` holding `ip:port`).

## Scope / limits

- Public streams only. Subscriber-only or otherwise gated streams are out of scope.
- Keep the proxy on your own LAN. It serves unencrypted HTTP, which is fine locally
  but should not be exposed to the public internet.
- This is a helper tool, not required to use LiveVita — the app plays Twitch
  directly without it.
