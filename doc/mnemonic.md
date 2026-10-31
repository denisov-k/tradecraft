# 12-word recovery phrases (BIP39)

Descriptor wallets can be created from, and restored from, a 12-word BIP39 recovery
phrase. The derivation matches the freicoin.ru web wallet, so the same words work in both:

- BIP39 English word list, no BIP39 passphrase
- BIP32 master key from the seed ("Bitcoin seed")
- BIP84 account `m/84'/0'/0'` on mainnet (`m/84'/1'/0'` on testnet and regtest)
- receive addresses `/0/i`, change `/1/i`, `wpk()` outputs (`fc1q...` addresses)

Test vector: `abandon abandon abandon abandon abandon abandon abandon abandon abandon
abandon abandon about` gives first receive address `fc1qvst2uj34cyh64fdg4t4tlsy0jguq39zqeyu8z4`
(master fingerprint `73c5da0a`). Never use that phrase for real coins: it is public.

## GUI

File -> Create Wallet. "Back up with a 12-word recovery phrase" is ticked by default: the
new words are shown once and must be written down before continuing. Tick "Restore from
recovery words I already have" to type in an existing phrase instead; the whole block chain
is then scanned for the wallet's coins.

## Command line (freicoind / freicoin-cli)

New wallet with new words:

    freicoin-cli generatemnemonic
    freicoin-cli createwallet "mywallet" false true
    freicoin-cli -rpcwallet=mywallet importmnemonic "word1 word2 ... word12" now

`createwallet <name> false true` makes a blank wallet (private keys enabled, no keys yet).
`now` skips the rescan, which is right for words that have never been used.

Restore from existing words (full rescan from the genesis block):

    freicoin-cli createwallet "restored" false true
    freicoin-cli -rpcwallet=restored importmnemonic "word1 word2 ... word12"

A Unix timestamp can be given instead of `now` to rescan from that time.

Preview the addresses for a phrase without storing anything:

    freicoin-cli mnemonicdescriptors "word1 word2 ... word12"
    freicoin-cli deriveaddresses "<receive descriptor from above>" "[0,4]"

If the wallet should be encrypted, run `encryptwallet` after `importmnemonic` (the import
needs the keys unlocked).

## RPCs

- `generatemnemonic ( words )`: new phrase from the strong RNG; 12 (default), 15, 18, 21
  or 24 words. Nothing is stored.
- `mnemonicdescriptors "mnemonic"`: the public receive and change descriptors for a phrase.
- `importmnemonic "mnemonic" ( timestamp )`: imports the phrase's receive and change
  descriptors into the loaded descriptor wallet as the active `bech32` descriptors, then
  rescans from `timestamp` (default 0, the whole chain; or "now").

Words are case-insensitive and extra spaces are ignored. A phrase with an unknown word,
the wrong number of words or a bad checksum is rejected.

Like any HD wallet, a restore finds coins only within the keypool look-ahead
(`-keypool`, default 1000 addresses on each chain).

Run `freicoin-cli help <command>` for the full help text.
