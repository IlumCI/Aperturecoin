// Copyright (c) 2026 The ApertureCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chain.h>
#include <test/util/setup_common.h>
#include <txdb.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

BOOST_FIXTURE_TEST_SUITE(powv2_index_tests, BasicTestingSetup)

static CBlockHeader V2Header()
{
    CBlockHeader h;
    h.nVersion = 0x20000000 | CBlockHeader::VERSION_POWV2;
    h.nTime = 1790000000;
    h.nBits = 0x207fffff;
    h.powv2.batch_root = uint256S("01");
    h.powv2.op = 3;
    h.powv2.tile_i = 1;
    h.powv2.tile_j = 2;
    h.powv2.span_s = 4;
    h.powv2.panel.assign(32 * 32, 0);
    for (size_t k = 0; k < h.powv2.panel.size(); ++k) h.powv2.panel[k] = static_cast<int8_t>(k % 191) - 95;
    return h;
}

BOOST_AUTO_TEST_CASE(trim_and_reload)
{
    const CBlockHeader header = V2Header();
    const uint256 hash = header.GetHash();
    CBlockIndex index(header);
    index.phashBlock = &hash;

    // Write the entry the way FlushStateToDisk does, then trim it.
    CBlockTreeDB db(1 << 20, true);
    BOOST_REQUIRE(db.WriteBatchSync({}, 0, {&index}));
    BOOST_CHECK(!index.IsPowV2PanelTrimmed());
    index.TrimPowV2Panel();
    BOOST_CHECK(index.IsPowV2PanelTrimmed());
    BOOST_CHECK(index.powv2.panel.capacity() == 0);

    // Reload through the loader, as GetBlockHeader() does.
    static CBlockTreeDB* s_db;
    s_db = &db;
    const PowV2Loader saved = g_powv2_loader;
    g_powv2_loader = [](const uint256& h, PowV2Proof& out) { return s_db->ReadPowV2(h, out); };
    BOOST_CHECK(index.GetBlockHeader().GetHash() == hash);
    BOOST_CHECK(index.GetPowV2().panel == header.powv2.panel);

    // Rewriting a trimmed entry keeps its panel on disk.
    const CDiskBlockIndex disk(&index);
    BOOST_CHECK(disk.powv2.panel == header.powv2.panel);
    BOOST_REQUIRE(db.WriteBatchSync({}, 0, {&index}));
    PowV2Proof stored;
    BOOST_REQUIRE(db.ReadPowV2(hash, stored));
    BOOST_CHECK(stored.panel == header.powv2.panel);

    // A missing record is an error, not an empty panel.
    g_powv2_loader = [](const uint256&, PowV2Proof&) { return false; };
    BOOST_CHECK_THROW(index.GetBlockHeader(), std::runtime_error);
    g_powv2_loader = saved;

    // v1 entries are never trimmed.
    CBlockHeader v1 = header;
    v1.nVersion = 0x20000000;
    v1.powv2.SetNull();
    CBlockIndex v1_index(v1);
    v1_index.TrimPowV2Panel();
    BOOST_CHECK(!v1_index.IsPowV2PanelTrimmed());
}

BOOST_AUTO_TEST_SUITE_END()
