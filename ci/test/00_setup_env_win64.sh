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

export CONTAINER_NAME=ci_win64
export CI_IMAGE_NAME_TAG="mirror.gcr.io/debian:trixie"  # Check that https://packages.debian.org/trixie/g++-mingw-w64-ucrt64 can cross-compile
export HOST=x86_64-w64-mingw32ucrt
export PACKAGES="g++-mingw-w64-ucrt64 nsis"
export RUN_UNIT_TESTS=false
export RUN_FUNCTIONAL_TESTS=false
export GOAL="deploy"
export BITCOIN_CONFIG="\
  --preset=dev-mode \
  -DENABLE_IPC=OFF \
  -DWITH_USDT=OFF \
  -DREDUCE_EXPORTS=ON \
  -DCMAKE_CXX_FLAGS='-Wno-error=maybe-uninitialized' \
"
