# freicoind with the share chain, built from a branch of the tradecraft fork.
# Wallet-less: payouts go to the address in .env, the node holds no keys.
FROM ubuntu:24.04 AS build
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential automake autoconf libtool pkg-config git ca-certificates python3 bsdmainutils \
    libboost-dev libevent-dev && rm -rf /var/lib/apt/lists/*
ARG TC_REPO=https://github.com/denisov-k/tradecraft.git
ARG TC_REF=sharechain
RUN git clone --depth 1 --branch ${TC_REF} ${TC_REPO} /src
WORKDIR /src
ARG JOBS=2
RUN ./autogen.sh \
 && ./configure --without-gui --disable-wallet --disable-tests --disable-bench --disable-fuzz-binary CXXFLAGS="-O2" \
 && make -j${JOBS} && strip src/freicoind src/freicoin-cli

FROM ubuntu:24.04
RUN apt-get update && apt-get install -y --no-install-recommends libevent-2.1-7t64 libevent-pthreads-2.1-7t64 ca-certificates \
 && rm -rf /var/lib/apt/lists/*
COPY --from=build /src/src/freicoind /src/src/freicoin-cli /usr/local/bin/
RUN useradd -m -u 1002 freicoin && mkdir -p /data && chown freicoin:freicoin /data
USER freicoin
VOLUME /data
ENTRYPOINT ["freicoind", "-datadir=/data"]
