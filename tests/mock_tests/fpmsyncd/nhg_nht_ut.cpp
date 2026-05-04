#include "ut_helpers_fpmsyncd.h"
#include "gtest/gtest.h"
#include <gmock/gmock.h>
#include "mock_table.h"
#include <net/if.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include "ipaddress.h"
#include "fpmsyncd/nhgmgr.h"
#include <iostream>
#include <fstream>
#include <nlohmann/json.hpp>
#include <nexthopgroup/nexthopgroupfull_json.h>

#define private public
#include "fpmlink.h"
#include "routesync.h"
#include "nhgmgr.h"
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
            for (auto& [key, val] : top_level.items()) {
                std::string json_str = val.dump();
                fib::NextHopGroupFull nhg;
                EXPECT_TRUE(fib::from_json_string(json_str, nhg))
                    << "Failed to parse NHG from json str for key: " << key;
                entries[nhg.id] = nhg;
            }

            /* Step 2: Topological sort — add them one by one in dependency order */
            std::set<uint32_t> added;
            std::set<uint32_t> all_ids;
            for (auto& [id, nhg] : entries) {
                all_ids.insert(id);
            }

            while (added.size() < entries.size()) {
                bool progress = false;
                for (auto& [id, nhg] : entries) {
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
    };
}

namespace ut_fpmsyncd
{
    /* ========== Topology 1 Tests ========== */

    /*
     * Topology 1, Local Failure: fc06::2 withdrawn
     * prev_resolved_nhg_id = 237
     *
     * Expected:
     * - 237: leaf, gateway match -> disabled self, skip APPDB
     * - 238: 237 in modified_node_set -> regenerate with only fc08::2
     * - 257,258,263,264: 238 in modified_node_set -> each regenerates
     * - 256: 257,258 in modified_node_set -> regenerates
     * - 262: 263,264 in modified_node_set -> regenerates
     */
    TEST_F(FpmSyncdNhtBackwalk, Topology1_LocalFailure)
    {
        auto ids = loadTopologyFromJson("fpmsyncd/test_topology_1.json");
        ASSERT_EQ(ids.size(), 9u);

        RIBNHGEntry* entry237 = m_nhgmgr->getRIBNHGEntryByRIBID(237);
        ASSERT_NE(entry237, nullptr);
        ASSERT_EQ(entry237->getResolvedEnableGroup()[237], true);

        RIBNHGEntry* entry238 = m_nhgmgr->getRIBNHGEntryByRIBID(238);
        ASSERT_NE(entry238, nullptr);
        ASSERT_EQ(entry238->getResolvedEnableGroup().size(), 2u);

        m_nhgmgr->fib_nhg_trigger_node_quick_fixup("fc06::2", 237);

        /* 237: self-reference disabled */
        ASSERT_EQ(entry237->getResolvedEnableGroup()[237], false);

        /* 238: 237 disabled, 234 still enabled */
        ASSERT_EQ(entry238->getResolvedEnableGroup()[237], false);
        ASSERT_EQ(entry238->getResolvedEnableGroup()[234], true);

        /* 238 should have been written to APPDB with only fc08::2 */
        uint32_t sonicId238 = entry238->getSonicObjID();
        if (sonicId238 > 0) {
            std::string nexthops;
            m_nextHopTable->hget(to_string(sonicId238), "nexthop", nexthops);
            EXPECT_TRUE(nexthops.find("fc08::2") != std::string::npos);
            EXPECT_TRUE(nexthops.find("fc06::2") == std::string::npos);
        }
    }

    /*
     * Topology 1, Remote Failure 1: 2064:200::1e withdrawn
     * prev_resolved_nhg_id = 258
     *
     * Expected:
     * - 258: leaf gateway match -> disabled self, skip APPDB
     * - 256: 258 in modified_node_set -> regenerate
     * - 238, 263, 264, 262 not reached
     */
    TEST_F(FpmSyncdNhtBackwalk, Topology1_RemoteFailure1)
    {
        loadTopologyFromJson("fpmsyncd/test_topology_1.json");

        m_nhgmgr->fib_nhg_trigger_node_quick_fixup("2064:200::1e", 258);

        /* 258: self disabled */
        RIBNHGEntry* e258 = m_nhgmgr->getRIBNHGEntryByRIBID(258);
        ASSERT_NE(e258, nullptr);
        ASSERT_EQ(e258->getResolvedEnableGroup()[258], false);

        /* 256: 258 disabled, 257 still enabled */
        RIBNHGEntry* e256 = m_nhgmgr->getRIBNHGEntryByRIBID(256);
        ASSERT_NE(e256, nullptr);
        ASSERT_EQ(e256->getResolvedEnableGroup()[258], false);
        ASSERT_EQ(e256->getResolvedEnableGroup()[257], true);

        /* 238 untouched */
        RIBNHGEntry* e238 = m_nhgmgr->getRIBNHGEntryByRIBID(238);
        ASSERT_EQ(e238->getResolvedEnableGroup()[234], true);
        ASSERT_EQ(e238->getResolvedEnableGroup()[237], true);
    }

    /*
     * Topology 1, Remote Failure 2: 1::1 withdrawn
     * prev_resolved_nhg_id = 263
     *
     * Expected:
     * - 263: leaf gateway match -> disabled self, skip APPDB
     * - 262: 263 disabled, 264 enabled -> regenerate
     * - Others not reached
     */
    TEST_F(FpmSyncdNhtBackwalk, Topology1_RemoteFailure2)
    {
        loadTopologyFromJson("fpmsyncd/test_topology_1.json");

        m_nhgmgr->fib_nhg_trigger_node_quick_fixup("1::1", 263);

        RIBNHGEntry* e263 = m_nhgmgr->getRIBNHGEntryByRIBID(263);
        ASSERT_NE(e263, nullptr);
        ASSERT_EQ(e263->getResolvedEnableGroup()[263], false);

        RIBNHGEntry* e262 = m_nhgmgr->getRIBNHGEntryByRIBID(262);
        ASSERT_NE(e262, nullptr);
        ASSERT_EQ(e262->getResolvedEnableGroup()[263], false);
        ASSERT_EQ(e262->getResolvedEnableGroup()[264], true);

        /* 256, 238 untouched */
        RIBNHGEntry* e256 = m_nhgmgr->getRIBNHGEntryByRIBID(256);
        ASSERT_EQ(e256->getResolvedEnableGroup()[257], true);
        ASSERT_EQ(e256->getResolvedEnableGroup()[258], true);
    }

    /* ========== Topology 2 Tests ========== */

    /*
     * Topology 2, Local Failure: fc06::2 withdrawn
     * prev_resolved_nhg_id = 235
     *
     * Expected:
     * - 235: leaf, gateway match -> disabled self
     * - 236: 235 disabled, 232 enabled -> regenerate
     * - 260,264: 236 in modified_node_set -> regenerate
     * - 263: 260,264 in modified_node_set -> regenerate
     * - 266: 235 in modified_node_set -> disabled, skip APPDB (single path)
     * - 269: 266 in modified_node_set -> regenerate
     */
    TEST_F(FpmSyncdNhtBackwalk, Topology2_LocalFailure)
    {
        auto ids = loadTopologyFromJson("fpmsyncd/test_topology_2.json");
        ASSERT_EQ(ids.size(), 9u);

        m_nhgmgr->fib_nhg_trigger_node_quick_fixup("fc06::2", 235);

        /* 235: self disabled */
        RIBNHGEntry* e235 = m_nhgmgr->getRIBNHGEntryByRIBID(235);
        ASSERT_NE(e235, nullptr);
        ASSERT_EQ(e235->getResolvedEnableGroup()[235], false);

        /* 236: 235 disabled, 232 enabled */
        RIBNHGEntry* e236 = m_nhgmgr->getRIBNHGEntryByRIBID(236);
        ASSERT_NE(e236, nullptr);
        ASSERT_EQ(e236->getResolvedEnableGroup()[235], false);
        ASSERT_EQ(e236->getResolvedEnableGroup()[232], true);

        /* 232 untouched (not in the backwalk path) */
        RIBNHGEntry* e232 = m_nhgmgr->getRIBNHGEntryByRIBID(232);
        ASSERT_EQ(e232->getResolvedEnableGroup()[232], true);
    }

    /*
     * Topology 2, Remote Failure 1: 2064:100::1d withdrawn
     * prev_resolved_nhg_id = 260
     *
     * Expected:
     * - 260: gateway match, single depends -> all disabled, skip APPDB
     * - 263: 260 in modified_node_set, 264 still enabled -> regenerate
     */
    TEST_F(FpmSyncdNhtBackwalk, Topology2_RemoteFailure1)
    {
        loadTopologyFromJson("fpmsyncd/test_topology_2.json");

        m_nhgmgr->fib_nhg_trigger_node_quick_fixup("2064:100::1d", 260);

        RIBNHGEntry* e260 = m_nhgmgr->getRIBNHGEntryByRIBID(260);
        ASSERT_NE(e260, nullptr);
        /* 260 has depends=[236], gateway match marks all disabled */
        ASSERT_EQ(e260->getResolvedEnableGroup()[236], false);

        /* 263: 260 disabled, 264 still enabled */
        RIBNHGEntry* e263 = m_nhgmgr->getRIBNHGEntryByRIBID(263);
        ASSERT_NE(e263, nullptr);
        ASSERT_EQ(e263->getResolvedEnableGroup()[260], false);
        ASSERT_EQ(e263->getResolvedEnableGroup()[264], true);
    }

    /*
     * Topology 2, Remote Failure 3: 2::2 withdrawn
     * prev_resolved_nhg_id = 270
     *
     * Expected:
     * - 270: gateway match, single depends -> all disabled, skip APPDB
     * - 269: 270 disabled, 266 enabled -> regenerate
     */
    TEST_F(FpmSyncdNhtBackwalk, Topology2_RemoteFailure3)
    {
        loadTopologyFromJson("fpmsyncd/test_topology_2.json");

        m_nhgmgr->fib_nhg_trigger_node_quick_fixup("2::2", 270);

        RIBNHGEntry* e270 = m_nhgmgr->getRIBNHGEntryByRIBID(270);
        ASSERT_NE(e270, nullptr);
        ASSERT_EQ(e270->getResolvedEnableGroup()[232], false);

        RIBNHGEntry* e269 = m_nhgmgr->getRIBNHGEntryByRIBID(269);
        ASSERT_NE(e269, nullptr);
        ASSERT_EQ(e269->getResolvedEnableGroup()[270], false);
        ASSERT_EQ(e269->getResolvedEnableGroup()[266], true);
    }

    /* ========== Topology 3 Tests ========== */

    /*
     * Topology 3, Local Failure: fc06::2 withdrawn
     * prev_resolved_nhg_id = 238
     *
     * Expected Part 1:
     * - 238: leaf, gateway match -> disabled self
     * - 237: 238 in modified_node_set -> regenerate with only fc08::2
     */
    TEST_F(FpmSyncdNhtBackwalk, Topology3_LocalFailure)
    {
        auto ids = loadTopologyFromJson("fpmsyncd/test_topology_3.json");
        ASSERT_EQ(ids.size(), 6u);

        m_nhgmgr->fib_nhg_trigger_node_quick_fixup("fc06::2", 238);

        /* 238: self disabled */
        RIBNHGEntry* e238 = m_nhgmgr->getRIBNHGEntryByRIBID(238);
        ASSERT_NE(e238, nullptr);
        ASSERT_EQ(e238->getResolvedEnableGroup()[238], false);

        /* 237: 238 disabled, 234 enabled */
        RIBNHGEntry* e237 = m_nhgmgr->getRIBNHGEntryByRIBID(237);
        ASSERT_NE(e237, nullptr);
        ASSERT_EQ(e237->getResolvedEnableGroup()[238], false);
        ASSERT_EQ(e237->getResolvedEnableGroup()[234], true);

        /* 237 APPDB should have only fc08::2 */
        uint32_t sonicId237 = e237->getSonicObjID();
        if (sonicId237 > 0) {
            std::string nexthops;
            m_nextHopTable->hget(to_string(sonicId237), "nexthop", nexthops);
            EXPECT_TRUE(nexthops.find("fc08::2") != std::string::npos);
            EXPECT_TRUE(nexthops.find("fc06::2") == std::string::npos);
        }
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
}
