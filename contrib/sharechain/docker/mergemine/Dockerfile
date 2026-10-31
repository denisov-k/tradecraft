# Mark Friedenbach's mergemine bitcoind (Bitcoin Core 29 + stratum + merge mining) with the
# fixes Vintage Hash found on live home miners (patches/, applied by fix-stratum.sh).
FROM ubuntu:24.04 AS build
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake pkg-config git ca-certificates python3 patch \
    libboost-dev libevent-dev libsqlite3-dev libzmq3-dev && rm -rf /var/lib/apt/lists/*
ARG MM_REF=v29.0.0.1-44182
RUN git clone --depth 1 --branch ${MM_REF} https://github.com/tradecraftio/mergemine.git /src
WORKDIR /src
COPY patches /tmp/patches
COPY fix-stratum.sh /tmp/fix-stratum.sh
RUN bash /tmp/fix-stratum.sh
ARG JOBS=2
RUN cmake -B build -DBUILD_TESTS=OFF -DBUILD_BENCH=OFF -DBUILD_GUI=OFF -DWITH_ZMQ=ON -DCMAKE_BUILD_TYPE=Release \
 && cmake --build build -j${JOBS} --target bitcoind bitcoin-cli

FROM ubuntu:24.04
RUN apt-get update && apt-get install -y --no-install-recommends libevent-2.1-7t64 libevent-pthreads-2.1-7t64 libevent-extra-2.1-7t64 libevent-core-2.1-7t64 libsqlite3-0 libzmq5 ca-certificates \
 && rm -rf /var/lib/apt/lists/*
COPY --from=build /src/build/bin/bitcoind /src/build/bin/bitcoin-cli /usr/local/bin/
RUN useradd -m -u 1001 bitcoin && mkdir -p /data && chown bitcoin:bitcoin /data
USER bitcoin
VOLUME /data
ENTRYPOINT ["bitcoind", "-datadir=/data"]
