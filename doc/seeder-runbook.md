Running an ApertureCoin DNS seeder
==================================

Community operators run ApertureCoin's DNS seeds. Nobody runs one on the
project's behalf. A seed is a small DNS server that crawls the peer-to-peer
network and answers `A`/`AAAA` queries with healthy node addresses. New nodes
query the seeds once to find their first peers. After that they learn
addresses from those peers.

Operators must follow [dnsseed-policy.md](dnsseed-policy.md).

Network constants
-----------------

| Network | Magic bytes | P2P port | Minimum protocol version | Required services |
|---------|-------------|----------|--------------------------|-------------------|
| main    | `a9 c3 e1 d7` | 9433  | 70017 | `NODE_NETWORK` \| `NODE_WITNESS` (0x9) |
| testnet | `b3 d5 f7 c9` | 19435 | 70017 | `NODE_NETWORK` \| `NODE_WITNESS` (0x9) |

The user agent prefix is `/ApertureCoinCore:`.

Setup
-----

1. **Run a full node** (`apertured`) on the seeder host. The crawler needs a
   first peer. Once other nodes are known, use `-seednode`. On day one, use
   `-addnode` with peers announced on the project issue tracker.
2. **Build a crawler.** Fork [sipa/bitcoin-seeder](https://github.com/sipa/bitcoin-seeder)
   and change these constants:
   - `pchMessageStart` in `protocol.cpp`: the magic bytes above
   - the default port in `main.cpp` and `db.h`: 9433 (testnet 19435)
   - the `PROTOCOL_VERSION` / required-version check: 70017
   - the seed hostname lists in `main.cpp`: leave empty and bootstrap with `-s <ip>`
   - the user-agent check, if you filter by it: `/ApertureCoinCore:`
3. **Delegate DNS.** Given a domain `example.org` and a seeder host
   `vps.example.org`:
   ```
   seed.example.org.  IN NS  vps.example.org.
   ```
   Then run:
   ```
   ./dnsseed -h seed.example.org -n vps.example.org -m admin.example.org -p 5353
   ```
   and redirect UDP 53 to 5353 with `iptables`.
4. **Check it works:**
   ```
   dig +short seed.example.org
   ```
   This should return several node IPs on port 9433. Also run:
   ```
   apertured -dnsseed=1 -connect=0 -forcednsseed -debug=net
   ```
   against a build that lists your seed.
5. **Monitor it:** track query rate, the number of good nodes in `dnsseed.dump`,
   and uptime. Alert if fewer than 20 good nodes are returned.

Getting listed
--------------

1. Open a pull request against `src/chainparams.cpp` that adds your hostname
   to `vSeeds`, plus an entry in `doc/dnsseed-policy.md`. Include:
   - the operator's name or pseudonym and contact
   - the hosting provider and ASN
   - a statement that you have read and follow the policy
2. Reviewers check that the seed answers correctly for 7 consecutive days.
3. **Launch gate:** mainnet ships with DNS seeds only once at least three
   independent operators are live, on at least two hosting providers or ASNs.
   Until then, nodes rely on the fixed seeds (`contrib/seeds`), `-seednode`
   and `-addnode`.

Fixed seeds
-----------

Fixed seeds are compiled in as a fallback when DNS fails. Before each release,
a maintainer runs:

```
python3 contrib/seeds/makeseeds.py < seeds_main.txt > contrib/seeds/nodes_main.txt
python3 contrib/seeds/generate-seeds.py contrib/seeds > src/chainparamsseeds.h
```

`seeds_main.txt` is the `dnsseed.dump` output from at least two independent
operators, merged together.
