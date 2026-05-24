/* System and third-party headers first (must not be affected by private hack) */
#include <net/if.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <iostream>
#include <fstream>
#include "gtest/gtest.h"
#include <gmock/gmock.h>
#include <nlohmann/json.hpp>
#include <nexthopgroup/nexthopgroupfull_json.h>
#include "mock_table.h"
#include "ipaddress.h"

/* Expose private members for testing */
#define private public
#include "ut_helpers_fpmsyncd.h"
#include "fpmlink.h"
#include "routesync.h"
#undef private

using namespace swss;
using namespace testing;
using json = nlohmann::ordered_json;

/*
 * Test Fixture for NHT backwalk / quick fixup tests.
 * Topologies loaded from JSON files matching ribfib_route_convergence.md LLD.
 */
namespace ut_fpmsyncd
{
    struct FpmSyncdNhtBackwalk : public ::testing::Test
    {
        std::shared_ptr<swss::DBConnector> m_app_db;
        std::shared_ptr<swss::RedisPipeline> pipeline;
        std::shared_ptr<NHGMgr> m_nhgmgr;
        std::shared_ptr<swss::Table> m_nextHopTable;
        std::shared_ptr<swss::Table> m_picContextTable;

        virtual void SetUp() override
        {
            testing_db::reset();
            m_app_db = std::make_shared<swss::DBConnector>("APPL_DB", 0);
            pipeline = std::make_shared<swss::RedisPipeline>(m_app_db.get());
            m_nhgmgr = std::make_shared<NHGMgr>(pipeline.get(), APP_NEXTHOP_GROUP_TABLE_NAME, APP_PIC_CONTEXT_TABLE_NAME, true);
            m_nextHopTable = std::make_shared<swss::Table>(m_app_db.get(), APP_NEXTHOP_GROUP_TABLE_NAME);
            m_picContextTable = std::make_shared<swss::Table>(m_app_db.get(), APP_PIC_CONTEXT_TABLE_NAME);
        }

        virtual void TearDown() override
        {
        }

        /*
         * Load a topology from a JSON file.
         *
         * Per LLD: "Load topology json, convert json str to fib::NextHopGroupFull
         * object and use addNHGFull(nhg, addr_family) to add them one by one."
         *
         * Each entry's JSON value is serialized back to string, then converted
         * via fib::from_json_string() to a NextHopGroupFull object.
         * Entries are added in dependency order (leaves first, composites last).
         *
         * Returns the set of NHG IDs loaded.
         */
        std::set<uint32_t> loadTopologyFromJson(const std::string& filename)
        {
            std::ifstream f(filename);
            EXPECT_TRUE(f.is_open()) << "Failed to open " << filename;
            json top_level = json::parse(f);

            /* Step 1: Convert each entry's json str to fib::NextHopGroupFull */
            std::map<uint32_t, fib::NextHopGroupFull> entries;
            for (auto it = top_level.items().begin(); it != top_level.items().end(); ++it) {
                std::string json_str = it.value().dump();
                fib::NextHopGroupFull nhg;
                EXPECT_TRUE(fib::from_json_string(json_str, nhg))
                    << "Failed to parse NHG from json str for key: " << it.key();
                entries[nhg.id] = nhg;
            }

            /* Step 2: Topological sort — add them one by one in dependency order */
            std::set<uint32_t> added;
            std::set<uint32_t> all_ids;
            for (auto it = entries.begin(); it != entries.end(); ++it) {
                all_ids.insert(it->first);
            }

            while (added.size() < entries.size()) {
                bool progress = false;
                for (auto it = entries.begin(); it != entries.end(); ++it) {
                    uint32_t id = it->first;
                    fib::NextHopGroupFull& nhg = it->second;
                    if (added.count(id)) continue;
                    bool deps_met = true;
                    for (uint32_t dep : nhg.depends) {
                        if (!added.count(dep)) {
                            deps_met = false;
                            break;
                        }
                    }
                    if (!deps_met) continue;

                    /* Determine address family from nexthop type */
                    uint8_t af = AF_INET6;
                    if (nhg.type == fib::NEXTHOP_TYPE_IPV4 ||
                        nhg.type == fib::NEXTHOP_TYPE_IPV4_IFINDEX) {
                        af = AF_INET;
                    }

                    /* Add to NHGMgr one by one */
                    EXPECT_EQ(m_nhgmgr->addNHGFull(nhg, af), 0)
                        << "Failed to addNHGFull for NHG " << id;
                    added.insert(id);
                    progress = true;
                }
                EXPECT_TRUE(progress) << "Circular dependency detected in topology";
                if (!progress) break;
            }

            return all_ids;
        }

        /* Reset all enable groups for a set of IDs */
        void resetAllEnableGroups(const std::set<uint32_t>& ids)
        {
            for (uint32_t id : ids) {
                RIBNHGEntry* e = m_nhgmgr->getRIBNHGEntryByRIBID(id);
                if (e) e->resetResolvedEnableGroup();
            }
        }

        /*
         * Helper: Run Part 1 backwalk directly and return the walking context.
         * This lets tests inspect visited_node_set, modified_node_set, etc.
         */
        fib_nhg_walking_ctx runPart1Backwalk(const std::string& nexthop, uint32_t start_id)
        {
            fib_nhg_walking_ctx ctx;
            ctx.nexthop_address = nexthop;
            ctx.rib_nhg_table = m_nhgmgr->m_rib_nhg_table;
            ctx.fib_nhg_walk_spec_func = NHGMgr::fib_nhg_walk_spec_for_node_quick_fixup;
            ctx.fib_nhg_prune_spec_func = NHGMgr::fib_nhg_prune_spec_for_node_quick_fixup;

            m_nhgmgr->fib_nhg_back_walk(start_id, ctx);
            return ctx;
        }

        /*
         * Helper: Run Part 2 (sonic_nhg) backwalk directly and return the walking context.
         */
        fib_nhg_walking_ctx runPart2Backwalk(const std::string& nexthop, uint32_t start_id)
        {
            fib_nhg_walking_ctx ctx;
            ctx.nexthop_address = nexthop;
            ctx.rib_nhg_table = m_nhgmgr->m_rib_nhg_table;
            ctx.fib_nhg_walk_spec_func = NHGMgr::fib_nhg_walk_spec_for_node_quick_fixup_sonic_nhg;
            ctx.fib_nhg_prune_spec_func = NHGMgr::fib_nhg_prune_spec_for_node_quick_fixup_sonic_nhg;

            m_nhgmgr->fib_nhg_back_walk(start_id, ctx);
            return ctx;
        }
    };
}

namespace ut_fpmsyncd
{
    /* ========== Topology 1 Tests ========== */

    /*
     * Topology 1, Local Failure: fc06::2 withdrawn
     * resolved_nhg_id = 237 (leaf, gateway fc06::2)
     *
     * Expected walk:
     * - visited: {237, 238, 257, 256, 258, 263, 262, 264}
     * - modified: {237, 238, 257, 256, 258, 263, 262, 264}
     * - 234 never reached (not in backwalk path from 237)
     *
     * State assertions:
     * - 237: self-disabled {237:false}
     * - 238: {234:true, 237:false}
     */
    TEST_F(FpmSyncdNhtBackwalk, Topology1_LocalFailure)
    {
        auto ids = loadTopologyFromJson("fpmsyncd/test_topology_1.json");
        ASSERT_EQ(ids.size(), 9u);

        auto ctx = runPart1Backwalk("fc06::2", 237);

        /* Verify walk context sets */
        std::set<uint32_t> expected_visited = {237, 238, 257, 256, 258, 263, 262, 264};
        std::set<uint32_t> expected_modified = {237, 238, 257, 256, 258, 263, 262, 264};
        EXPECT_EQ(ctx.visited_node_set, expected_visited);
        EXPECT_EQ(ctx.modified_node_set, expected_modified);

        /* 234 never reached */
        EXPECT_EQ(ctx.visited_node_set.count(234), 0u);

        /* State assertions */
        RIBNHGEntry* entry237 = m_nhgmgr->getRIBNHGEntryByRIBID(237);
        ASSERT_NE(entry237, nullptr);
        EXPECT_EQ(entry237->getResolvedEnableGroup()[237], false);

        RIBNHGEntry* entry238 = m_nhgmgr->getRIBNHGEntryByRIBID(238);
        ASSERT_NE(entry238, nullptr);
        EXPECT_EQ(entry238->getResolvedEnableGroup()[237], false);
        EXPECT_EQ(entry238->getResolvedEnableGroup()[234], true);
    }

    /*
     * Topology 1, Remote Failure 1: 2064:200::1e withdrawn
     * resolved_nhg_id = 258 (gateway 2064:200::1e, depends=[238])
     *
     * Expected walk:
     * - visited: {258, 256}
     * - modified: {258, 256}
     * - 258: all-disabled {238:false}, skip APPDB
     * - 256: {257:true, 258:false}
     */
    TEST_F(FpmSyncdNhtBackwalk, Topology1_RemoteFailure1)
    {
        loadTopologyFromJson("fpmsyncd/test_topology_1.json");

        auto ctx = runPart1Backwalk("2064:200::1e", 258);

        std::set<uint32_t> expected_visited = {258, 256};
        std::set<uint32_t> expected_modified = {258, 256};
        EXPECT_EQ(ctx.visited_node_set, expected_visited);
        EXPECT_EQ(ctx.modified_node_set, expected_modified);

        /* 258: all depends disabled */
        RIBNHGEntry* e258 = m_nhgmgr->getRIBNHGEntryByRIBID(258);
        ASSERT_NE(e258, nullptr);
        EXPECT_EQ(e258->getResolvedEnableGroup()[238], false);

        /* 256: 258 disabled via Visit-for-State, 257 still enabled */
        RIBNHGEntry* e256 = m_nhgmgr->getRIBNHGEntryByRIBID(256);
        ASSERT_NE(e256, nullptr);
        EXPECT_EQ(e256->getResolvedEnableGroup()[258], false);
        EXPECT_EQ(e256->getResolvedEnableGroup()[257], true);

        /* 238 untouched */
        RIBNHGEntry* e238 = m_nhgmgr->getRIBNHGEntryByRIBID(238);
        EXPECT_EQ(e238->getResolvedEnableGroup()[234], true);
        EXPECT_EQ(e238->getResolvedEnableGroup()[237], true);
    }

    /*
     * Topology 1, Remote Failure 2: 1::1 withdrawn
     * resolved_nhg_id = 263 (gateway 1::1, depends=[238])
     *
     * Expected walk:
     * - visited: {263, 262}
     * - modified: {263, 262}
     * - 263: all-disabled {238:false}
     * - 262: {263:false, 264:true}
     */
    TEST_F(FpmSyncdNhtBackwalk, Topology1_RemoteFailure2)
    {
        loadTopologyFromJson("fpmsyncd/test_topology_1.json");

        auto ctx = runPart1Backwalk("1::1", 263);

        std::set<uint32_t> expected_visited = {263, 262};
        std::set<uint32_t> expected_modified = {263, 262};
        EXPECT_EQ(ctx.visited_node_set, expected_visited);
        EXPECT_EQ(ctx.modified_node_set, expected_modified);

        RIBNHGEntry* e263 = m_nhgmgr->getRIBNHGEntryByRIBID(263);
        ASSERT_NE(e263, nullptr);
        EXPECT_EQ(e263->getResolvedEnableGroup()[238], false);

        RIBNHGEntry* e262 = m_nhgmgr->getRIBNHGEntryByRIBID(262);
        ASSERT_NE(e262, nullptr);
        EXPECT_EQ(e262->getResolvedEnableGroup()[263], false);
        EXPECT_EQ(e262->getResolvedEnableGroup()[264], true);

        /* 256, 238 untouched */
        RIBNHGEntry* e256 = m_nhgmgr->getRIBNHGEntryByRIBID(256);
        EXPECT_EQ(e256->getResolvedEnableGroup()[257], true);
        EXPECT_EQ(e256->getResolvedEnableGroup()[258], true);
    }

    /* ========== Topology 2 Tests ========== */

    /*
     * Topology 2, Local Failure: fc06::2 withdrawn
     * resolved_nhg_id = 235 (leaf, gateway fc06::2)
     *
     * Expected walk:
     * - visited: {235, 236, 260, 263, 264, 266, 269}
     * - modified: {235, 236, 260, 263, 264, 266, 269}
     * - 232, 270 never reached
     */
    TEST_F(FpmSyncdNhtBackwalk, Topology2_LocalFailure)
    {
        auto ids = loadTopologyFromJson("fpmsyncd/test_topology_2.json");
        ASSERT_EQ(ids.size(), 9u);

        auto ctx = runPart1Backwalk("fc06::2", 235);

        std::set<uint32_t> expected_visited = {235, 236, 260, 263, 264, 266, 269};
        std::set<uint32_t> expected_modified = {235, 236, 260, 263, 264, 266, 269};
        EXPECT_EQ(ctx.visited_node_set, expected_visited);
        EXPECT_EQ(ctx.modified_node_set, expected_modified);

        /* 232, 270 not visited */
        EXPECT_EQ(ctx.visited_node_set.count(232), 0u);
        EXPECT_EQ(ctx.visited_node_set.count(270), 0u);

        /* 235: self disabled */
        RIBNHGEntry* e235 = m_nhgmgr->getRIBNHGEntryByRIBID(235);
        ASSERT_NE(e235, nullptr);
        EXPECT_EQ(e235->getResolvedEnableGroup()[235], false);

        /* 236: 235 disabled, 232 enabled */
        RIBNHGEntry* e236 = m_nhgmgr->getRIBNHGEntryByRIBID(236);
        ASSERT_NE(e236, nullptr);
        EXPECT_EQ(e236->getResolvedEnableGroup()[235], false);
        EXPECT_EQ(e236->getResolvedEnableGroup()[232], true);

        /* 266: all-disabled {235:false} */
        RIBNHGEntry* e266 = m_nhgmgr->getRIBNHGEntryByRIBID(266);
        ASSERT_NE(e266, nullptr);
        EXPECT_EQ(e266->getResolvedEnableGroup()[235], false);

        /* 269: 266 disabled via Visit-for-State, 270 enabled */
        RIBNHGEntry* e269 = m_nhgmgr->getRIBNHGEntryByRIBID(269);
        ASSERT_NE(e269, nullptr);
        EXPECT_EQ(e269->getResolvedEnableGroup()[266], false);
        EXPECT_EQ(e269->getResolvedEnableGroup()[270], true);

        /* 232 untouched */
        RIBNHGEntry* e232 = m_nhgmgr->getRIBNHGEntryByRIBID(232);
        EXPECT_EQ(e232->getResolvedEnableGroup()[232], true);
    }

    /*
     * Topology 2, Remote Failure 1: 2064:100::1d withdrawn
     * resolved_nhg_id = 260 (gateway 2064:100::1d, depends=[236])
     *
     * Expected walk:
     * - visited: {260, 263}
     * - modified: {260, 263}
     * - 260: all-disabled {236:false}
     * - 263: {260:false, 264:true}
     */
    TEST_F(FpmSyncdNhtBackwalk, Topology2_RemoteFailure1)
    {
        loadTopologyFromJson("fpmsyncd/test_topology_2.json");

        auto ctx = runPart1Backwalk("2064:100::1d", 260);

        std::set<uint32_t> expected_visited = {260, 263};
        std::set<uint32_t> expected_modified = {260, 263};
        EXPECT_EQ(ctx.visited_node_set, expected_visited);
        EXPECT_EQ(ctx.modified_node_set, expected_modified);

        RIBNHGEntry* e260 = m_nhgmgr->getRIBNHGEntryByRIBID(260);
        ASSERT_NE(e260, nullptr);
        EXPECT_EQ(e260->getResolvedEnableGroup()[236], false);

        RIBNHGEntry* e263 = m_nhgmgr->getRIBNHGEntryByRIBID(263);
        ASSERT_NE(e263, nullptr);
        EXPECT_EQ(e263->getResolvedEnableGroup()[260], false);
        EXPECT_EQ(e263->getResolvedEnableGroup()[264], true);
    }

    /*
     * Topology 2, Remote Failure 2: 1::1 withdrawn
     * resolved_nhg_id = 263
     *
     * Expected walk:
     * - visited: {263}
     * - modified: {} (empty — no gateway match, no modified depends, ECMP so not pruned but no dependents)
     */
    TEST_F(FpmSyncdNhtBackwalk, Topology2_RemoteFailure2)
    {
        loadTopologyFromJson("fpmsyncd/test_topology_2.json");

        auto ctx = runPart1Backwalk("1::1", 263);

        std::set<uint32_t> expected_visited = {263};
        std::set<uint32_t> expected_modified = {};
        EXPECT_EQ(ctx.visited_node_set, expected_visited);
        EXPECT_EQ(ctx.modified_node_set, expected_modified);

        /* 263: enable_group untouched */
        RIBNHGEntry* e263 = m_nhgmgr->getRIBNHGEntryByRIBID(263);
        ASSERT_NE(e263, nullptr);
        EXPECT_EQ(e263->getResolvedEnableGroup()[260], true);
        EXPECT_EQ(e263->getResolvedEnableGroup()[264], true);
    }

    /*
     * Topology 2, Remote Failure 3: 2::2 withdrawn
     * resolved_nhg_id = 270 (gateway 2::2, depends=[232])
     *
     * Expected walk:
     * - visited: {270, 269}
     * - modified: {270, 269}
     * - 270: all-disabled {232:false}
     * - 269: {266:true, 270:false}
     */
    TEST_F(FpmSyncdNhtBackwalk, Topology2_RemoteFailure3)
    {
        loadTopologyFromJson("fpmsyncd/test_topology_2.json");

        auto ctx = runPart1Backwalk("2::2", 270);

        std::set<uint32_t> expected_visited = {270, 269};
        std::set<uint32_t> expected_modified = {270, 269};
        EXPECT_EQ(ctx.visited_node_set, expected_visited);
        EXPECT_EQ(ctx.modified_node_set, expected_modified);

        RIBNHGEntry* e270 = m_nhgmgr->getRIBNHGEntryByRIBID(270);
        ASSERT_NE(e270, nullptr);
        EXPECT_EQ(e270->getResolvedEnableGroup()[232], false);

        RIBNHGEntry* e269 = m_nhgmgr->getRIBNHGEntryByRIBID(269);
        ASSERT_NE(e269, nullptr);
        EXPECT_EQ(e269->getResolvedEnableGroup()[270], false);
        EXPECT_EQ(e269->getResolvedEnableGroup()[266], true);
    }

    /* ========== Topology 3 Tests ========== */

    /*
     * Topology 3, Local Failure: fc06::2 withdrawn
     * resolved_nhg_id = 238 (leaf, gateway fc06::2)
     *
     * Part 1 expected walk:
     * - visited: {238, 237}
     * - modified: {238, 237}
     * - 238: self-disabled
     * - 237: {234:true, 238:false}
     *
     * Part 2: getVrfEntries("fc06::2") -> no match
     */
    TEST_F(FpmSyncdNhtBackwalk, Topology3_LocalFailure)
    {
        auto ids = loadTopologyFromJson("fpmsyncd/test_topology_3.json");
        ASSERT_EQ(ids.size(), 6u);

        auto ctx = runPart1Backwalk("fc06::2", 238);

        std::set<uint32_t> expected_visited = {238, 237};
        std::set<uint32_t> expected_modified = {238, 237};
        EXPECT_EQ(ctx.visited_node_set, expected_visited);
        EXPECT_EQ(ctx.modified_node_set, expected_modified);

        /* 238: self disabled */
        RIBNHGEntry* e238 = m_nhgmgr->getRIBNHGEntryByRIBID(238);
        ASSERT_NE(e238, nullptr);
        EXPECT_EQ(e238->getResolvedEnableGroup()[238], false);

        /* 237: 238 disabled, 234 enabled */
        RIBNHGEntry* e237 = m_nhgmgr->getRIBNHGEntryByRIBID(237);
        ASSERT_NE(e237, nullptr);
        EXPECT_EQ(e237->getResolvedEnableGroup()[238], false);
        EXPECT_EQ(e237->getResolvedEnableGroup()[234], true);
    }

    /*
     * Topology 3, Remote Failure: 2064:100::1d withdrawn
     * resolved_nhg_id = 237 (for Part 1)
     *
     * Part 1: 237 is ECMP with no gateway match, no modified deps → walk_spec returns false.
     *   ECMP + not matched → don't prune, but dependents=[] → ends.
     *   visited: {237}, modified: {}
     *
     * Part 2: getVrfEntries("2064:100::1d") → finds NHG 240
     *   Backwalk from 240:
     *   - visited: {240, 239}
     *   - modified: {240, 239}
     *   - 240: self-disabled {240:false}
     *   - 239: {240:false, 241:true}
     */
    TEST_F(FpmSyncdNhtBackwalk, Topology3_RemoteFailure)
    {
        loadTopologyFromJson("fpmsyncd/test_topology_3.json");

        /* Part 1: walk from 237 */
        auto ctx1 = runPart1Backwalk("2064:100::1d", 237);

        std::set<uint32_t> expected_visited_p1 = {237};
        std::set<uint32_t> expected_modified_p1 = {};
        EXPECT_EQ(ctx1.visited_node_set, expected_visited_p1);
        EXPECT_EQ(ctx1.modified_node_set, expected_modified_p1);

        /* 237: untouched */
        RIBNHGEntry* e237 = m_nhgmgr->getRIBNHGEntryByRIBID(237);
        EXPECT_EQ(e237->getResolvedEnableGroup()[234], true);
        EXPECT_EQ(e237->getResolvedEnableGroup()[238], true);

        /* Part 2: walk from 240 using sonic_nhg walk spec */
        auto ctx2 = runPart2Backwalk("2064:100::1d", 240);

        std::set<uint32_t> expected_visited_p2 = {240, 239};
        std::set<uint32_t> expected_modified_p2 = {240, 239};
        EXPECT_EQ(ctx2.visited_node_set, expected_visited_p2);
        EXPECT_EQ(ctx2.modified_node_set, expected_modified_p2);

        /* 240: self disabled */
        RIBNHGEntry* e240 = m_nhgmgr->getRIBNHGEntryByRIBID(240);
        ASSERT_NE(e240, nullptr);
        EXPECT_EQ(e240->getResolvedEnableGroup()[240], false);

        /* 239: 240 disabled, 241 enabled */
        RIBNHGEntry* e239 = m_nhgmgr->getRIBNHGEntryByRIBID(239);
        ASSERT_NE(e239, nullptr);
        EXPECT_EQ(e239->getResolvedEnableGroup()[240], false);
        EXPECT_EQ(e239->getResolvedEnableGroup()[241], true);

        /* Verify updated_sonic_nhg_keys is non-empty (dedup tracking) */
        EXPECT_GE(ctx2.updated_sonic_nhg_keys.size(), 1u);
    }

    /* ========== General Tests ========== */

    /*
     * Test that m_resolved_enable_group is properly initialized from JSON topology
     */
    TEST_F(FpmSyncdNhtBackwalk, ResolvedEnableGroupInit)
    {
        loadTopologyFromJson("fpmsyncd/test_topology_1.json");

        /* Leaf 234: self-reference */
        RIBNHGEntry* e234 = m_nhgmgr->getRIBNHGEntryByRIBID(234);
        ASSERT_NE(e234, nullptr);
        ASSERT_EQ(e234->getResolvedEnableGroup().size(), 1u);
        ASSERT_EQ(e234->getResolvedEnableGroup()[234], true);

        /* Leaf 237: self-reference */
        RIBNHGEntry* e237 = m_nhgmgr->getRIBNHGEntryByRIBID(237);
        ASSERT_NE(e237, nullptr);
        ASSERT_EQ(e237->getResolvedEnableGroup().size(), 1u);
        ASSERT_EQ(e237->getResolvedEnableGroup()[237], true);

        /* ECMP 238: one entry per depends */
        RIBNHGEntry* e238 = m_nhgmgr->getRIBNHGEntryByRIBID(238);
        ASSERT_NE(e238, nullptr);
        ASSERT_EQ(e238->getResolvedEnableGroup().size(), 2u);
        ASSERT_EQ(e238->getResolvedEnableGroup()[234], true);
        ASSERT_EQ(e238->getResolvedEnableGroup()[237], true);

        /* Composite 256: depends=[257,258] */
        RIBNHGEntry* e256 = m_nhgmgr->getRIBNHGEntryByRIBID(256);
        ASSERT_NE(e256, nullptr);
        ASSERT_EQ(e256->getResolvedEnableGroup().size(), 2u);
        ASSERT_EQ(e256->getResolvedEnableGroup()[257], true);
        ASSERT_EQ(e256->getResolvedEnableGroup()[258], true);
    }

    /*
     * Test resetResolvedEnableGroup restores all to true
     */
    TEST_F(FpmSyncdNhtBackwalk, ResetEnableGroup)
    {
        loadTopologyFromJson("fpmsyncd/test_topology_1.json");

        m_nhgmgr->fib_nhg_trigger_node_quick_fixup("fc06::2", 237);

        RIBNHGEntry* e237 = m_nhgmgr->getRIBNHGEntryByRIBID(237);
        ASSERT_EQ(e237->getResolvedEnableGroup()[237], false);

        e237->resetResolvedEnableGroup();
        ASSERT_EQ(e237->getResolvedEnableGroup()[237], true);
    }

    /*
     * Test that non-existent resolved_nhg_id doesn't crash
     */
    TEST_F(FpmSyncdNhtBackwalk, NonExistentResolvedNhgId)
    {
        loadTopologyFromJson("fpmsyncd/test_topology_1.json");
        m_nhgmgr->fib_nhg_trigger_node_quick_fixup("fc06::2", 99999);
    }

    /*
     * Test resolveLeafEnableFlags() on intermediate nodes after backwalk.
     *
     * Topology 1, fc06::2 down:
     * - NHG 257 (intermediate, depends=[238], m_resolvedGroup={234,237})
     *   m_resolved_enable_group = {238: true} (depends-level, NOT leaf-level)
     *   resolveLeafEnableFlags() should walk down: 238->{234:true, 237:false}
     *   Result: {234: true, 237: false}
     *
     * - NHG 256 (composite, depends=[257,258], m_resolvedGroup={234,237})
     *   m_resolved_enable_group = {257:true, 258:true}
     *   resolveLeafEnableFlags() walks both subtrees, merges with union
     *   Result: {234: true, 237: false}
     *
     * - NHG 238 (ECMP, depends=[234,237], m_resolvedGroup={234,237})
     *   m_resolved_enable_group = {234:true, 237:false} (leaf IDs match)
     *   Result: {234: true, 237: false}
     */
    TEST_F(FpmSyncdNhtBackwalk, ResolveLeafEnableFlags)
    {
        loadTopologyFromJson("fpmsyncd/test_topology_1.json");

        /* Trigger backwalk: fc06::2 down */
        m_nhgmgr->fib_nhg_trigger_node_quick_fixup("fc06::2", 237);

        /* NHG 238 (ECMP): leaf IDs match enable_group keys directly */
        RIBNHGEntry* e238 = m_nhgmgr->getRIBNHGEntryByRIBID(238);
        ASSERT_NE(e238, nullptr);
        auto flags238 = e238->resolveLeafEnableFlags();
        EXPECT_EQ(flags238.size(), 2u);
        EXPECT_EQ(flags238[234], true);   /* fc08::2 enabled */
        EXPECT_EQ(flags238[237], false);  /* fc06::2 disabled */

        /* NHG 257 (intermediate, depends=[238]):
         * m_resolved_enable_group = {238: true} — no leaf IDs here
         * resolveLeafEnableFlags() recurses into 238 to get leaf flags */
        RIBNHGEntry* e257 = m_nhgmgr->getRIBNHGEntryByRIBID(257);
        ASSERT_NE(e257, nullptr);
        auto flags257 = e257->resolveLeafEnableFlags();
        EXPECT_EQ(flags257.size(), 2u);
        EXPECT_EQ(flags257[234], true);   /* fc08::2 reachable */
        EXPECT_EQ(flags257[237], false);  /* fc06::2 disabled in 238 */

        /* NHG 256 (composite, depends=[257,258]):
         * Both 257 and 258 point to 238, so same leaf resolution */
        RIBNHGEntry* e256 = m_nhgmgr->getRIBNHGEntryByRIBID(256);
        ASSERT_NE(e256, nullptr);
        auto flags256 = e256->resolveLeafEnableFlags();
        EXPECT_EQ(flags256.size(), 2u);
        EXPECT_EQ(flags256[234], true);
        EXPECT_EQ(flags256[237], false);

        /* NHG 234 (leaf): self-reference, still enabled */
        RIBNHGEntry* e234 = m_nhgmgr->getRIBNHGEntryByRIBID(234);
        ASSERT_NE(e234, nullptr);
        auto flags234 = e234->resolveLeafEnableFlags();
        EXPECT_EQ(flags234.size(), 1u);
        EXPECT_EQ(flags234[234], true);

        /* NHG 237 (leaf): self-reference, disabled by backwalk */
        RIBNHGEntry* e237 = m_nhgmgr->getRIBNHGEntryByRIBID(237);
        ASSERT_NE(e237, nullptr);
        auto flags237 = e237->resolveLeafEnableFlags();
        EXPECT_EQ(flags237.size(), 1u);
        EXPECT_EQ(flags237[237], false);
    }

    /*
     * Test resolveLeafEnableFlags() for remote failure where one depends
     * subtree is fully disabled.
     *
     * Topology 1, 2064:200::1e withdrawn:
     * - NHG 258 all-disabled: {238: false}
     * - NHG 256 (depends=[257,258]): 258 disabled, 257 enabled
     *   resolveLeafEnableFlags() skips 258 subtree, walks 257 -> 238 -> {234:true, 237:true}
     *   Result: {234:true, 237:true}
     */
    TEST_F(FpmSyncdNhtBackwalk, ResolveLeafEnableFlagsRemoteFailure)
    {
        loadTopologyFromJson("fpmsyncd/test_topology_1.json");

        /* Trigger: 2064:200::1e withdrawn */
        m_nhgmgr->fib_nhg_trigger_node_quick_fixup("2064:200::1e", 258);

        /* NHG 256: 258 disabled, 257 enabled -> all leaves reachable via 257 */
        RIBNHGEntry* e256 = m_nhgmgr->getRIBNHGEntryByRIBID(256);
        ASSERT_NE(e256, nullptr);
        auto flags256 = e256->resolveLeafEnableFlags();
        EXPECT_EQ(flags256.size(), 2u);
        EXPECT_EQ(flags256[234], true);   /* fc08::2 via 257->238 */
        EXPECT_EQ(flags256[237], true);   /* fc06::2 via 257->238 */

        /* NHG 258: all-disabled, resolveLeafEnableFlags skips subtree */
        RIBNHGEntry* e258 = m_nhgmgr->getRIBNHGEntryByRIBID(258);
        ASSERT_NE(e258, nullptr);
        auto flags258 = e258->resolveLeafEnableFlags();
        /* 258's depends={238}, enable_group={238:false} -> skip 238 subtree */
        EXPECT_EQ(flags258[234], false);
        EXPECT_EQ(flags258[237], false);
    }

    /* ========== Function-Level Unit Tests ========== */

    /*
     * checkNeedUpdate: if m_resolved_enable_group has any disabled entry,
     * force updated=true even when NHG fields are identical.
     */
    TEST_F(FpmSyncdNhtBackwalk, CheckNeedUpdate_DisabledEnableGroup)
    {
        loadTopologyFromJson("fpmsyncd/test_topology_1.json");

        /* Disable path 237 via backwalk */
        m_nhgmgr->fib_nhg_trigger_node_quick_fixup("fc06::2", 237);

        RIBNHGEntry* e238 = m_nhgmgr->getRIBNHGEntryByRIBID(238);
        ASSERT_NE(e238, nullptr);
        EXPECT_EQ(e238->getResolvedEnableGroup()[237], false);

        /* Now call checkNeedUpdate with the same NHG data — should detect disabled state */
        /* Re-read the NHG from topology to get identical fields */
        std::ifstream f("fpmsyncd/test_topology_1.json");
        json top_level = json::parse(f);
        fib::NextHopGroupFull nhg238;
        for (auto it = top_level.items().begin(); it != top_level.items().end(); ++it) {
            std::string json_str = it.value().dump();
            fib::NextHopGroupFull nhg;
            fib::from_json_string(json_str, nhg);
            if (nhg.id == 238) {
                nhg238 = nhg;
                break;
            }
        }

        bool updated = false;
        e238->checkNeedUpdate(nhg238, AF_INET6, updated);
        EXPECT_TRUE(updated);
    }

    /*
     * checkNeedUpdate: when all m_resolved_enable_group entries are enabled
     * and fields are identical, updated should remain false.
     */
    TEST_F(FpmSyncdNhtBackwalk, CheckNeedUpdate_AllEnabled)
    {
        loadTopologyFromJson("fpmsyncd/test_topology_1.json");

        RIBNHGEntry* e238 = m_nhgmgr->getRIBNHGEntryByRIBID(238);
        ASSERT_NE(e238, nullptr);

        /* All enable_group entries are true (no backwalk triggered) */
        EXPECT_EQ(e238->getResolvedEnableGroup()[234], true);
        EXPECT_EQ(e238->getResolvedEnableGroup()[237], true);

        std::ifstream f("fpmsyncd/test_topology_1.json");
        json top_level = json::parse(f);
        fib::NextHopGroupFull nhg238;
        for (auto it = top_level.items().begin(); it != top_level.items().end(); ++it) {
            std::string json_str = it.value().dump();
            fib::NextHopGroupFull nhg;
            fib::from_json_string(json_str, nhg);
            if (nhg.id == 238) {
                nhg238 = nhg;
                break;
            }
        }

        bool updated = false;
        e238->checkNeedUpdate(nhg238, AF_INET6, updated);
        EXPECT_FALSE(updated);
    }

    /*
     * Global map: verify entries are indexed by gateway address during addNHGFull().
     */
    TEST_F(FpmSyncdNhtBackwalk, GlobalMapAddRemove)
    {
        loadTopologyFromJson("fpmsyncd/test_topology_1.json");

        /* NHG 237 is a leaf with gateway fc06::2 — should be in global map */
        auto& global_entries = m_nhgmgr->m_rib_nhg_table->getGlobalEntries("fc06::2");
        EXPECT_GE(global_entries.size(), 1u);

        /* Verify 237 is in the set */
        RIBNHGEntry* e237 = m_nhgmgr->getRIBNHGEntryByRIBID(237);
        ASSERT_NE(e237, nullptr);
        EXPECT_NE(global_entries.find(e237), global_entries.end());

        /* Remove and verify */
        bool removed = m_nhgmgr->m_rib_nhg_table->removeGlobalEntry("fc06::2", e237);
        EXPECT_TRUE(removed);

        auto& after_remove = m_nhgmgr->m_rib_nhg_table->getGlobalEntries("fc06::2");
        EXPECT_EQ(after_remove.find(e237), after_remove.end());
    }

    /*
     * VRF map: verify VPN-context entries are indexed via addVrfEntry().
     */
    TEST_F(FpmSyncdNhtBackwalk, VrfMapAddRemove)
    {
        loadTopologyFromJson("fpmsyncd/test_topology_3.json");

        /* Topology 3 has VPN entries with gateway 2064:100::1d */
        auto& vrf_entries = m_nhgmgr->m_rib_nhg_table->getVrfEntries("2064:100::1d");
        EXPECT_GE(vrf_entries.size(), 1u);

        /* NHG 240 should be in VRF map (has sonic gateway obj) */
        RIBNHGEntry* e240 = m_nhgmgr->getRIBNHGEntryByRIBID(240);
        ASSERT_NE(e240, nullptr);
        EXPECT_NE(vrf_entries.find(e240), vrf_entries.end());

        /* Remove and verify */
        bool removed = m_nhgmgr->m_rib_nhg_table->removeVrfEntry("2064:100::1d", e240);
        EXPECT_TRUE(removed);

        auto& after = m_nhgmgr->m_rib_nhg_table->getVrfEntries("2064:100::1d");
        EXPECT_EQ(after.find(e240), after.end());
    }

    /*
     * addNHGDependents: verify bidirectional dependency graph after topology load.
     */
    TEST_F(FpmSyncdNhtBackwalk, AddNHGDependents)
    {
        loadTopologyFromJson("fpmsyncd/test_topology_1.json");

        /* ECMP 238 depends on {234, 237} — so 234's dependents should contain 238 */
        RIBNHGEntry* e234 = m_nhgmgr->getRIBNHGEntryByRIBID(234);
        ASSERT_NE(e234, nullptr);
        auto deps234 = e234->getDependentsID();
        EXPECT_NE(deps234.find(238), deps234.end());

        /* 237's dependents should contain 238 */
        RIBNHGEntry* e237 = m_nhgmgr->getRIBNHGEntryByRIBID(237);
        ASSERT_NE(e237, nullptr);
        auto deps237 = e237->getDependentsID();
        EXPECT_NE(deps237.find(238), deps237.end());

        /* 238's dependents should contain 257 (257 depends on 238) */
        RIBNHGEntry* e238 = m_nhgmgr->getRIBNHGEntryByRIBID(238);
        ASSERT_NE(e238, nullptr);
        auto deps238 = e238->getDependentsID();
        EXPECT_NE(deps238.find(257), deps238.end());
    }

    /*
     * removeNHGDependents: verify dependency is removed.
     */
    TEST_F(FpmSyncdNhtBackwalk, RemoveNHGDependents)
    {
        loadTopologyFromJson("fpmsyncd/test_topology_1.json");

        RIBNHGEntry* e234 = m_nhgmgr->getRIBNHGEntryByRIBID(234);
        ASSERT_NE(e234, nullptr);

        /* Before removal: 234's dependents contains 238 */
        auto before = e234->getDependentsID();
        EXPECT_NE(before.find(238), before.end());

        /* Remove 238 from 234's dependents */
        std::set<uint32_t> deps_238 = {234, 237};
        m_nhgmgr->m_rib_nhg_table->removeNHGDependents(deps_238, 238);

        /* After removal: 234's dependents no longer contains 238 */
        auto after = e234->getDependentsID();
        EXPECT_EQ(after.find(238), after.end());

        /* 237 also cleaned up */
        RIBNHGEntry* e237 = m_nhgmgr->getRIBNHGEntryByRIBID(237);
        auto after237 = e237->getDependentsID();
        EXPECT_EQ(after237.find(238), after237.end());
    }

    /*
     * onNhtEventMsg: verify netlink message parsing triggers backwalk
     * when curr_resolved_nhg_id == 0.
     *
     * Uses RouteSync directly with two pipelines (matching its constructor).
     */
    TEST_F(FpmSyncdNhtBackwalk, OnNhtEventMsg_Parse)
    {
        /* Build a NHT event JSON: nexthop fc06::2 unreachable, prev_resolved=237 */
        std::string nht_json = R"({"rnh_prefix":"fc06::2/128","prev_resolved_prefix":"fc06::0/64","prev_resolved_nhg_id":237,"curr_resolved_prefix":"","curr_resolved_nhg_id":0})";

        /* Build netlink message: RTM_NEWNHTEVENT with rtmsg header + NHA_JSON_STR attr */
        uint32_t json_len = static_cast<uint32_t>(nht_json.size() + 1);
        unsigned short attr_len = static_cast<unsigned short>(RTA_LENGTH(json_len));
        uint32_t msg_len = static_cast<uint32_t>(NLMSG_LENGTH(sizeof(struct rtmsg)) + RTA_ALIGN(attr_len));

        std::vector<uint8_t> buf(msg_len, 0);
        struct nlmsghdr* nlh = reinterpret_cast<struct nlmsghdr*>(buf.data());
        nlh->nlmsg_len = msg_len;
        nlh->nlmsg_type = RTM_NEWNHTEVENT;

        struct rtmsg* rtm = reinterpret_cast<struct rtmsg*>(NLMSG_DATA(nlh));
        rtm->rtm_family = AF_INET6;

        /* Add NHA_JSON_STR attribute (type=2) */
        struct rtattr* rta = reinterpret_cast<struct rtattr*>(
            reinterpret_cast<uint8_t*>(rtm) + NLMSG_ALIGN(sizeof(struct rtmsg)));
        rta->rta_type = 2;  /* NHA_JSON_STR */
        rta->rta_len = attr_len;
        memcpy(RTA_DATA(rta), nht_json.c_str(), json_len);

        int len = (int)(nlh->nlmsg_len - NLMSG_LENGTH(sizeof(struct rtmsg)));

        /* Create RouteSync with two pipelines */
        swss::DBConnector app_state_db("APPL_STATE_DB", 0);
        swss::RedisPipeline app_state_pl(&app_state_db);
        RouteSync rs(pipeline.get(), &app_state_pl);

        /* Load topology into RouteSync's NHGMgr */
        std::ifstream f("fpmsyncd/test_topology_1.json");
        json top_level = json::parse(f);
        std::map<uint32_t, fib::NextHopGroupFull> entries;
        for (auto it = top_level.items().begin(); it != top_level.items().end(); ++it) {
            std::string json_str = it.value().dump();
            fib::NextHopGroupFull nhg;
            fib::from_json_string(json_str, nhg);
            entries[nhg.id] = nhg;
        }
        std::set<uint32_t> added;
        while (added.size() < entries.size()) {
            bool progress = false;
            for (auto it = entries.begin(); it != entries.end(); ++it) {
                uint32_t id = it->first;
                fib::NextHopGroupFull& nhg = it->second;
                if (added.count(id)) continue;
                bool deps_met = true;
                for (uint32_t dep : nhg.depends) {
                    if (!added.count(dep)) { deps_met = false; break; }
                }
                if (!deps_met) continue;
                rs.m_rib_fib_nhg_mgr.addNHGFull(nhg, AF_INET6);
                added.insert(id);
                progress = true;
            }
            if (!progress) break;
        }

        /* Call onNhtEventMsg */
        rs.onNhtEventMsg(nlh, len);

        /* Verify backwalk was triggered: 237 should be disabled */
        RIBNHGEntry* e237 = rs.m_rib_fib_nhg_mgr.getRIBNHGEntryByRIBID(237);
        ASSERT_NE(e237, nullptr);
        EXPECT_EQ(e237->getResolvedEnableGroup()[237], false);
    }

    /*
     * onNhtEventMsg: when curr_resolved_nhg_id != 0, early return (no backwalk).
     */
    TEST_F(FpmSyncdNhtBackwalk, OnNhtEventMsg_NonZeroCurr)
    {
        /* JSON with curr_resolved_nhg_id != 0 */
        std::string nht_json = R"({"rnh_prefix":"fc06::2/128","prev_resolved_prefix":"fc06::0/64","prev_resolved_nhg_id":237,"curr_resolved_prefix":"fc06::0/64","curr_resolved_nhg_id":238})";

        uint32_t json_len = static_cast<uint32_t>(nht_json.size() + 1);
        unsigned short attr_len = static_cast<unsigned short>(RTA_LENGTH(json_len));
        uint32_t msg_len = static_cast<uint32_t>(NLMSG_LENGTH(sizeof(struct rtmsg)) + RTA_ALIGN(attr_len));

        std::vector<uint8_t> buf(msg_len, 0);
        struct nlmsghdr* nlh = reinterpret_cast<struct nlmsghdr*>(buf.data());
        nlh->nlmsg_len = msg_len;
        nlh->nlmsg_type = RTM_NEWNHTEVENT;

        struct rtmsg* rtm = reinterpret_cast<struct rtmsg*>(NLMSG_DATA(nlh));
        rtm->rtm_family = AF_INET6;

        struct rtattr* rta = reinterpret_cast<struct rtattr*>(
            reinterpret_cast<uint8_t*>(rtm) + NLMSG_ALIGN(sizeof(struct rtmsg)));
        rta->rta_type = 2;
        rta->rta_len = attr_len;
        memcpy(RTA_DATA(rta), nht_json.c_str(), json_len);

        int len = (int)(nlh->nlmsg_len - NLMSG_LENGTH(sizeof(struct rtmsg)));

        swss::DBConnector app_state_db("APPL_STATE_DB", 0);
        swss::RedisPipeline app_state_pl(&app_state_db);
        RouteSync rs(pipeline.get(), &app_state_pl);

        /* Load topology in dependency order */
        std::ifstream f("fpmsyncd/test_topology_1.json");
        json top_level = json::parse(f);
        std::map<uint32_t, fib::NextHopGroupFull> entries;
        for (auto it = top_level.items().begin(); it != top_level.items().end(); ++it) {
            std::string json_str = it.value().dump();
            fib::NextHopGroupFull nhg;
            fib::from_json_string(json_str, nhg);
            entries[nhg.id] = nhg;
        }
        std::set<uint32_t> added;
        while (added.size() < entries.size()) {
            bool progress = false;
            for (auto it = entries.begin(); it != entries.end(); ++it) {
                uint32_t id = it->first;
                fib::NextHopGroupFull& nhg = it->second;
                if (added.count(id)) continue;
                bool deps_met = true;
                for (uint32_t dep : nhg.depends) {
                    if (!added.count(dep)) { deps_met = false; break; }
                }
                if (!deps_met) continue;
                rs.m_rib_fib_nhg_mgr.addNHGFull(nhg, AF_INET6);
                added.insert(id);
                progress = true;
            }
            if (!progress) break;
        }

        rs.onNhtEventMsg(nlh, len);

        /* 237 should remain enabled (no backwalk triggered) */
        RIBNHGEntry* e237 = rs.m_rib_fib_nhg_mgr.getRIBNHGEntryByRIBID(237);
        ASSERT_NE(e237, nullptr);
        EXPECT_EQ(e237->getResolvedEnableGroup()[237], true);
    }

    /*
     * getNextHopGroupFields(backwalk=true): disabled leaves are filtered out.
     */
    TEST_F(FpmSyncdNhtBackwalk, GetNextHopGroupFields_BackwalkTrue)
    {
        loadTopologyFromJson("fpmsyncd/test_topology_1.json");

        /* Disable 237 (fc06::2) */
        m_nhgmgr->fib_nhg_trigger_node_quick_fixup("fc06::2", 237);

        /* NHG 238 (ECMP depends=[234,237], resolvedGroup={234,237}) */
        RIBNHGEntry* e238 = m_nhgmgr->getRIBNHGEntryByRIBID(238);
        ASSERT_NE(e238, nullptr);

        /* Call getNextHopGroupFields with backwalk=true */
        int ret = e238->getNextHopGroupFields(true);
        EXPECT_EQ(ret, 0);

        /* FV vector should only contain fc08::2 (from 234), not fc06::2 (from 237) */
        auto fv = e238->getFvVector();
        bool found_fc08 = false;
        bool found_fc06 = false;
        for (const auto& kv : fv) {
            if (fvField(kv) == "nexthop") {
                std::string val = fvValue(kv);
                if (val.find("fc08::2") != std::string::npos) found_fc08 = true;
                if (val.find("fc06::2") != std::string::npos) found_fc06 = true;
            }
        }
        EXPECT_TRUE(found_fc08);
        EXPECT_FALSE(found_fc06);
    }

    /*
     * getNextHopGroupFields(backwalk=false): all paths included regardless of state.
     */
    TEST_F(FpmSyncdNhtBackwalk, GetNextHopGroupFields_BackwalkFalse)
    {
        loadTopologyFromJson("fpmsyncd/test_topology_1.json");

        /* Disable 237 (fc06::2) */
        m_nhgmgr->fib_nhg_trigger_node_quick_fixup("fc06::2", 237);

        RIBNHGEntry* e238 = m_nhgmgr->getRIBNHGEntryByRIBID(238);
        ASSERT_NE(e238, nullptr);

        /* Call getNextHopGroupFields with backwalk=false (normal zebra path) */
        int ret = e238->getNextHopGroupFields(false);
        EXPECT_EQ(ret, 0);

        /* FV vector should contain BOTH nexthops (no filtering) */
        auto fv = e238->getFvVector();
        bool found_fc08 = false;
        bool found_fc06 = false;
        for (const auto& kv : fv) {
            if (fvField(kv) == "nexthop") {
                std::string val = fvValue(kv);
                if (val.find("fc08::2") != std::string::npos) found_fc08 = true;
                if (val.find("fc06::2") != std::string::npos) found_fc06 = true;
            }
        }
        EXPECT_TRUE(found_fc08);
        EXPECT_TRUE(found_fc06);
    }

    /*
     * writeToDB dedup: second write with same fields is skipped via m_last_appdb_fields.
     */
    TEST_F(FpmSyncdNhtBackwalk, WriteToDB_Dedup)
    {
        loadTopologyFromJson("fpmsyncd/test_topology_1.json");

        /* NHG 256 has a sonic object ID (should be writable) */
        RIBNHGEntry* e256 = m_nhgmgr->getRIBNHGEntryByRIBID(256);
        ASSERT_NE(e256, nullptr);

        /* Trigger backwalk to generate fields and write */
        m_nhgmgr->fib_nhg_trigger_node_quick_fixup("fc06::2", 237);

        /* Capture last_appdb_fields after first write */
        std::string first_fields = e256->getLastAppdbFields();
        EXPECT_FALSE(first_fields.empty());

        /* Trigger same backwalk again — resetEnableGroups first so it can re-walk */
        e256->resetResolvedEnableGroup();
        RIBNHGEntry* e238 = m_nhgmgr->getRIBNHGEntryByRIBID(238);
        e238->resetResolvedEnableGroup();
        RIBNHGEntry* e237 = m_nhgmgr->getRIBNHGEntryByRIBID(237);
        e237->resetResolvedEnableGroup();

        m_nhgmgr->fib_nhg_trigger_node_quick_fixup("fc06::2", 237);

        /* m_last_appdb_fields should be same (dedup path hit) */
        std::string second_fields = e256->getLastAppdbFields();
        EXPECT_EQ(first_fields, second_fields);
    }

    /*
     * writeToDB with changed fields: different backwalk produces different output.
     */
    TEST_F(FpmSyncdNhtBackwalk, WriteToDB_Changed)
    {
        loadTopologyFromJson("fpmsyncd/test_topology_1.json");

        /* Trigger first backwalk: fc06::2 down */
        m_nhgmgr->fib_nhg_trigger_node_quick_fixup("fc06::2", 237);

        RIBNHGEntry* e256 = m_nhgmgr->getRIBNHGEntryByRIBID(256);
        ASSERT_NE(e256, nullptr);
        std::string first_fields = e256->getLastAppdbFields();

        /* Reset and trigger different failure: 2064:200::1e down */
        auto ids = std::set<uint32_t>{234, 237, 238, 256, 257, 258, 262, 263, 264};
        resetAllEnableGroups(ids);

        m_nhgmgr->fib_nhg_trigger_node_quick_fixup("2064:200::1e", 258);

        std::string second_fields = e256->getLastAppdbFields();

        /* Fields should differ (different paths disabled) */
        EXPECT_NE(first_fields, second_fields);
    }
}
