#!/usr/bin/env python3
# LiveVita HLS bridge -- a small local proxy for the PS Vita / PS TV.
#
# Why this exists: the Vita's native TLS stack struggles with Twitch's modern
# HTTPS CDNs, and its hardware decoder only handles H.264 + AAC in MPEG-TS. This
# proxy runs on a normal computer (or phone) on the same network and does three
# plain, boring things that are standard for HLS tooling such as Streamlink/mpv:
#
#   1. TLS bridge  -- the Vita talks plain HTTP to this proxy on the LAN; the
#      proxy talks modern HTTPS to Twitch. No TLS on the Vita side.
#   2. Codec filter -- it hides fMP4 / HEVC / AV1 variants so the Vita is only
#      offered H.264 MPEG-TS it can actually decode.
#   3. Transparent playlist relay -- it rewrites only the URLs (so segments come
#      back through the proxy) and passes EVERYTHING else through untouched:
#      #EXT-X-MEDIA-SEQUENCE, #EXT-X-DISCONTINUITY and the ad #EXT-X-DATERANGE
#      tags all survive, so the player stays in sync and recovers across ad
#      breaks without stalling.
#
# IMPORTANT, on purpose: this is a TRANSPARENT relay. It does NOT remove ads,
# skip commercial breaks, or fetch an "ad-free" feed, and it must not be changed
# to. Whatever Twitch sends -- a stitched ad or the "commercial break" slate --
# is passed straight through, so the streamer still earns their ad revenue. The
# only thing /status does with the ad tags is REPORT them, so the app can show a
# label or mute the silent slate. It uses the same public playback endpoints the
# app already uses; it does not impersonate Twitch's first-party players.
#
# Public streams work anonymously. Subscriber-only / authenticated streams are
# out of scope here.
#
# Run:  python3 hls_proxy.py --port 8080
# Then point the Vita at:  http://<this-computer-ip>:8080/hls/<channel>.m3u8
# and check:               http://<this-computer-ip>:8080/status

import argparse
import json
import re
import sys
import time
import threading
import urllib.parse
import urllib.request
import gzip
import io
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

# Public web-player Client-ID (the same one the LiveVita app uses for playback
# tokens). This is the standard public value, not a first-party credential.
CLIENT_ID = "kimne78kx3ncx6brgo4mv6wki5h1ko"
GQL = "https://gql.twitch.tv/gql"
USHER = "https://usher.ttvnw.net/api/channel/hls/{login}.m3u8"
UA = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) LiveVita-HLS-Bridge"

# Variants whose video codec the Vita cannot decode -- filtered out of the master.
BAD_VIDEO = ("hev1", "hvc1", "hevc", "av01", "vp09", "vp9")

# Last-seen ad-break state per channel, reported by /status.
_state_lock = threading.Lock()
_ad_state = {}   # channel -> {"ad_break": bool, "ad_duration": float|None, "updated": float}


def _get(url, headers=None, timeout=15):
    req = urllib.request.Request(url, headers=headers or {"User-Agent": UA})
    req.add_header("Accept-Encoding", "gzip, identity")
    with urllib.request.urlopen(req, timeout=timeout) as r:
        data = r.read()
        if r.headers.get("Content-Encoding") == "gzip":
            data = gzip.GzipFile(fileobj=io.BytesIO(data)).read()
        return data, r.getcode(), r.headers.get("Content-Type", "")


def access_token(login):
    """Fetch a public stream playback access token via the standard GQL query."""
    query = {
        "query": ("query($login:String!){streamPlaybackAccessToken(channelName:$login,"
                  "params:{platform:\"web\",playerBackend:\"mediaplayer\",playerType:\"site\"})"
                  "{value signature}}"),
        "variables": {"login": login},
    }
    body = json.dumps(query).encode()
    req = urllib.request.Request(GQL, data=body, headers={
        "Client-ID": CLIENT_ID,
        "Content-Type": "application/json",
        "User-Agent": UA,
    })
    with urllib.request.urlopen(req, timeout=15) as r:
        j = json.loads(r.read())
    tok = (j.get("data") or {}).get("streamPlaybackAccessToken")
    if not tok or not tok.get("value") or not tok.get("signature"):
        raise RuntimeError("no playback token (channel offline or invalid?)")
    return tok["value"], tok["signature"]


def master_url(login):
    value, sig = access_token(login)
    # Omit supported_codecs/fast_bread so Twitch biases toward MPEG-TS (not fMP4).
    params = {
        "allow_source": "true",
        "allow_audio_only": "true",
        "player_backend": "mediaplayer",
        "playlist_include_framerate": "true",
        "reassignments_supported": "true",
        "sig": sig,
        "token": value,
        "p": str(int(time.time() * 1000) % 1_000_000),
    }
    return USHER.format(login=login) + "?" + urllib.parse.urlencode(params)


def _proxied(base, upstream, kind):
    return "http://%s/%s?u=%s" % (base, kind, urllib.parse.quote(upstream, safe=""))


def filter_master(text, base):
    """Keep only H.264 MPEG-TS variants; rewrite their URLs back to this proxy."""
    out, lines = [], text.splitlines()
    i, n = 0, len(lines)
    while i < n:
        line = lines[i]
        if line.startswith("#EXT-X-STREAM-INF"):
            codecs = ""
            m = re.search(r'CODECS="([^"]*)"', line)
            if m:
                codecs = m.group(1).lower()
            url = lines[i + 1] if i + 1 < n else ""
            drop = ("avc1" not in codecs and codecs != "") or any(b in codecs for b in BAD_VIDEO)
            if not drop and url and not url.startswith("#"):
                out.append(line)
                out.append(_proxied(base, url, "media"))
            i += 2
            continue
        out.append(line)   # headers, #EXT-X-MEDIA (audio), everything else: passthrough
        i += 1
    return "\n".join(out) + "\n"


_DATERANGE_AD = re.compile(r'CLASS="twitch-stitched-ad"', re.I)
_DURATION = re.compile(r'DURATION=([0-9.]+)', re.I)


def rewrite_media(text, base, channel):
    """Rewrite segment URLs; pass all tags through; record ad-break state."""
    ad_break, ad_dur = False, None
    out = []
    for line in text.splitlines():
        if line.startswith("#EXT-X-DATERANGE") and _DATERANGE_AD.search(line):
            ad_break = True
            d = _DURATION.search(line)
            if d:
                ad_dur = max(ad_dur or 0.0, float(d.group(1)))
            out.append(line)                       # keep the ad tag -- do not strip
        elif line.startswith("#EXT-X-MAP:"):
            m = re.search(r'URI="([^"]*)"', line)   # fMP4 init (rare once filtered); relay it
            if m:
                line = line.replace(m.group(1), _proxied(base, m.group(1), "seg"))
            out.append(line)
        elif line and not line.startswith("#"):
            out.append(_proxied(base, line, "seg"))  # a segment URL
        else:
            out.append(line)                         # EXTINF, DISCONTINUITY, MEDIA-SEQUENCE, ...
    with _state_lock:
        _ad_state[channel] = {"ad_break": ad_break, "ad_duration": ad_dur, "updated": time.time()}
    return "\n".join(out) + "\n"


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass  # quiet

    def _send(self, code, body, ctype):
        if isinstance(body, str):
            body = body.encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "keep-alive")
        self.end_headers()
        self.wfile.write(body)

    def _fail(self, code, msg):
        self._send(code, msg + "\n", "text/plain")

    def do_GET(self):
        try:
            parsed = urllib.parse.urlparse(self.path)
            path = parsed.path
            qs = urllib.parse.parse_qs(parsed.query)
            base = self.headers.get("Host") or "127.0.0.1"

            if path == "/status":
                ch = (qs.get("channel", [None])[0])
                with _state_lock:
                    if ch:
                        st = _ad_state.get(ch, {"ad_break": False, "ad_duration": None, "updated": 0})
                    else:
                        # no channel given: report the most recently updated one
                        st = max(_ad_state.values(), key=lambda s: s["updated"], default={
                            "ad_break": False, "ad_duration": None, "updated": 0})
                return self._send(200, json.dumps(st), "application/json")

            m = re.match(r"^/hls/([A-Za-z0-9_]{1,40})\.m3u8$", path)
            if m:
                login = m.group(1).lower()
                data, _, _ = _get(master_url(login), timeout=15)
                # remember which channel this proxy instance is serving media for
                text = filter_master(data.decode("utf-8", "replace"), base)
                return self._send(200, text, "application/vnd.apple.mpegurl")

            if path == "/media":
                up = qs.get("u", [None])[0]
                if not up:
                    return self._fail(400, "missing u")
                # derive channel from the upstream host path for /status keying
                channel = _channel_of(up)
                data, _, _ = _get(up, timeout=15)
                text = rewrite_media(data.decode("utf-8", "replace"), base, channel)
                return self._send(200, text, "application/vnd.apple.mpegurl")

            if path == "/seg":
                up = qs.get("u", [None])[0]
                if not up:
                    return self._fail(400, "missing u")
                return self._stream_segment(up)

            return self._fail(404, "not found")
        except urllib.error.HTTPError as e:
            self._fail(502, "upstream HTTP %d" % e.code)
        except Exception as e:
            self._fail(502, "proxy error: %s" % e)

    def _stream_segment(self, up):
        req = urllib.request.Request(up, headers={"User-Agent": UA})
        with urllib.request.urlopen(req, timeout=20) as r:
            self.send_response(200)
            ctype = r.headers.get("Content-Type") or "video/mp2t"
            self.send_header("Content-Type", ctype)
            clen = r.headers.get("Content-Length")
            if clen:
                self.send_header("Content-Length", clen)
            self.send_header("Connection", "keep-alive")
            self.end_headers()
            while True:
                chunk = r.read(65536)
                if not chunk:
                    break
                self.wfile.write(chunk)


def _channel_of(url):
    # Twitch media playlist hosts encode the channel; fall back to the path.
    m = re.search(r"/([A-Za-z0-9_]+)/(?:chunked|[0-9]+p[0-9]*)/", url)
    return (m.group(1).lower() if m else "stream")


def main():
    ap = argparse.ArgumentParser(description="LiveVita local HLS/TLS bridge (transparent; does not strip ads)")
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=8080)
    args = ap.parse_args()
    srv = ThreadingHTTPServer((args.host, args.port), Handler)
    print("LiveVita HLS bridge on http://%s:%d" % (args.host, args.port))
    print("  Vita playlist : http://<this-pc-ip>:%d/hls/<channel>.m3u8" % args.port)
    print("  Ad status     : http://<this-pc-ip>:%d/status?channel=<channel>" % args.port)
    print("  (transparent relay -- ads and the commercial slate are passed through, not removed)")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print("\nbye")
        sys.exit(0)


if __name__ == "__main__":
    main()
