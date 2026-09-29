Genesis block mining
====================

`mine_genesis.py` rebuilds the genesis coinbase transaction exactly the way
`CreateGenesisBlock()` in `src/chainparams.cpp` does. It then searches for a
nonce using a native ApertureMatMul helper.

Build the helper:

```
gcc -O2 -c blake3_unit.c -o blake3_unit.o
g++ -O2 -std=c++17 -pthread -I../../src -o mine_genesis mine_genesis.cpp \
    ../../src/crypto/matmulpow.cpp blake3_unit.o
```

`blake3_unit.c` compiles the vendored portable BLAKE3 as a single unit.

Example, the mainnet parameters from `src/chainparams.cpp`:

```
./mine_genesis.py --timestamp "<pszTimestamp>" --script <hex> \
    --time <unix time> --bits 1f0fffff --dim 512
```

Mainnet launch checklist
------------------------

- Replace the development fund placeholder script (a provably unspendable
  `OP_RETURN`) in `CMainParams` with the published multisig P2WSH.
- Re-mine the genesis block and update the asserts in `CMainParams`.
