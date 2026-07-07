v28.1-42439 Release Notes
=========================

Freicoin version v28.1-42439 is now available from:

  https://github.com/tradecraftio/tradecraft/releases/tag/v28.1-42439

This release includes new features, various bug fixes and performance improvements, as well as updated translations.

Please report bugs using the issue tracker at GitHub:

  https://github.com/tradecraftio/tradecraft/issues

To receive security and update notifications, please subscribe to:

  https://tradecraft.groups.io/g/announce/

How to Upgrade
--------------

If you are running an older version, shut it down.  Wait until it has completely shut down (which might take a few minutes in some cases), then run the installer (on Windows) or just copy over `/Applications/Freicoin-Qt` (on macOS) or `freicoind`/`freicoin-qt` (on Linux).

Upgrading directly from a version of Freicoin that has reached its EOL is possible, but it might take some time if the data directory needs to be migrated.  Old wallet versions of Freicoin are generally supported.

Compatibility
-------------

Freicoin is supported and extensively tested on operating systems using the Linux Kernel 3.17+, macOS 11.0+, and Windows 7 and newer.  Freicoin should also work on most other UNIX-like systems but is not as frequently tested on them.  It is not recommended to use Freicoin on unsupported systems.

Notable changes
---------------

### P2P and Network Changes

- Support for UPnP was dropped. If you want to open a port automatically, consider using the `-natpmp`
option instead, which uses PCP or NAT-PMP depending on router support. (#31130)

- libnatpmp was replaced with a built-in implementation of PCP and NAT-PMP (still enabled using the `-natpmp` option). This supports automatic IPv4 port forwarding as well as IPv6 pinholing. (#30043)

- When the `-port` configuration option is used, the default onion listening port will now be derived to be that port + 1 instead of being set to a fixed value (8640 on mainnet).  This re-allows setups with multiple local nodes using different `-port` and not using `-bind`, which would lead to a startup failure in v28-42407 due to a port collision.

  Note that a `HiddenServicePort` manually configured in `torrc` may need adjustment if used in connection with the `-port` option.  For example, if you are using `-port=5555` with a non-standard value and not using `-bind=...=onion`, previously Freicoin would listen for incoming Tor connections on `127.0.0.1:8640`.  Now it would listen on `127.0.0.1:5556` (`-port` plus one).  If you configured the hidden service manually in torrc now you have to change it from `HiddenServicePort 8639 127.0.0.1:8640` to `HiddenServicePort 8639 127.0.0.1:5556`, or configure bitcoind with `-bind=127.0.0.1:8640=onion` to get the previous behavior.  (bitcoin/bitcoin#31223)

- bitcoin/bitcoin#30568 addrman: change internal id counting to int64_t

### Mempool Policy and Mining Changes

- bitcoin/bitcoin#31166 key: clear out secret data in DecodeExtKey

- Due to a bug, the default block reserved weight (`4,000 WU`) for fixed-size block header, transactions count, and coinbase transaction was reserved twice and could not be lowered. As a result the total reserved weight was always `8,000 WU`, meaning that even when specifying a `-blockmaxweight` higher than the default (even to the max of `4,000,000 WU`), the actual block size will never exceed `3,992,000 WU`.
The fix consolidates the reservation into a single place and introduces a new startup option, `-blockreservedweight` which specifies the reserved weight directly. The default value of `-blockreservedweight` is set to `8,000 WU` to ensure backward compatibility for users who relied on the previous behavior of `-blockmaxweight`.
The minimum value of `-blockreservedweight` is set to `2,000 WU`. Users setting `-blockreservedweight` below the default should ensure that the total weight of their block header, transaction count, and coinbase transaction does not exceed the reduced value or they may risk mining an invalid block. (#31384)

- bitcoin/bitcoin#31013 depends: For mingw cross compile use `-gcc-posix` to prevent library conflict
- bitcoin/bitcoin#31502 depends: Fix CXXFLAGS on NetBSD

- The RPC `testmempoolaccept` response now includes a `reject-details` field in some cases,
similar to the complete error messages returned by `sendrawtransaction` (#28121)

- bitcoin/bitcoin#31016 test: add missing sync to feature_fee_estimation.py
- bitcoin/bitcoin#31448 fuzz: add cstdlib to FuzzedDataProvider
- bitcoin/bitcoin#31419 test: fix MIN macro redefinition
- bitcoin/bitcoin#31563 rpc: Extend scope of validation mutex in generateblock

- `getblockchaininfo` and `getchainstates` now return `nBits` and the current target in the `target` field (#31583)

- bitcoin/bitcoin#30961 ci: add LLVM_SYMBOLIZER_PATH to Valgrind fuzz job

### New RPCs

- bitcoin/bitcoin#31267 refactor: Drop deprecated space in `operator""_mst`
- bitcoin/bitcoin#31431 util: use explicit cast in MultiIntBitSet::Fill()

Credits
-------

Thanks to everyone who directly contributed to this release:

- fanquake
- Hennadii Stepanov
- laanwj
- MarcoFalke
- Mark Friedenbach
- Marnix
- Martin Zumsande
- Sebastian Falbesoner

As well as to everyone that helped with translations on [Transifex](https://www.transifex.com/tradecraft/freicoin-1/).
