# OpenBSD Build Guide

**Updated for OpenBSD [7.8](https://www.openbsd.org/78.html)**

This guide describes how to build freicoind, command-line utilities, and GUI on OpenBSD.

## Preparation

### 1. Install Required Dependencies
Run the following as root to install the base dependencies for building.

```bash
pkg_add git cmake boost libevent
```

SQLite is required for the wallet:

```bash
pkg_add sqlite3
```

To build Bitcoin Core without the wallet, use `-DENABLE_WALLET=OFF`.

Cap'n Proto is needed for IPC functionality (see [multiprocess.md](multiprocess.md)):

```bash
pkg_add capnproto
```

Compile with `-DENABLE_IPC=OFF` if you do not need IPC functionality.

See [dependencies.md](dependencies.md) for a complete overview.

### 2. Clone Freicoin Repo
Clone the Freicoin repository to a directory. All build scripts and commands will run from this directory.
``` bash
git clone https://github.com/tradecraftio/tradecraft.git
```

### 3. Install Optional Dependencies

#### Wallet Dependencies

It is not necessary to build wallet functionality to run either `freicoind` or `freicoin-qt`.

###### Descriptor Wallet Support

SQLite is required to support [descriptor wallets](descriptors.md).

``` bash
pkg_add sqlite3
```

###### Legacy Wallet Support
BerkeleyDB is only required to support legacy wallets.

It is recommended to use Berkeley DB 4.8. You cannot use the BerkeleyDB library
from ports. However you can build it yourself, [using depends](/depends).

Refer to [depends/README.md](/depends/README.md) for detailed instructions.

```bash
gmake -C depends NO_BOOST=1 NO_LIBEVENT=1 NO_QT=1 NO_SQLITE=1 NO_ZMQ=1 NO_USDT=1
...
to: /path/to/bitcoin/depends/*-unknown-openbsd*
```

Then set `BDB_PREFIX`:

```bash
export BDB_PREFIX="[path displayed above]"
```

#### GUI Dependencies
###### Qt6

Freicoin includes a GUI built with the cross-platform Qt Framework. To compile the GUI, we need to install
the necessary parts of Qt, the libqrencode and pass `-DBUILD_GUI=ON`. Skip if you don't intend to use the GUI.

```bash
pkg_add qt6-qtbase qt6-qttools
```

###### libqrencode

The GUI will be able to encode addresses in QR codes unless this feature is explicitly disabled. To install libqrencode, run:

```bash
pkg_add libqrencode
```

Otherwise, if you don't need QR encoding support, use the `-DWITH_QRENCODE=OFF` option to disable this feature in order to compile the GUI.

---

#### Notifications
###### ZeroMQ

Freicoin can provide notifications via ZeroMQ. If the package is installed, support will be compiled in.
```bash
pkg_add zeromq
```

#### Test Suite Dependencies
There is an included test suite that is useful for testing code changes when developing.
To run the test suite (recommended), you will need to have Python 3 installed:

```bash
pkg_add python py3-zmq  # Select the newest version of the python package if necessary.
```

## Building Bitcoin Core

### 1. Configuration

There are many ways to configure Freicoin, here are a few common examples:

##### Wallet and GUI:
This enables wallet support and the GUI, assuming SQLite and Qt 6 are installed.

```bash
cmake -B build -DBUILD_GUI=ON
```

Run `cmake -B build -LH` to see the full list of available options.

### 2. Compile

```bash
cmake --build build     # Append "-j N" for N parallel jobs.
ctest --test-dir build  # Append "-j N" for N parallel tests.
```

## Resource limits

If the build runs into out-of-memory errors, the instructions in this section
might help.

The standard ulimit restrictions in OpenBSD are very strict:
```bash
data(kbytes)         1572864
```

This is, unfortunately, in some cases not enough to compile some `.cpp` files in the project,
(see issue [#6658](https://github.com/bitcoin/bitcoin/issues/6658)).
If your user is in the `staff` group the limit can be raised with:
```bash
ulimit -d 3000000
```
The change will only affect the current shell and processes spawned by it. To
make the change system-wide, change `datasize-cur` and `datasize-max` in
`/etc/login.conf`, and reboot.
