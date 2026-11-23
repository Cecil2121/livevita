# Third-party notices

LiveVita's own code (everything in `src/` outside `src/player/`) is under the MIT License (`LICENSE`).
The app also contains code from the projects below, each under its own license.

| Component | Used for | License |
|---|---|---|
| [Vita IPTV](https://github.com/emrhky/VitaIPTV) by emrhky | the video player engine in `src/player/` (HLS, MPEG-TS, hardware H.264 decoding, audio, the HTTPS client) | MIT; full text in `licenses/VitaIPTV-LICENSE.txt` |
| [Mbed TLS](https://github.com/Mbed-TLS/mbedtls) | built-in TLS (https) | Apache-2.0; full text in `licenses/mbedtls-LICENSE.txt` |
| [jsmn](https://github.com/zserge/jsmn) | JSON parsing | MIT; full text in `licenses/jsmn-LICENSE.txt` |
| [minimp3](https://github.com/lieff/minimp3) | MP2 / MP3 audio decoding (through the player engine) | CC0 1.0; `src/player/minimp3.LICENSE` |
| [libvita2d](https://github.com/xerpi/libvita2d) | drawing | MIT |
| [FreeType](https://freetype.org), libpng, libjpeg-turbo, zlib | through vita2d | their own permissive licenses |
| [VitaSDK](https://vitasdk.org) | toolchain and system library stubs | various permissive licenses |

Changes made to the Vita IPTV files in `src/player/`:
- `curlio.c` / `curlio.h`: POST requests with custom headers, and the body of error answers.
- `hls.c` / `hls.h`: `HLS_URL_MAX` enlarged from 1024 to 2048 characters (Twitch's long playlist addresses); the
  `#EXT-X-MAP` URI is kept (`map_uri`).
- `vod.c` / `vod.h`: a fragmented-MP4 demuxer for live HLS (`fmp4_create`, `fmp4_set_init`, `fmp4_feed_segment`).
- `tsplayer.c`: the stream address buffer enlarged from 512 to 2048 characters; live HLS with fMP4 segments
  (init segment, whole segments handed to the fMP4 demuxer) instead of the "MPEG-TS only" error.
- `netua.h`: the PS Vita browser's User-Agent instead of VLC's.

Required statements:
- Mbed TLS: Copyright The Mbed TLS Contributors. Licensed under the Apache License, Version 2.0.
- Portions of this software are copyright © The FreeType Project (www.freetype.org). All rights reserved.
- This software is based in part on the work of the Independent JPEG Group.

LiveVita is not affiliated with, endorsed by or sponsored by Twitch Interactive, Inc. or Sony Interactive
Entertainment. "Twitch" is a trademark of Twitch Interactive, Inc.
