# Frame/input tap

This fork can serve what you see and what you press to another program on the
same machine. It sends sampled decoded video frames as RGB24, and the
controller state chiaki-ng sends to the console, over a Unix socket. It is
the data source for [kcd1-autodice](https://github.com/phreakazoid21/kcd1-autodice)'s
capture mode. It is off unless you turn it on.

The code is `gui/src/tap.cpp` and `gui/include/tap.h`. The hooks are one call
each in `StreamSession::SendFeedbackState` and `DpadSendFeedbackState`, in the
session's connect/quit events, in the frame-thread handler in `qmlbackend.cpp`,
and `ChiakiTap::Init()` in `main.cpp`.

## Turning it on

| Variable | Meaning |
|----------|---------|
| `CHIAKI_TAP_SOCK` | Socket path to listen on. Unset or empty: the tap is off and every hook returns at once. |
| `CHIAKI_TAP_FPS` | Periodic frames per second, 0 to 240 (decimals allowed). Default 4. `0` sends only post-input frames. |

Environment variables only reach chiaki-ng when you start it from a shell, not
from the Dock or Finder:

```bash
CHIAKI_TAP_SOCK=/tmp/chiaki-tap.sock /Applications/chiaki-ng.app/Contents/MacOS/chiaki
```

or, through Launch Services:

```bash
open -a chiaki-ng --env CHIAKI_TAP_SOCK=/tmp/chiaki-tap.sock
```

`/tmp/chiaki-tap.sock` is the path autodice dials by default (`-tapsock`).

The tap needs libswscale and POSIX sockets. A build without them (Windows,
or an FFmpeg configured with `--disable-all` and no swscale) compiles the tap
out. Setting `CHIAKI_TAP_SOCK` there only logs a warning.

## Behaviour

* **Listening.** At startup chiaki-ng binds the path and makes it `0600`, so
  only your user can connect. A stale socket from an earlier run is removed
  first. If the path exists and is *not* a socket, chiaki-ng leaves it alone
  and the tap stays off. The path must be shorter than 104 bytes on macOS
  (108 on Linux).
* **One client at a time.** A connection made while a live client is
  connected is closed at once, without a HELLO. A client that has gone away
  is noticed within half a second, and the next one is accepted.
* **No client, no work.** Until a client connects, frames are not referenced,
  copied or converted. The only work is remembering the last input state.
* **Never blocks streaming.** The input hook and the frame hook only take a
  short lock and queue a message. A worker thread does every conversion and
  write.
  * Frames wait in a queue of 2. When the worker falls behind, the oldest
    queued frame is dropped, sparing post-input frames.
  * Inputs and session events have their own queue and are never dropped.
  * A client that doesn't take a message within 3 seconds has stopped
    reading and is disconnected. So is one that lets 16384 inputs pile up.
    A client therefore sees a gap in inputs only as a disconnect, never
    silently.
* **Which frames.** The worker sends a reference to the latest decoded frame
  `CHIAKI_TAP_FPS` times per second. It also sends the first frame decoded at
  least 150 ms after each input change; those frames carry flag bit 0. One
  frame can be both, and is sent once. Frames are converted at their native
  size. VideoToolbox (and other hardware) frames are first copied to memory
  with `av_hwframe_transfer_data`, then converted with swscale. The colour
  matrix follows the frame's tags; untagged HD frames are treated as BT.709.
  HDR (10-bit) streams are converted without tone mapping.
* **Which inputs.** An INPUT message is sent right before chiaki-ng hands the
  merged controller state to the session. It is the state after input
  blocking, every controller, the keyboard and touch are combined. It goes out
  only when the buttons, L2/R2 or a stick axis changed since the last one.
  Gyro, accelerometer and touchpad positions are not sent.

## Wire protocol (version 1)

Every message is a 24-byte header followed by `payload_len` bytes. All
integers are little-endian.

| Offset | Type | Field |
|-------:|------|-------|
| 0 | u32 | `magic` = `0x5041544B` (the bytes `K T A P`) |
| 4 | u8 | `version` = 1 |
| 5 | u8 | `type` |
| 6 | u16 | `flags` |
| 8 | u64 | `ts_ns`: `CLOCK_REALTIME` in nanoseconds |
| 16 | u32 | `payload_len` |
| 20 | u32 | reserved, 0 |

| Type | Name | Payload |
|-----:|------|---------|
| 1 | HELLO | JSON `{"version":1,"chiaki":"<version>"}`. Always the first message on a connection. |
| 2 | INPUT | 16 bytes, see below |
| 3 | FRAME | 16-byte frame header, then `stride * height` bytes of pixels |
| 4 | SESSION | JSON `{"event":"started"\|"stopped","width":W,"height":H}` |

INPUT payload:

| Offset | Type | Field |
|-------:|------|-------|
| 0 | u32 | `buttons`: `ChiakiControllerButton` bitmask (`CROSS` = 1<<0 … `PS` = 1<<15) |
| 4 | u8 | `l2` |
| 5 | u8 | `r2` |
| 6 | i16 | `left_x` |
| 8 | i16 | `left_y` |
| 10 | i16 | `right_x` |
| 12 | i16 | `right_y` |
| 14 | u16 | reserved, 0 |

FRAME payload:

| Offset | Type | Field |
|-------:|------|-------|
| 0 | u32 | `width` |
| 4 | u32 | `height` |
| 8 | u32 | `stride`: bytes per row, at least `3 * width` (rows are padded to 32 bytes) |
| 12 | u8 | `format`: 1 = RGB24 (R, G, B per pixel) |
| 13 | u8[3] | padding, 0 |
| 16 | | `stride * height` bytes, top row first |

FRAME flags: bit 0 is set on a frame sent because it follows an input change.

Notes for clients:

* `ts_ns` is taken when the event happens: an input change, a frame being
  decoded, the stream connecting or quitting. Frames are converted later, so
  across types messages are *not* in `ts_ns` order. Order them by `ts_ns`.
* SESSION is sent when the stream connects and when it quits. A client that
  connects mid-stream doesn't get a "started" event. The width and height
  are the negotiated stream resolution; each FRAME carries its own size.
* A 1080p frame is about 6 MB, and a 4K one about 25 MB. Reject absurd
  `payload_len` values instead of allocating them.
* Ignore message types you don't know: skip `payload_len` bytes.

## Building on macOS

These steps built the fork's `chiaki` target on Apple silicon with Homebrew's
Qt 6.11, FFmpeg 9.0 and protobuf 36, and Xcode's clang 21. libplacebo 7.387
was built from source; Homebrew's `libplacebo` formula is what the CI uses
instead. `.github/workflows/build-macos-arm.yml` is the reference for a
release build, including `macdeployqt`, MoltenVK and signing.

```bash
brew install qtbase qtdeclarative qtsvg ffmpeg pkgconf opus openssl@3 cmake ninja protobuf libplacebo json-c sdl2
# optional: speexdsp (microphone echo cancelling), qt (adds WebEngine for the PSN login page)
git submodule update --init third-party/nanopb third-party/jerasure third-party/gf-complete third-party/curl third-party/cpp-steam-tools

# nanopb needs a Python protobuf runtime at least as new as protoc:
# protoc 36.x needs protobuf 7.36.x.
python3 -m venv ~/.venvs/chiaki
~/.venvs/chiaki/bin/pip install "protobuf>=$(protoc --version | awk '{print "7." $2}' | cut -d. -f1-2),<8"

export CPATH="$(brew --prefix)/opt/ffmpeg/include"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCHIAKI_ENABLE_CLI=OFF -DCHIAKI_ENABLE_TESTS=OFF -DCHIAKI_ENABLE_STEAMDECK_NATIVE=OFF \
  -DCHIAKI_LIB_MINIUPNPC_EXTERNAL_PROJECT=ON \
  -DOPENSSL_ROOT_DIR="$(brew --prefix)/opt/openssl@3" \
  -DCMAKE_PREFIX_PATH="$(brew --prefix)/opt/openssl@3;$(brew --prefix)" \
  -DPython_EXECUTABLE=$HOME/.venvs/chiaki/bin/python3 -DPYTHON_EXECUTABLE=$HOME/.venvs/chiaki/bin/python3
cmake --build build --target chiaki
```

CMake prints `Frame/input tap enabled` when it finds libswscale. The app is
`build/gui/chiaki.app`.

Two things that break this build:

* **OpenSSL 4.** If Homebrew has OpenSSL 4 linked, its headers in
  `/opt/homebrew/include` shadow openssl@3's. The bundled curl then fails in
  `vtls/openssl.c` with `incomplete definition of type 'ASN1_INTEGER'`. Run
  `brew unlink openssl@4`, as the CI does. If you'd rather keep it linked,
  copy openssl@3's `include/openssl` into a directory of your own and put
  that first in `CPATH`. Adding openssl@3's own include directory to `CPATH`
  doesn't work, because clang drops a `CPATH` entry that repeats an
  `-isystem` one.
* **nanopb.** `VersionError: Detected incompatible Protobuf Gencode/Runtime
  versions` means the Python protobuf is older than `protoc`. Use the venv
  above, and make sure CMake picked it (`-DPython_EXECUTABLE`).
