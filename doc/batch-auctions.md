Frequent batch auctions
=======================

When agents trade continuously in a first-come order book, the fastest bot wins
by racing, and whoever orders transactions can sandwich trades. Frequent batch
auctions (FBA; arXiv 2302.01177, 2202.06384) remove the race: orders submitted
together clear together, at one uniform price.

On ApertureCoin, uniform-price batches are enforced by **covenants**, using the
tapscript introspection opcodes (`doc/covenants.md`). No dedicated consensus
rule is involved.

Orders
------

An order is a Taproot output. Its internal key is the unspendable NUMS point,
and it has two leaves:

- **fill** is the order covenant:
  - **Sell `q` tokens, limit `L`.** Price `p ≥ L`. Output `i` (same index as
    the order's input) pays the owner exactly `q·p` plus the order's SCIENCE
    value, and carries no tokens.
  - **Buy `q` tokens, limit `L`, funded with `V` SCIENCE.** Price `p ≤ L`.
    Output `i` pays the owner exactly `q` tokens of the category plus
    `V − q·p` SCIENCE.
- **cancel** is `<owner> OP_CHECKSIG`. The owner can withdraw an unfilled
  order at any time.

The order's parameters (side, quantity, limit, owner) are inside the Taproot
commitment. They stay **sealed on chain until settlement**. Observers of the
chain cannot see what an order is before it fills, which removes the
information that sandwiching relies on (arXiv 2609.31379). Traders give their
parameters privately to the party that computes the batch.

Settlement
----------

A settlement transaction spends the matched orders, pays every trader in the
output with the same index, adds a fee input for the solver, and ends with a
price marker as its last output:

```
OP_RETURN <"APXP" ‖ category> <p>
```

Each fill leaf reconstructs the expected marker from its witness price with
`OP_CAT`, and compares it with `OP_TXOUTPUTCOUNT 1SUB OP_OUTPUTBYTECODE`.
There is only one last output, so **every order in the transaction executes at
the same price `p`**. A solver cannot pay different traders different prices;
the test checks this ("price discrimination is rejected"). Token and SCIENCE
conservation are ordinary consensus rules.

`aperture_sdk.auction.clearing_price` computes, for all-or-nothing orders, the
price that maximizes matched volume: the midpoint of the marginal seller and
buyer limits.

What is and is not enforced
---------------------------

| Property | Enforced by |
|---|---|
| Every fill respects its limit | Each order's covenant |
| All fills in a settlement use one price | Each covenant checks the single marker |
| Exact payment to each trader, no skimming | Each covenant checks an exact value and token amount |
| Owner can cancel | The cancel leaf |
| Orders are sealed until filled | The Taproot commitment |
| **Every crossing order is included** | **Not enforced.** Proof-of-work miners choose which transactions to include, and enforcing inclusion would need a consensus-level order index. Several independent solvers competing for the fee input mitigate this. |
| Commit-reveal with bonds | Not needed for sealing, since Taproot already hides the parameters. It remains an option if orders must also be hidden from the solver. |
| AMM liquidity in the batch | Not implemented. A pool UTXO whose covenant accepts the uniform price (arXiv 2210.04929) fits the same pattern. |

Prices are integers in satoshis per token base unit. Tokens meant for trading
should choose a base unit that gives enough price resolution.

Tests
-----

`test/functional/feature_batch_auction.py` covers:

- sealed order posting;
- computation of the clearing price;
- rejection of prices outside a matched limit on either side;
- rejection of price discrimination;
- settlement at the uniform price, with the payouts and token delivery
  checked;
- a non-crossing pair that cannot fill;
- owner cancellation of a buy order and a sell order.
