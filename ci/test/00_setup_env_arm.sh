#!/usr/bin/env bash
#
# Copyright (c) 2019-present The Bitcoin Core developers
# Copyright (c) 2010-2024 The Freicoin Developers
#
# This program is free software: you can redistribute it and/or modify it under
# the terms of version 3 of the GNU Affero General Public License as published
# by the Free Software Foundation.
#
# This program is distributed in the hope that it will be useful, but WITHOUT
# ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
# FOR A PARTICULAR PURPOSE.  See the GNU Affero General Public License for more
# details.
#
# You should have received a copy of the GNU Affero General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.

export LC_ALL=C.UTF-8

export HOST=arm-linux-gnueabihf
export DPKG_ADD_ARCH="armhf"
export PACKAGES="python3-zmq g++-arm-linux-gnueabihf libc6:armhf libstdc++6:armhf libfontconfig1:armhf libxcb1:armhf"
export CONTAINER_NAME=ci_arm_linux
export CI_IMAGE_NAME_TAG="mirror.gcr.io/debian:trixie"  # Check that https://packages.debian.org/trixie/g++-arm-linux-gnueabihf (version 14.x, similar to guix) can cross-compile
export CI_IMAGE_PLATFORM="linux/arm64"
export GOAL="install"
export CI_LIMIT_STACK_SIZE=1
# -Wno-psabi is to disable ABI warnings: "note: parameter passing for argument of type ... changed in GCC 7.1"
# This could be removed once the ABI change warning does not show up by default
export BITCOIN_CONFIG=" \
  --preset=dev-mode \
  -DREDUCE_EXPORTS=ON \
  -DCMAKE_CXX_FLAGS='-Wno-psabi -Wno-error=maybe-uninitialized' \
"
