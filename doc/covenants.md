Tapscript covenants and introspection
=====================================

ApertureCoin extends tapscript (BIP342, leaf version 0xc0) with opcodes for
covenants: scripts that constrain how the coins they lock may be spent. The
opcodes are active from genesis. Legacy and SegWit v0 scripts are unchanged:
there, the introspection opcodes remain invalid and `OP_NOP4` remains a NOP.

Each new opcode takes one of tapscript's `OP_SUCCESSx` code points, which can
be defined without affecting any other script version. The remaining
`OP_SUCCESSx` values (for example 0xca and 0xd4–0xfe) keep their BIP342
meaning.

Opcodes
-------

| Opcode | Value | Stack effect | Notes |
|---|---|---|---|
| `OP_CAT` | 0x7e | a b → a‖b | Result ≤ 520 bytes |
| `OP_MUL` / `OP_DIV` / `OP_MOD` | 0x95–0x97 | a b → a×b, a÷b, a mod b | Division truncates toward zero; division or modulo by zero fails |
| `OP_CHECKTEMPLATEVERIFY` | 0xb3 | h → h | BIP119 standard template hash; fails if a 32-byte `h` does not match |
| `OP_INPUTINDEX` | 0xc0 | → i | |
| `OP_ACTIVEBYTECODE` | 0xc1 | → script | From the last executed `OP_CODESEPARATOR` |
| `OP_TXVERSION`, `OP_TXINPUTCOUNT`, `OP_TXOUTPUTCOUNT`, `OP_TXLOCKTIME` | 0xc2–0xc5 | → n | |
| `OP_UTXOVALUE`, `OP_UTXOBYTECODE` | 0xc6, 0xc7 | i → x | Spent output `i`. The bytecode excludes the token prefix. |
| `OP_OUTPOINTTXHASH`, `OP_OUTPOINTINDEX` | 0xc8, 0xc9 | i → x | Outpoint of input `i`. The txid is in internal byte order. |
| `OP_INPUTSEQUENCENUMBER` | 0xcb | i → n | |
| `OP_OUTPUTVALUE`, `OP_OUTPUTBYTECODE` | 0xcc, 0xcd | i → x | Output `i` |
| `OP_UTXOTOKENCATEGORY`, `OP_UTXOTOKENCOMMITMENT`, `OP_UTXOTOKENAMOUNT` | 0xce–0xd0 | i → x | Token data of spent output `i` |
| `OP_OUTPUTTOKENCATEGORY`, `OP_OUTPUTTOKENCOMMITMENT`, `OP_OUTPUTTOKENAMOUNT` | 0xd1–0xd3 | i → x | Token data of output `i` |

The introspection opcodes follow Bitcoin Cash CHIP-2021-02 and CHIP-2022-02,
including their code points. The rules that apply to all of them:

- **Token category.** The category push is 32 bytes. A mutable or minting NFT
  appends one capability byte (0x01 mutable, 0x02 minting). An output without
  tokens pushes an empty category and commitment, and an amount of 0.
- **Numbers.** Tapscript arithmetic uses 8-byte script numbers, in the range
  −(2⁶³−1) … 2⁶³−1. Any result outside this range fails with "Arithmetic
  result out of 64-bit range". Values and token amounts can therefore be
  computed on directly.
- **Indexes.** An out-of-range index fails with "Introspection index out of
  range".

Examples
--------

These scripts are tested in `test/functional/feature_covenants.py`.

```
# Only this exact spending transaction (BIP119 template):
<template_hash> OP_CHECKTEMPLATEVERIFY

# Recursive vault: output 0 must pay back to this script, keeping value minus a fee cap.
0 OP_OUTPUTBYTECODE OP_INPUTINDEX OP_UTXOBYTECODE OP_EQUALVERIFY
0 OP_OUTPUTVALUE OP_INPUTINDEX OP_UTXOVALUE <max_fee> OP_SUB OP_GREATERTHANOREQUAL

# Rate limit: at most 100 tokens of this category may leave per spend.
0 OP_OUTPUTTOKENCATEGORY OP_INPUTINDEX OP_UTXOTOKENCATEGORY OP_EQUALVERIFY
0 OP_OUTPUTTOKENAMOUNT OP_INPUTINDEX OP_UTXOTOKENAMOUNT 100 OP_SUB OP_GREATERTHANOREQUAL
```

These are the building blocks for agent mandates (`doc/agent-mandates.md`):
spending limits that consensus enforces on an agent's key, whatever the agent
decides.

Tests
-----

| Test | What it covers |
|---|---|
| `src/test/script_tapscript_ext_tests.cpp` | Every opcode's result and each error path. It also checks `IsOpSuccess` and runs the CTV template hash against a known transaction. |
| `test/functional/feature_covenants.py` | P2TR script-path spends through mempool and block validation. |
