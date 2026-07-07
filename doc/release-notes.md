
Bitcoin Core version v30.0 is now available from:

  <https://bitcoincore.org/bin/bitcoin-core-30.0/>
=======
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

Policy
------

- The maximum number of potentially executed legacy signature operations in a
  single standard transaction is now limited to 2500. Signature operations in all
  previous output scripts, in all input scripts, as well as all P2SH redeem
  scripts (if there are any) are counted toward the limit. The new limit is
  assumed to not affect any known typically formed standard transactions. The
  change was done to prepare for a possible BIP54 deployment in the future. (#32521)

- `-datacarriersize` is increased to 100,000 by default, which effectively uncaps
  the limit (as the maximum transaction size limit will be hit first). It can be
  overridden with `-datacarriersize=83` to revert to the limit enforced in previous
  versions. (#32406)

- When the `-port` configuration option is used, the default onion listening port will now be derived to be that port + 1 instead of being set to a fixed value (8640 on mainnet).  This re-allows setups with multiple local nodes using different `-port` and not using `-bind`, which would lead to a startup failure in v28-42407 due to a port collision.

  Note that a `HiddenServicePort` manually configured in `torrc` may need adjustment if used in connection with the `-port` option.  For example, if you are using `-port=5555` with a non-standard value and not using `-bind=...=onion`, previously Freicoin would listen for incoming Tor connections on `127.0.0.1:8640`.  Now it would listen on `127.0.0.1:5556` (`-port` plus one).  If you configured the hidden service manually in torrc now you have to change it from `HiddenServicePort 8639 127.0.0.1:8640` to `HiddenServicePort 8639 127.0.0.1:5556`, or configure bitcoind with `-bind=127.0.0.1:8640=onion` to get the previous behavior.  (bitcoin/bitcoin#31223)

- bitcoin/bitcoin#30568 addrman: change internal id counting to int64_t

- The default minimum relay feerate (`-minrelaytxfee`) and incremental relay feerate
  (`-incrementalrelayfee`) have been changed to 0.1 satoshis per vB. They can still
  be changed using their respective configuration options, but it is recommended to
  change both together if you decide to do so. (#33106)

- bitcoin/bitcoin#31166 key: clear out secret data in DecodeExtKey

  Note that unless these lower defaults are widely adopted across the network, transactions
  created with lower fee rates are not guaranteed to propagate or confirm. The wallet
  feerates remain unchanged; `-mintxfee` must be changed before attempting to create
  transactions with lower feerates using the wallet. (#33106)

- bitcoin/bitcoin#31013 depends: For mingw cross compile use `-gcc-posix` to prevent library conflict
- bitcoin/bitcoin#31502 depends: Fix CXXFLAGS on NetBSD

- Opportunistic 1-parent-1-child package relay has been improved to handle
  situations when the child already has unconfirmed parent(s) in the mempool.
  This means that 1p1c packages can be accepted and propagate, even if they are
  connected to broader topologies: multi-parent-1-child (where only 1 parent
  requires fee-bumping), grandparent-parent-child (where only parent requires
  fee-bumping) etc. (#31385)

- bitcoin/bitcoin#31016 test: add missing sync to feature_fee_estimation.py
- bitcoin/bitcoin#31448 fuzz: add cstdlib to FuzzedDataProvider
- bitcoin/bitcoin#31419 test: fix MIN macro redefinition
- bitcoin/bitcoin#31563 rpc: Extend scope of validation mutex in generateblock

- A new `bitcoin` command line tool has been added to make features more discoverable
  and convenient to use. The `bitcoin` tool just calls other executables and does not
  implement any functionality on its own. Specifically `bitcoin node` is a synonym for
  `bitcoind`, `bitcoin gui` is a synonym for `bitcoin-qt`, and `bitcoin rpc` is a synonym
  for `bitcoin-cli -named`. Other commands and options can be listed with `bitcoin help`.
  The new `bitcoin` command is an alternative to calling other commands directly, but it
  doesn't replace them, and there are no plans to deprecate existing commands. (#31375)

- bitcoin/bitcoin#30961 ci: add LLVM_SYMBOLIZER_PATH to Valgrind fuzz job

- Support for external signing on Windows has been re-enabled. (#29868)


- Logs now include which peer sent us a header. Additionally there are fewer
  redundant header log messages. A side-effect of this change is that for
  some untypical cases new headers aren't logged anymore, e.g. a direct
  `BLOCK` message with a previously unknown header and `submitheader` RPC. (#27826)
=======
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
