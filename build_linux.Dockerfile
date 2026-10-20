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

# The image build_linux.sh builds the Linux programs in: Debian bookworm, with the headers of
# GTK 3, Qt 5 and Qt 6. What is built against it needs glibc 2.36 and runs on every later
# distribution with the kit it uses: GTK 3 keeps its ABI, and Qt keeps its within a major, so
# bookworm's GTK 3.24, Qt 5.15 and Qt 6.4 run with any later 3, 5 and 6 (Ubuntu 24.04 and on).
FROM debian:bookworm-slim
RUN apt-get update \
 && apt-get install -y --no-install-recommends g++ pkg-config libgtk-3-dev qtbase5-dev qt6-base-dev \
 && rm -rf /var/lib/apt/lists/*
