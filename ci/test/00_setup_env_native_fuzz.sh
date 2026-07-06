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

export CI_IMAGE_NAME_TAG="mirror.gcr.io/ubuntu:24.04"
export CONTAINER_NAME=ci_native_fuzz
export APT_LLVM_V="20"
export PACKAGES="clang-${APT_LLVM_V} llvm-${APT_LLVM_V} libclang-rt-${APT_LLVM_V}-dev libevent-dev libboost-dev libsqlite3-dev"
export NO_DEPENDS=1
export RUN_UNIT_TESTS=false
export RUN_FUNCTIONAL_TESTS=false
export RUN_FUZZ_TESTS=true
export GOAL="all"
export CI_CONTAINER_CAP="--cap-add SYS_PTRACE"  # If run with (ASan + LSan), the container needs access to ptrace (https://github.com/google/sanitizers/issues/764)
<<<<<<< v29.0
export BITCOIN_CONFIG="\
 -DBUILD_FOR_FUZZING=ON \
 -DSANITIZERS=fuzzer,address,undefined,float-divide-by-zero,integer \
 -DCMAKE_C_COMPILER=clang-${APT_LLVM_V} \
 -DCMAKE_CXX_COMPILER=clang++-${APT_LLVM_V} \
 -DCMAKE_C_FLAGS='-ftrivial-auto-var-init=pattern' \
 -DCMAKE_CXX_FLAGS='-ftrivial-auto-var-init=pattern' \
"
export LLVM_SYMBOLIZER_PATH="/usr/bin/llvm-symbolizer-${APT_LLVM_V}"
=======
export FREICOIN_CONFIG="--enable-fuzz --with-sanitizers=fuzzer,address,undefined,float-divide-by-zero,integer \
CC='clang-18 -ftrivial-auto-var-init=pattern' CXX='clang++-18 -ftrivial-auto-var-init=pattern'"
export CCACHE_MAXSIZE=200M
export LLVM_SYMBOLIZER_PATH="/usr/bin/llvm-symbolizer-18"
>>>>>>> tc-28.1
