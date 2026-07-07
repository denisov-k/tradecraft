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

export CONTAINER_NAME=ci_native_previous_releases
export CI_IMAGE_NAME_TAG="mirror.gcr.io/ubuntu:22.04"
# Use minimum supported python3.10 and gcc-11, see doc/dependencies.md
export PACKAGES="gcc-11 g++-11 python3-zmq"
export DEP_OPTS="DEBUG=1 CC=gcc-11 CXX=g++-11"
export TEST_RUNNER_EXTRA="--previous-releases --coverage --extended --exclude feature_dbcrash"  # Run extended tests so that coverage does not fail, but exclude the very slow dbcrash
export RUN_UNIT_TESTS_SEQUENTIAL="true"
export RUN_UNIT_TESTS="false"
export GOAL="install"
export DOWNLOAD_PREVIOUS_RELEASES="true"
export BITCOIN_CONFIG="\
 -DWITH_ZMQ=ON -DBUILD_GUI=ON -DREDUCE_EXPORTS=ON \
 -DCMAKE_BUILD_TYPE=Debug \
 -DCMAKE_C_FLAGS='-funsigned-char' \
 -DCMAKE_C_FLAGS_DEBUG='-g0 -O2' \
 -DCMAKE_CXX_FLAGS='-funsigned-char' \
 -DCMAKE_CXX_FLAGS_DEBUG='-g0 -O2' \
 -DAPPEND_CPPFLAGS='-DBOOST_MULTI_INDEX_ENABLE_SAFE_MODE' \
"
