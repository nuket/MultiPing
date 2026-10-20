#!/usr/bin/env bash
# MIT License
#
# MultiPing
# Copyright (c) 2026 Max Vilimpoc
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in all
# copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.

# Builds the Linux programs: multiping-gtk (GTK 3), multiping-qt5 and multiping-qt6 (Qt Widgets,
# one source against each major). By default in Docker's Debian bookworm (build_linux.Dockerfile,
# the image built the first time and again when the file changes) as the invoking user, so they
# need nothing newer than glibc 2.36, GTK 3.24, Qt 5.15 and Qt 6.4 and run on Ubuntu 24.04 and
# later as on Debian 12. --native builds with this machine's own g++ and kits instead, for
# whichever of gtk+-3.0, Qt5Widgets and Qt6Widgets pkg-config finds.

set -euo pipefail
cd "$(dirname "$0")"

NATIVE=0
for arg in "$@"; do
    case "$arg" in
        --native) NATIVE=1 ;;
        *) echo "Usage: $0 [--native]" >&2; exit 1 ;;
    esac
done

# The revision under the copyright: MULTIPING_GIT_REVISION when the caller knows it
# (systems-build.py, building a tree synced without its .git), else this checkout's.
GIT_REV=${MULTIPING_GIT_REVISION:-$(git rev-parse --short HEAD 2>/dev/null || echo unknown)}

# The compile, the same in the container and natively. Each program is one source file; the
# icon is MultiPing.ico, built in by the assembler from this folder (MultiPingPosix.h).
COMPILE='
set -e
FLAGS="-std=c++17 -O2 -Wall -Wextra -fPIC -DGIT_REV=\"$GIT_REV\""
LINK="-pthread -Wl,--as-needed -Wl,-O1 -s"
built=""
if pkg-config --exists gtk+-3.0; then
    g++ $FLAGS $(pkg-config --cflags gtk+-3.0) MultiPingGtk.cpp -o multiping-gtk $(pkg-config --libs gtk+-3.0) $LINK
    built="$built multiping-gtk"
fi
for major in 5 6; do
    if pkg-config --exists Qt${major}Widgets; then
        g++ $FLAGS $(pkg-config --cflags Qt${major}Widgets) MultiPingQt.cpp -o multiping-qt$major $(pkg-config --libs Qt${major}Widgets) $LINK
        built="$built multiping-qt$major"
    fi
done
[ -n "$built" ] || { echo "neither GTK 3 nor Qt found by pkg-config" >&2; exit 1; }
for f in $built; do echo "Built $f, $(stat -c %s $f) bytes"; done
'

if [ "$NATIVE" = 1 ]; then
    GIT_REV="$GIT_REV" sh -c "$COMPILE"
    exit 0
fi

# The image is named after the Dockerfile's contents, so a changed file makes a new one.
TAG=$(sha256sum build_linux.Dockerfile | cut -c1-12)
IMAGE=multiping-linux-build:$TAG
if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
    # With https_proxy set (systems-build.py's tunnel on loopback), apt in the image build
    # reaches the proxy over this machine's own network.
    NET=()
    if [ -n "${https_proxy:-}" ]; then
        NET=(--network host --build-arg "https_proxy=$https_proxy" --build-arg "http_proxy=${http_proxy:-$https_proxy}")
    fi
    echo "Building the image $IMAGE (once)"
    docker build -q "${NET[@]}" -t "$IMAGE" - < build_linux.Dockerfile   # no context: it copies nothing in
fi
docker run --rm \
    --user "$(id -u):$(id -g)" \
    --env GIT_REV="$GIT_REV" \
    --env HOME=/tmp \
    --volume /etc/localtime:/etc/localtime:ro \
    --volume "$(pwd)":/src \
    --workdir /src \
    "$IMAGE" \
    sh -c "$COMPILE"
