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

# Only install BCC tracing packages in CI. Container has to match the host for BCC to work.
if [[ "${INSTALL_BCC_TRACING_TOOLS}" == "true" ]]; then
  # Required for USDT functional tests to run
  BPFCC_PACKAGE="bpfcc-tools linux-headers-$(uname --kernel-release)"
  export CI_CONTAINER_CAP="--privileged -v /sys/kernel:/sys/kernel:rw"
else
  BPFCC_PACKAGE=""
  export CI_CONTAINER_CAP="--cap-add SYS_PTRACE"  # If run with (ASan + LSan), the container needs access to ptrace (https://github.com/google/sanitizers/issues/764)
fi

export CONTAINER_NAME=ci_native_asan
export APT_LLVM_V="20"
export PACKAGES="systemtap-sdt-dev clang-${APT_LLVM_V} llvm-${APT_LLVM_V} libclang-rt-${APT_LLVM_V}-dev python3-zmq qtbase5-dev qttools5-dev qttools5-dev-tools libevent-dev libboost-dev libdb5.3++-dev libzmq3-dev libqrencode-dev libsqlite3-dev ${BPFCC_PACKAGE}"
export NO_DEPENDS=1
export GOAL="install"
<<<<<<< v29.0
export BITCOIN_CONFIG="\
 -DWITH_USDT=ON -DWITH_ZMQ=ON -DWITH_BDB=ON -DWARN_INCOMPATIBLE_BDB=OFF -DBUILD_GUI=ON \
 -DSANITIZERS=address,float-divide-by-zero,integer,undefined \
 -DCMAKE_C_COMPILER=clang-${APT_LLVM_V} \
 -DCMAKE_CXX_COMPILER=clang++-${APT_LLVM_V} \
 -DCMAKE_C_FLAGS='-ftrivial-auto-var-init=pattern' \
 -DCMAKE_CXX_FLAGS='-ftrivial-auto-var-init=pattern -Wno-error=deprecated-declarations' \
 -DAPPEND_CXXFLAGS='-std=c++23' \
 -DAPPEND_CPPFLAGS='-DARENA_DEBUG -DDEBUG_LOCKORDER' \
"
=======
export FREICOIN_CONFIG="--enable-usdt --enable-zmq --with-incompatible-bdb --with-gui=qt5 \
CPPFLAGS='-DARENA_DEBUG -DDEBUG_LOCKORDER' \
--with-sanitizers=address,float-divide-by-zero,integer,undefined \
CC='clang-18 -ftrivial-auto-var-init=pattern' CXX='clang++-18 -ftrivial-auto-var-init=pattern'"
export CCACHE_MAXSIZE=300M
>>>>>>> tc-28.1
