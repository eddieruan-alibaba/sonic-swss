#include <net/if.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <iostream>
#include <cstring>

#define private public
#include "nhgmgr.h"
#include "nhg_warm_restart_assist.h"
#undef private

#include "ut_helpers_fpmsyncd.h"
#include "gtest/gtest.h"
#include <gmock/gmock.h>
#include "mock_table.h"

using namespace swss;
using namespace testing;

namespace ut_fpmsyncd
{

struct NHGWarmRestartAssistTest : public ::testing::Test
{
    std::shared_ptr<swss::DBConnector> m_app_db;
    std::shared_ptr<swss::DBConnector> m_state_db;
    std::shared_ptr<swss::RedisPipeline> m_pipeline;
    std::shared_ptr<NHGMgr> m_nhgMgr;
    std::unique_ptr<NHGWarmRestartAssist> m_assist;
    std::shared_ptr<swss::Table> m_warmStateTable;

    virtual void SetUp() override
    {
        testing_db::reset();

        m_app_db = std::make_shared<swss::DBConnector>("APPL_DB", 0);
        m_state_db = std::make_shared<swss::DBConnector>("STATE_DB", 0);
        m_pipeline = std::make_shared<swss::RedisPipeline>(m_app_db.get());
        m_nhgMgr = std::make_shared<NHGMgr>(m_pipeline.get(),
            APP_NEXTHOP_GROUP_TABLE_NAME, APP_PIC_CONTEXT_TABLE_NAME, true);

        m_warmStateTable = std::make_shared<swss::Table>(m_state_db.get(),
            FPMSYNCD_NHG_WARM_STATE_TABLE);
    }

    virtual void TearDown() override
    {
    }

    void createAssist()
    {
        m_assist = std::make_unique<NHGWarmRestartAssist>(
            m_pipeline.get(), "fpmsyncd", "bgp", m_nhgMgr.get(), m_state_db.get());
    }

    void populateStateDB(uint32_t zebra_id, uint32_t sonic_obj_id,
                         bool is_single, uint8_t af,
                         const std::string &key_hash)
    {
        std::vector<FieldValueTuple> fvs;
        fvs.emplace_back("sonic_obj_id", std::to_string(sonic_obj_id));
        fvs.emplace_back("sonic_pic_id", "0");
        fvs.emplace_back("sonic_obj_type", std::to_string(SONIC_NHG_OBJ_TYPE_NHG_NORMAL));
        fvs.emplace_back("is_single", is_single ? "1" : "0");
        fvs.emplace_back("is_shared", "0");
        fvs.emplace_back("af", std::to_string(af));
        fvs.emplace_back("sonic_nhg_key_hash", key_hash);

        m_warmStateTable->set(std::to_string(zebra_id), fvs);
    }
};

TEST_F(NHGWarmRestartAssistTest, ReadNHGStateFromDBEmpty)
{
    createAssist();
    m_assist->readNHGStateFromDB();

    EXPECT_TRUE(m_assist->isNHGWarmStartInProgress());
    EXPECT_EQ(m_assist->m_nhgCacheMap.size(), 0u);
}

TEST_F(NHGWarmRestartAssistTest, ReadNHGStateFromDBWithEntries)
{
    populateStateDB(100, 1, true, AF_INET, "test_hash_100");
    populateStateDB(200, 2, false, AF_INET6, "test_hash_200");

    createAssist();
    m_assist->readNHGStateFromDB();

    EXPECT_TRUE(m_assist->isNHGWarmStartInProgress());
    EXPECT_EQ(m_assist->m_nhgCacheMap.size(), 2u);

    auto it100 = m_assist->m_nhgCacheMap.find(100);
    ASSERT_NE(it100, m_assist->m_nhgCacheMap.end());
    EXPECT_EQ(it100->second.zebra_id, 100u);
    EXPECT_EQ(it100->second.sonic_obj_id, 1u);
    EXPECT_TRUE(it100->second.is_single);
    EXPECT_EQ(it100->second.af, AF_INET);
    EXPECT_EQ(it100->second.sonic_nhg_key_hash, "test_hash_100");
    EXPECT_EQ(it100->second.state, NHGWarmRestartAssist::NHG_STALE);

    auto it200 = m_assist->m_nhgCacheMap.find(200);
    ASSERT_NE(it200, m_assist->m_nhgCacheMap.end());
    EXPECT_EQ(it200->second.zebra_id, 200u);
    EXPECT_FALSE(it200->second.is_single);
    EXPECT_EQ(it200->second.state, NHGWarmRestartAssist::NHG_STALE);
}

TEST_F(NHGWarmRestartAssistTest, ReadNHGStateSkipsIDStateKey)
{
    std::vector<FieldValueTuple> fvs;
    fvs.emplace_back("nhg_next_id", "10");
    fvs.emplace_back("pic_next_id", "5");
    m_warmStateTable->set(FPMSYNCD_NHG_WARM_ID_STATE_KEY, fvs);

    populateStateDB(100, 1, true, AF_INET, "hash_100");

    createAssist();
    m_assist->readNHGStateFromDB();

    EXPECT_EQ(m_assist->m_nhgCacheMap.size(), 1u);
    EXPECT_NE(m_assist->m_nhgCacheMap.find(100), m_assist->m_nhgCacheMap.end());
}

TEST_F(NHGWarmRestartAssistTest, InsertNHGToMapNewEntry)
{
    createAssist();
    m_assist->m_nhgWarmStartInProgress = true;

    NextHopGroupFull nhg = createSingleIPv4NextHopNHGFull("10.0.0.1", "192.168.1.1", 50);
    m_assist->insertNHGToMap(nhg, AF_INET, false);

    auto it = m_assist->m_nhgCacheMap.find(50);
    ASSERT_NE(it, m_assist->m_nhgCacheMap.end());
    EXPECT_EQ(it->second.state, NHGWarmRestartAssist::NHG_NEW);
    EXPECT_TRUE(it->second.is_single);

    auto pendIt = m_assist->m_pendingEntries.find(50);
    ASSERT_NE(pendIt, m_assist->m_pendingEntries.end());
    EXPECT_FALSE(pendIt->second.is_delete);
}

TEST_F(NHGWarmRestartAssistTest, InsertNHGToMapDeleteEntry)
{
    createAssist();
    m_assist->m_nhgWarmStartInProgress = true;

    NHGWarmRestartAssist::NHGCacheEntry existing;
    existing.zebra_id = 60;
    existing.sonic_obj_id = 5;
    existing.sonic_pic_id = 0;
    existing.sonic_obj_type = SONIC_NHG_OBJ_TYPE_NHG_NORMAL;
    existing.is_single = true;
    existing.is_shared = false;
    existing.af = AF_INET;
    existing.state = NHGWarmRestartAssist::NHG_STALE;
    m_assist->m_nhgCacheMap[60] = existing;

    NextHopGroupFull delNhg;
    delNhg.id = 60;
    m_assist->insertNHGToMap(delNhg, AF_INET, true);

    auto it = m_assist->m_nhgCacheMap.find(60);
    ASSERT_NE(it, m_assist->m_nhgCacheMap.end());
    EXPECT_EQ(it->second.state, NHGWarmRestartAssist::NHG_DELETE);
}

TEST_F(NHGWarmRestartAssistTest, InsertNHGToMapSameEntry)
{
    createAssist();
    m_assist->m_nhgWarmStartInProgress = true;

    NextHopGroupFull nhg = createSingleIPv4NextHopNHGFull("10.0.0.1", "192.168.1.1", 70);

    SonicNHGObjectKey key = NHGWarmRestartAssist::buildKeyFromNHGFull(nhg, AF_INET);
    std::string keyHash = key.serialize();

    NHGWarmRestartAssist::NHGCacheEntry existing;
    existing.zebra_id = 70;
    existing.sonic_obj_id = 7;
    existing.sonic_pic_id = 0;
    existing.sonic_obj_type = SONIC_NHG_OBJ_TYPE_NHG_NORMAL;
    existing.is_single = true;
    existing.is_shared = false;
    existing.af = AF_INET;
    existing.sonic_nhg_key_hash = keyHash;
    existing.state = NHGWarmRestartAssist::NHG_STALE;
    m_assist->m_nhgCacheMap[70] = existing;

    m_assist->insertNHGToMap(nhg, AF_INET, false);

    auto it = m_assist->m_nhgCacheMap.find(70);
    ASSERT_NE(it, m_assist->m_nhgCacheMap.end());
    EXPECT_EQ(it->second.state, NHGWarmRestartAssist::NHG_SAME);
}

TEST_F(NHGWarmRestartAssistTest, InsertNHGToMapChangedEntry)
{
    createAssist();
    m_assist->m_nhgWarmStartInProgress = true;

    NHGWarmRestartAssist::NHGCacheEntry existing;
    existing.zebra_id = 80;
    existing.sonic_obj_id = 8;
    existing.sonic_pic_id = 0;
    existing.sonic_obj_type = SONIC_NHG_OBJ_TYPE_NHG_NORMAL;
    existing.is_single = true;
    existing.is_shared = false;
    existing.af = AF_INET;
    existing.sonic_nhg_key_hash = "old_hash_that_wont_match";
    existing.state = NHGWarmRestartAssist::NHG_STALE;
    m_assist->m_nhgCacheMap[80] = existing;

    NextHopGroupFull nhg = createSingleIPv4NextHopNHGFull("10.0.0.99", "192.168.1.1", 80);
    m_assist->insertNHGToMap(nhg, AF_INET, false);

    auto it = m_assist->m_nhgCacheMap.find(80);
    ASSERT_NE(it, m_assist->m_nhgCacheMap.end());
    EXPECT_EQ(it->second.state, NHGWarmRestartAssist::NHG_NEW);

    auto pendIt = m_assist->m_pendingEntries.find(80);
    ASSERT_NE(pendIt, m_assist->m_pendingEntries.end());
}

TEST_F(NHGWarmRestartAssistTest, CompareWithIncomingEmptyHash)
{
    createAssist();

    NHGWarmRestartAssist::NHGCacheEntry entry;
    entry.sonic_nhg_key_hash = "";

    SonicNHGObjectKey key;
    key.type = SONIC_NHG_OBJ_TYPE_NHG_NORMAL;

    EXPECT_FALSE(m_assist->compareWithIncoming(entry, key));
}

TEST_F(NHGWarmRestartAssistTest, BuildKeyFromNHGFullSingleIPv4)
{
    NextHopGroupFull nhg = createSingleIPv4NextHopNHGFull("10.1.2.3", "192.168.0.1", 100);

    SonicNHGObjectKey key = NHGWarmRestartAssist::buildKeyFromNHGFull(nhg, AF_INET);

    EXPECT_EQ(key.type, SONIC_NHG_OBJ_TYPE_NHG_NORMAL);
    EXPECT_EQ(key.nexthop, "10.1.2.3");
    EXPECT_FALSE(key.ifName.empty());
    EXPECT_TRUE(key.groupMember.empty());
}

TEST_F(NHGWarmRestartAssistTest, BuildKeyFromNHGFullSingleIPv6)
{
    NextHopGroupFull nhg = createSingleIPv6NextHopNHGFull("fc00::1", "fc00::100", 101);

    SonicNHGObjectKey key = NHGWarmRestartAssist::buildKeyFromNHGFull(nhg, AF_INET6);

    EXPECT_EQ(key.type, SONIC_NHG_OBJ_TYPE_NHG_NORMAL);
    EXPECT_EQ(key.nexthop, "fc00::1");
    EXPECT_FALSE(key.ifName.empty());
    EXPECT_TRUE(key.groupMember.empty());
}

TEST_F(NHGWarmRestartAssistTest, BuildKeyFromNHGFullMultiNexthop)
{
    uint32_t ribIDA = 10;
    uint32_t ribIDB = 11;
    uint32_t ribIDMulti = 12;

    NextHopGroupFull nhgA = createSingleIPv4NextHopNHGFull("10.0.0.1", "1.1.1.1", ribIDA);
    NextHopGroupFull nhgB = createSingleIPv4NextHopNHGFull("10.0.0.2", "1.1.1.2", ribIDB);

    std::map<uint32_t, NextHopGroupFull> nhgFullList = {
        {ribIDA, nhgA},
        {ribIDB, nhgB}
    };
    std::map<uint32_t, uint32_t> weights = {
        {ribIDA, 1},
        {ribIDB, 1}
    };
    std::map<uint32_t, uint32_t> numDirects = {
        {ribIDA, 0},
        {ribIDB, 0}
    };
    std::vector<uint32_t> depends = {ribIDA, ribIDB};
    std::vector<uint32_t> dependents = {};

    NextHopGroupFull nhgMulti = createMultiNextHopNHGFull(
        nhgFullList, weights, numDirects, depends, dependents, ribIDMulti);

    SonicNHGObjectKey key = NHGWarmRestartAssist::buildKeyFromNHGFull(nhgMulti, AF_INET);

    EXPECT_EQ(key.type, SONIC_NHG_OBJ_TYPE_NHG_NORMAL);
    EXPECT_TRUE(key.nexthop.empty());
    EXPECT_TRUE(key.ifName.empty());
    EXPECT_EQ(key.groupMember.size(), 2u);
}

TEST_F(NHGWarmRestartAssistTest, BuildKeyFromNHGFullSRv6)
{
    NextHopGroupFull nhg = createSingleSRv6VPNNextHopNHGFull(
        "fc00:0:1::1", "fc00:0:2::1", "fc00::100", 102);

    SonicNHGObjectKey key = NHGWarmRestartAssist::buildKeyFromNHGFull(nhg, AF_INET6);

    EXPECT_EQ(key.type, SONIC_NHG_OBJ_TYPE_NHG_NORMAL);
    EXPECT_FALSE(key.vpnSid.empty());
    EXPECT_FALSE(key.segSrc.empty());
}

TEST_F(NHGWarmRestartAssistTest, ReconcileNHGAllStale)
{
    createAssist();
    m_assist->m_nhgWarmStartInProgress = true;

    NextHopGroupFull nhg = createSingleIPv4NextHopNHGFull("10.0.0.1", "1.1.1.1", 200);
    ASSERT_EQ(m_nhgMgr->addNHGFull(nhg, AF_INET), 0);

    NHGWarmRestartAssist::NHGCacheEntry entry;
    entry.zebra_id = 200;
    entry.sonic_obj_id = 1;
    entry.sonic_pic_id = 0;
    entry.sonic_obj_type = SONIC_NHG_OBJ_TYPE_NHG_NORMAL;
    entry.is_single = true;
    entry.is_shared = false;
    entry.af = AF_INET;
    entry.state = NHGWarmRestartAssist::NHG_STALE;
    m_assist->m_nhgCacheMap[200] = entry;

    m_assist->reconcileNHG();

    EXPECT_FALSE(m_assist->isNHGWarmStartInProgress());
    EXPECT_TRUE(m_assist->m_nhgCacheMap.empty());

    RIBNHGEntry *ribEntry = m_nhgMgr->getRIBNHGEntryByRIBID(200);
    EXPECT_EQ(ribEntry, nullptr);
}

TEST_F(NHGWarmRestartAssistTest, ReconcileNHGSameNoChange)
{
    createAssist();
    m_assist->m_nhgWarmStartInProgress = true;

    NextHopGroupFull nhg = createSingleIPv4NextHopNHGFull("10.0.0.5", "1.1.1.5", 300);
    ASSERT_EQ(m_nhgMgr->addNHGFull(nhg, AF_INET), 0);

    RIBNHGEntry *entryBefore = m_nhgMgr->getRIBNHGEntryByRIBID(300);
    ASSERT_NE(entryBefore, nullptr);
    uint32_t sonicIdBefore = entryBefore->getSonicObjID();

    NHGWarmRestartAssist::NHGCacheEntry cacheEntry;
    cacheEntry.zebra_id = 300;
    cacheEntry.sonic_obj_id = sonicIdBefore;
    cacheEntry.sonic_pic_id = 0;
    cacheEntry.sonic_obj_type = SONIC_NHG_OBJ_TYPE_NHG_NORMAL;
    cacheEntry.is_single = true;
    cacheEntry.is_shared = false;
    cacheEntry.af = AF_INET;
    cacheEntry.state = NHGWarmRestartAssist::NHG_SAME;
    m_assist->m_nhgCacheMap[300] = cacheEntry;

    m_assist->reconcileNHG();

    EXPECT_FALSE(m_assist->isNHGWarmStartInProgress());
    RIBNHGEntry *entryAfter = m_nhgMgr->getRIBNHGEntryByRIBID(300);
    ASSERT_NE(entryAfter, nullptr);
    EXPECT_EQ(entryAfter->getSonicObjID(), sonicIdBefore);
}

TEST_F(NHGWarmRestartAssistTest, ReconcileNHGNewReplaces)
{
    createAssist();
    m_assist->m_nhgWarmStartInProgress = true;

    NextHopGroupFull nhgOld = createSingleIPv4NextHopNHGFull("10.0.0.1", "1.1.1.1", 400);
    ASSERT_EQ(m_nhgMgr->addNHGFull(nhgOld, AF_INET), 0);

    NextHopGroupFull nhgNew = createSingleIPv4NextHopNHGFull("10.0.0.99", "1.1.1.1", 400);

    NHGWarmRestartAssist::NHGCacheEntry cacheEntry;
    cacheEntry.zebra_id = 400;
    cacheEntry.sonic_obj_id = 1;
    cacheEntry.sonic_pic_id = 0;
    cacheEntry.sonic_obj_type = SONIC_NHG_OBJ_TYPE_NHG_NORMAL;
    cacheEntry.is_single = true;
    cacheEntry.is_shared = false;
    cacheEntry.af = AF_INET;
    cacheEntry.state = NHGWarmRestartAssist::NHG_NEW;
    m_assist->m_nhgCacheMap[400] = cacheEntry;

    NHGWarmRestartAssist::PendingNHGEntry pending;
    pending.nhg = nhgNew;
    pending.af = AF_INET;
    pending.is_delete = false;
    m_assist->m_pendingEntries[400] = pending;

    m_assist->reconcileNHG();

    EXPECT_FALSE(m_assist->isNHGWarmStartInProgress());
    RIBNHGEntry *entry = m_nhgMgr->getRIBNHGEntryByRIBID(400);
    ASSERT_NE(entry, nullptr);
}

TEST_F(NHGWarmRestartAssistTest, ReconcileOrdersSingleBeforeMulti)
{
    createAssist();
    m_assist->m_nhgWarmStartInProgress = true;

    NHGWarmRestartAssist::NHGCacheEntry singleEntry;
    singleEntry.zebra_id = 500;
    singleEntry.sonic_obj_id = 10;
    singleEntry.sonic_pic_id = 0;
    singleEntry.sonic_obj_type = SONIC_NHG_OBJ_TYPE_NHG_NORMAL;
    singleEntry.is_single = true;
    singleEntry.is_shared = false;
    singleEntry.af = AF_INET;
    singleEntry.state = NHGWarmRestartAssist::NHG_SAME;
    m_assist->m_nhgCacheMap[500] = singleEntry;

    NHGWarmRestartAssist::NHGCacheEntry multiEntry;
    multiEntry.zebra_id = 600;
    multiEntry.sonic_obj_id = 20;
    multiEntry.sonic_pic_id = 0;
    multiEntry.sonic_obj_type = SONIC_NHG_OBJ_TYPE_NHG_NORMAL;
    multiEntry.is_single = false;
    multiEntry.is_shared = false;
    multiEntry.af = AF_INET;
    multiEntry.state = NHGWarmRestartAssist::NHG_SAME;
    m_assist->m_nhgCacheMap[600] = multiEntry;

    m_assist->reconcileNHG();

    EXPECT_FALSE(m_assist->isNHGWarmStartInProgress());
    EXPECT_TRUE(m_assist->m_nhgCacheMap.empty());
    EXPECT_TRUE(m_assist->m_pendingEntries.empty());
}

TEST_F(NHGWarmRestartAssistTest, CreateSonicNormalNHGObjectKeyMultiNexthop)
{
    uint32_t ribIDA = 10;
    uint32_t ribIDB = 11;
    uint32_t ribIDMulti = 12;

    NextHopGroupFull nhgA = createSingleIPv4NextHopNHGFull("10.0.0.1", "1.1.1.1", ribIDA);
    NextHopGroupFull nhgB = createSingleIPv4NextHopNHGFull("10.0.0.2", "1.1.1.2", ribIDB);

    std::map<uint32_t, NextHopGroupFull> nhgFullList = {
        {ribIDA, nhgA},
        {ribIDB, nhgB}
    };
    std::map<uint32_t, uint32_t> weights = {
        {ribIDA, 1},
        {ribIDB, 2}
    };
    std::map<uint32_t, uint32_t> numDirects = {
        {ribIDA, 0},
        {ribIDB, 0}
    };
    std::vector<uint32_t> depends = {ribIDA, ribIDB};
    std::vector<uint32_t> dependents = {};

    NextHopGroupFull nhgMulti = createMultiNextHopNHGFull(
        nhgFullList, weights, numDirects, depends, dependents, ribIDMulti);

    SonicNHGObjectKey key = NHGWarmRestartAssist::buildKeyFromNHGFull(nhgMulti, AF_INET);

    EXPECT_EQ(key.groupMember.size(), 2u);
    bool foundA = false, foundB = false;
    for (const auto &m : key.groupMember)
    {
        if (m.first == ribIDA && m.second == 1) foundA = true;
        if (m.first == ribIDB && m.second == 2) foundB = true;
    }
    EXPECT_TRUE(foundA);
    EXPECT_TRUE(foundB);
}

}
