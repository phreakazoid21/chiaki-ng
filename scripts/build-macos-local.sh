#!/usr/bin/env bash
# Builds chiaki-ng's GUI (with the frame/input tap) on a Mac with Homebrew.
#
# usage: scripts/build-macos-local.sh [--install] [--clean]
#
#   --install  brew install the formulas that are missing (otherwise the
#              script lists them and stops)
#   --clean    delete the build directory first
#
# Environment:
#   BUILD_DIR        where to build (default: build, in the repository)
#   CHIAKI_DEPS_DIR  where helpers that Homebrew doesn't provide go
#                    (default: ~/Library/Caches/chiaki-ng-build)
#
# Besides missing formulas (and only with --install), nothing outside
# BUILD_DIR, CHIAKI_DEPS_DIR and the repository's submodules is changed. In
# particular the script works around a linked OpenSSL 4 instead of unlinking
# it, and keeps nanopb's Python packages in its own virtualenv. Without
# Homebrew's libplacebo it builds one into CHIAKI_DEPS_DIR.
#
# The result is build/gui/chiaki.app, run in place. It is not the packaged
# release app; .github/workflows/build-macos-arm.yml shows how that one is
# bundled (macdeployqt, MoltenVK) and signed.
set -euo pipefail

LIBPLACEBO_TAG=v7.387.0 # what scripts/build-libplacebo.sh builds
SUBMODULES=(third-party/nanopb third-party/jerasure third-party/gf-complete third-party/curl third-party/cpp-steam-tools)
FORMULAS=(qtbase qtdeclarative qtsvg ffmpeg opus openssl@3 protobuf json-c pkgconf cmake ninja)

install=0
clean=0
for arg in "$@"; do
	case "$arg" in
		--install) install=1 ;;
		--clean) clean=1 ;;
		-h|--help) sed -n '2,24p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
		*) echo "unknown option: $arg (see --help)" >&2; exit 2 ;;
	esac
done

step() { printf '\n\033[1m== %s\033[0m\n' "$*"; }
die() { printf '\033[31merror:\033[0m %s\n' "$*" >&2; exit 1; }

[[ "$(uname -s)" == Darwin ]] || die "this script is for macOS"
command -v brew >/dev/null || die "Homebrew is required: https://brew.sh"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
DEPS="${CHIAKI_DEPS_DIR:-$HOME/Library/Caches/chiaki-ng-build}"
BREW="$(brew --prefix)"

step "Homebrew formulas"
installed="$(brew list --formula -1)"
missing=()
for f in "${FORMULAS[@]}"; do
	grep -qx "$f" <<<"$installed" || missing+=("$f")
done
# Homebrew's sdl2 is now an alias of sdl2-compat.
grep -qxE 'sdl2|sdl2-compat' <<<"$installed" || missing+=(sdl2)
# libplacebo is optional: it can be built here instead.
want_placebo=0
grep -qx libplacebo <<<"$installed" || want_placebo=1
if ((${#missing[@]})); then
	if ((install)); then
		brew install "${missing[@]}"
	else
		echo "Missing: ${missing[*]}"
		echo "Install them with: brew install ${missing[*]}"
		echo "or rerun this script with --install."
		exit 1
	fi
fi
if ((want_placebo && install)); then
	brew install libplacebo
	want_placebo=0
fi
echo "ok"

mkdir -p "$DEPS"
VENV="$DEPS/venv"
export PATH="$VENV/bin:$PATH"
export PKG_CONFIG_PATH="$DEPS/prefix/lib/pkgconfig:${PKG_CONFIG_PATH:-}"

step "Python tools (in $VENV)"
# nanopb's generator needs a protobuf runtime at least as new as protoc
# ("Detected incompatible Protobuf Gencode/Runtime versions" otherwise).
[[ -x "$VENV/bin/python3" ]] || python3 -m venv "$VENV"
"$VENV/bin/pip" install --quiet --upgrade pip protobuf
echo "protoc: $(protoc --version), python protobuf: $("$VENV/bin/python3" -c 'import google.protobuf as p; print(p.__version__)')"

step "libplacebo"
if pkg-config --exists 'libplacebo >= 7.349.0'; then
	echo "using libplacebo $(pkg-config --modversion libplacebo)"
else
	((want_placebo)) || die "libplacebo is installed but pkg-config can't find it"
	echo "building $LIBPLACEBO_TAG into $DEPS/prefix (brew install libplacebo skips this)"
	"$VENV/bin/pip" install --quiet meson jinja2
	if [[ ! -d "$DEPS/libplacebo" ]]; then
		git clone --quiet --recursive --shallow-submodules --depth 1 --branch "$LIBPLACEBO_TAG" https://github.com/haasn/libplacebo.git "$DEPS/libplacebo"
	fi
	meson setup --reconfigure "$DEPS/libplacebo/build" "$DEPS/libplacebo" --prefix "$DEPS/prefix" --libdir lib \
		-Dxxhash=disabled -Ddemos=false -Dtests=false -Dvk-proc-addr=disabled
	ninja -C "$DEPS/libplacebo/build" install >/dev/null
	mkdir -p "$DEPS/prefix/include"
	cp -R "$DEPS/libplacebo/3rdparty/Vulkan-Headers/include/vulkan" "$DEPS/libplacebo/3rdparty/Vulkan-Headers/include/vk_video" "$DEPS/prefix/include/"
fi

step "Headers"
# With OpenSSL 4 linked into $BREW/include, its headers shadow openssl@3's:
# several dependencies put -I$BREW/include on the command line. The bundled
# curl then doesn't compile, and chiaki-lib compiles against OpenSSL 4 but
# links openssl@3. A copy of openssl@3's headers, put before every target's
# own include directories, wins. (The real directory can't be used: clang
# drops an -I that repeats an -isystem directory.)
rm -rf "$DEPS/openssl3-include"
mkdir -p "$DEPS/openssl3-include"
cp -R "$BREW/opt/openssl@3/include/openssl" "$DEPS/openssl3-include/"
printf 'include_directories(BEFORE "%s")\n' "$DEPS/openssl3-include" >"$DEPS/openssl3-first.cmake"
export CPATH="$DEPS/prefix/include:$BREW/opt/ffmpeg/include"
echo "openssl@3 $(sed -n 's/^# *define OPENSSL_VERSION_STR "\(.*\)"/\1/p' "$DEPS/openssl3-include/openssl/opensslv.h")"

step "Submodules"
git -C "$ROOT" submodule update --init "${SUBMODULES[@]}"

step "Configure ($BUILD_DIR)"
((clean)) && rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"
log="$BUILD_DIR/configure.log"
if ! cmake -S "$ROOT" -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE=Release \
	-DCHIAKI_ENABLE_CLI=OFF -DCHIAKI_ENABLE_TESTS=OFF -DCHIAKI_ENABLE_STEAMDECK_NATIVE=OFF \
	-DCHIAKI_LIB_MINIUPNPC_EXTERNAL_PROJECT=ON \
	-DOPENSSL_ROOT_DIR="$BREW/opt/openssl@3" \
	-DOPENSSL_INCLUDE_DIR="$BREW/opt/openssl@3/include" \
	-DOPENSSL_CRYPTO_LIBRARY="$BREW/opt/openssl@3/lib/libcrypto.dylib" \
	-DOPENSSL_SSL_LIBRARY="$BREW/opt/openssl@3/lib/libssl.dylib" \
	-DCMAKE_PREFIX_PATH="$BREW/opt/openssl@3;$BREW;$DEPS/prefix" \
	-DCMAKE_PROJECT_INCLUDE="$DEPS/openssl3-first.cmake" \
	-DPython_EXECUTABLE="$VENV/bin/python3" -DPYTHON_EXECUTABLE="$VENV/bin/python3" \
	>"$log" 2>&1; then
	tail -40 "$log"
	die "configure failed; full output in $log"
fi
grep -h "Frame/input tap" "$log" || true

step "Build"
cmake --build "$BUILD_DIR" --target chiaki

app="$BUILD_DIR/gui/chiaki.app"
step "Done: $app"
echo "Run it with the tap on:"
echo "  CHIAKI_TAP_SOCK=/tmp/chiaki-tap.sock \"$app/Contents/MacOS/chiaki\""
