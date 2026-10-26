#include "ut_helpers_fpmsyncd.h"
#include "gtest/gtest.h"
#include "mock_table.h"
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/nexthop.h>
#include <sys/socket.h> /* AF_INET */

/* NHA_JSON_STR attribute type (must match nhgWarmRestartAssist.cpp) */
#ifndef NHA_JSON_STR
#define NHA_JSON_STR 2
#endif

/* NHG message type defines (must match fpmlink.h) */
#ifndef RTM_NEWNHGFIB
#define RTM_NEWNHGFIB 5000
#endif
#ifndef RTM_DELNHGFIB
#define RTM_DELNHGFIB 5001
#endif

#ifndef RTM_NEWROUTE
#define RTM_NEWROUTE 24
#endif
#ifndef RTM_NEWLINK
#define RTM_NEWLINK 16
#endif

#define private public
#include "fpmsyncd/nhgWarmRestartAssist.h"
#undef private

using namespace swss;
using namespace testing;

namespace ut_fpmsyncd
{
    struct FpmSyncdNhgWarmAssist : public ::testing::Test
    {
        std::shared_ptr<swss::DBConnector> m_app_db;
        std::shared_ptr<swss::RedisPipeline> pipeline;
        std::shared_ptr<NHGMgr> m_nhgmgr;
        std::shared_ptr<NhgWarmRestartAssist> m_assist;
        std::shared_ptr<swss::Table> m_nextHopTable;
        std::shared_ptr<swss::Table> m_picContextTable;
        std::shared_ptr<swss::DBConnector> m_state_db;
        std::shared_ptr<swss::Table> m_stateTable;

        virtual void SetUp() override
        {
            testing_db::reset();
            m_app_db = std::make_shared<swss::DBConnector>("APPL_DB", 0);
            pipeline = std::make_shared<swss::RedisPipeline>(m_app_db.get());
            m_nhgmgr = std::make_shared<NHGMgr>(pipeline.get(),
                       APP_NEXTHOP_GROUP_TABLE_NAME, APP_PIC_CONTEXT_TABLE_NAME, true);
            m_assist = std::make_shared<NhgWarmRestartAssist>(pipeline.get(), m_nhgmgr.get());
            m_nextHopTable = std::make_shared<swss::Table>(m_app_db.get(), APP_NEXTHOP_GROUP_TABLE_NAME);
            m_picContextTable = std::make_shared<swss::Table>(m_app_db.get(), APP_PIC_CONTEXT_TABLE_NAME);
        }

        void createStateTable()
        {
            m_state_db = std::make_shared<swss::DBConnector>("APPL_STATE_DB", 0);
            m_stateTable = std::make_shared<swss::Table>(m_state_db.get(), "NHG_FULL_STATE_TABLE");
        }

        /* private 访问经 #define private public */
        NhgWarmRestartAssist::NhgWarmRestartState getWrState() { return m_assist->m_state; }
        void setWrState(NhgWarmRestartAssist::NhgWarmRestartState s) { m_assist->m_state = s; }
        std::map<sonicObjectID, NhgWarmRestartAssist::SavedNHGInfo>& getSavedNhgInfos()
        { return m_assist->m_saved_nhg_infos; }
        std::map<std::string, NhgWarmRestartAssist::AppDbNHGEntry>& getAppDbNhgFvs()
        { return m_assist->m_appdb_nhg_fvs; }
        std::set<ribID>& getReconciledIds() { return m_assist->m_reconciled_ids; }

        swss::RIBNHGTable* getRibNhgTable() { return m_nhgmgr->getRIBNHGTable(); }
    };

    TEST_F(FpmSyncdNhgWarmAssist, WarmRestart_InitSetsState)
    {
        m_assist->initWarmStart();
        EXPECT_EQ(getWrState(), NhgWarmRestartAssist::NHG_WR_INITIALIZED);
        EXPECT_TRUE(m_assist->inProgress());
    }

    TEST_F(FpmSyncdNhgWarmAssist, WarmRestart_NoneAndReconciledNotInProgress)
    {
        EXPECT_FALSE(m_assist->inProgress());
        setWrState(NhgWarmRestartAssist::NHG_WR_RECONCILED);
        EXPECT_FALSE(m_assist->inProgress());
    }

    TEST_F(FpmSyncdNhgWarmAssist, WarmRestart_InitClearsMaps)
    {
        getSavedNhgInfos()[sonicObjectID(7)] = {sonicObjectID(7), sonicObjectID(0), (uint8_t)AF_INET};
        getAppDbNhgFvs()["9"] = {sonicObjectID(9), {}, false};
        getReconciledIds().insert(ribID(3));
        m_assist->initWarmStart();
        EXPECT_TRUE(getSavedNhgInfos().empty());
        EXPECT_TRUE(getAppDbNhgFvs().empty());
        EXPECT_TRUE(getReconciledIds().empty());
    }

    // --- saveState ---

    TEST_F(FpmSyncdNhgWarmAssist, WarmRestart_SaveSingleHopSkipped)
    {
        createStateTable();

        /* Add a normal single-hop NHG (no Sonic object created) */
        auto nhg1 = createSingleIPv4NextHopNHGFull("10.0.0.1", "0.0.0.0", 100);
        m_nhgmgr->addNHGFull(nhg1, AF_INET);

        /* Save state */
        m_assist->saveState(*m_stateTable);

        /* Single-hop NHG should NOT be in state table (no Sonic object) */
        std::vector<std::string> keys;
        m_stateTable->getKeys(keys);

        /* Only NHG_ID_ALLOCATOR should be present */
        EXPECT_EQ(keys.size(), 1u);
        EXPECT_EQ(keys[0], "NHG_ID_ALLOCATOR");
    }

    TEST_F(FpmSyncdNhgWarmAssist, WarmRestart_SaveMultiHopPersisted)
    {
        createStateTable();

        /* Add single-hop NHGs first (dependencies for multi-hop) */
        auto nhg1 = createSingleIPv4NextHopNHGFull("10.0.0.1", "0.0.0.0", 100);
        auto nhg2 = createSingleIPv4NextHopNHGFull("10.0.0.2", "0.0.0.0", 200);
        m_nhgmgr->addNHGFull(nhg1, AF_INET);
        m_nhgmgr->addNHGFull(nhg2, AF_INET);

        /* Add multi-hop NHG (this creates a Sonic NHG object) */
        std::map<uint32_t, NextHopGroupFull> members = {{100, nhg1}, {200, nhg2}};
        std::map<uint32_t, uint32_t> weights = {{100, 1}, {200, 1}};
        std::map<uint32_t, uint32_t> numDirects = {{100, 0}, {200, 0}};
        auto nhg3 = createMultiNextHopNHGFull(members, weights, numDirects, {100, 200}, {}, 300);
        m_nhgmgr->addNHGFull(nhg3, AF_INET);

        /* Save state */
        m_assist->saveState(*m_stateTable);

        /* Multi-hop NHG should be in state table */
        std::string sonicIdStr;
        bool found = m_stateTable->hget("300", "sonic_nhg_id", sonicIdStr);
        EXPECT_TRUE(found);
        EXPECT_FALSE(sonicIdStr.empty());

        std::string afStr;
        m_stateTable->hget("300", "af", afStr);
        EXPECT_EQ(afStr, std::to_string(AF_INET));

        std::string jsonStr;
        m_stateTable->hget("300", "json", jsonStr);
        EXPECT_FALSE(jsonStr.empty());
    }

    TEST_F(FpmSyncdNhgWarmAssist, WarmRestart_SaveIDAllocator)
    {
        createStateTable();

        /* Add and remove some NHGs to advance the allocator */
        auto nhg1 = createSingleIPv4NextHopNHGFull("10.0.0.1", "0.0.0.0", 100);
        auto nhg2 = createSingleIPv4NextHopNHGFull("10.0.0.2", "0.0.0.0", 200);
        m_nhgmgr->addNHGFull(nhg1, AF_INET);
        m_nhgmgr->addNHGFull(nhg2, AF_INET);

        m_assist->saveState(*m_stateTable);

        std::string nextNhgId, nextPicId;
        m_stateTable->hget("NHG_ID_ALLOCATOR", "next_nhg_id", nextNhgId);
        m_stateTable->hget("NHG_ID_ALLOCATOR", "next_pic_id", nextPicId);

        EXPECT_FALSE(nextNhgId.empty());
        EXPECT_FALSE(nextPicId.empty());
    }

    // --- loadState ---

    TEST_F(FpmSyncdNhgWarmAssist, WarmRestart_LoadRestoresState)
    {
        createStateTable();

        /* Manually populate state table (simulating data from a previous save) */
        std::vector<swss::FieldValueTuple> fvs;
        fvs.emplace_back("sonic_nhg_id", "42");
        fvs.emplace_back("af", std::to_string(AF_INET));
        fvs.emplace_back("json", "{}");
        m_stateTable->set("100", fvs);

        /* Populate allocator state */
        std::vector<swss::FieldValueTuple> allocFvs;
        allocFvs.emplace_back("next_nhg_id", "50");
        allocFvs.emplace_back("next_pic_id", "10");
        m_stateTable->set("NHG_ID_ALLOCATOR", allocFvs);

        /* Also populate APP_DB with a NHG entry */
        std::vector<swss::FieldValueTuple> appFvs;
        appFvs.emplace_back("nexthop", "10.0.0.1");
        appFvs.emplace_back("ifname", "Ethernet0");
        m_nextHopTable->set("42", appFvs);

        m_assist->initWarmStart();
        m_assist->loadState(*m_stateTable, *m_nextHopTable);

        /* Verify state */
        EXPECT_EQ(getWrState(), NhgWarmRestartAssist::NHG_WR_RESTORED);
        EXPECT_FALSE(getSavedNhgInfos().empty());
        EXPECT_FALSE(getAppDbNhgFvs().empty());
    }

    TEST_F(FpmSyncdNhgWarmAssist, WarmRestart_LoadRestoresAllocator)
    {
        createStateTable();

        std::vector<swss::FieldValueTuple> allocFvs;
        allocFvs.emplace_back("next_nhg_id", "100");
        allocFvs.emplace_back("next_pic_id", "50");
        m_stateTable->set("NHG_ID_ALLOCATOR", allocFvs);

        m_assist->initWarmStart();
        m_assist->loadState(*m_stateTable, *m_nextHopTable);

        EXPECT_EQ(m_nhgmgr->getSonicIDMgr().getNextNhgID(), 100u);
        EXPECT_EQ(m_nhgmgr->getSonicIDMgr().getNextPicID(), 50u);
    }

    TEST_F(FpmSyncdNhgWarmAssist, WarmRestart_LoadCorruptedDataGraceful)
    {
        createStateTable();

        /* Write corrupted data */
        std::vector<swss::FieldValueTuple> fvs;
        fvs.emplace_back("sonic_nhg_id", "not_a_number");
        fvs.emplace_back("af", "xyz");
        m_stateTable->set("999", fvs);

        std::vector<swss::FieldValueTuple> allocFvs;
        allocFvs.emplace_back("next_nhg_id", "garbage");
        allocFvs.emplace_back("next_pic_id", "");
        m_stateTable->set("NHG_ID_ALLOCATOR", allocFvs);

        m_assist->initWarmStart();

        /* Should NOT crash */
        EXPECT_NO_THROW(m_assist->loadState(*m_stateTable, *m_nextHopTable));

        /* State should still be RESTORED (graceful degradation) */
        EXPECT_EQ(getWrState(), NhgWarmRestartAssist::NHG_WR_RESTORED);
    }

    /*
     * Build a raw netlink NHG message from a NextHopGroupFull object.
     * Mimics the format parsed by parseNHGFromRawMsg():
     * nlmsghdr + nhmsg + NHA_ID + NHA_JSON_STR
     */
    static std::vector<uint8_t> buildNHGRawMsg(const NextHopGroupFull &nhg, uint16_t cmd, uint8_t af = AF_INET)
    {
        /* Serialize NHG to JSON */
        nlohmann::ordered_json j = nhg;
        std::string jsonStr = j.dump();

        /* Calculate message sizes */
        size_t nhmsg_size = sizeof(struct nhmsg);
        size_t nha_id_size = RTA_LENGTH(sizeof(uint32_t));       /* NHA_ID attribute */
        size_t nha_json_size = RTA_LENGTH(jsonStr.size() + 1);   /* NHA_JSON_STR attribute (null-terminated) */
        size_t nlmsg_len = NLMSG_ALIGN(NLMSG_LENGTH(nhmsg_size)) + RTA_ALIGN(nha_id_size) + RTA_ALIGN(nha_json_size);

        std::vector<uint8_t> buf(nlmsg_len, 0);

        /* Fill nlmsghdr */
        struct nlmsghdr *nlh = (struct nlmsghdr *)buf.data();
        nlh->nlmsg_len = static_cast<__u32>(nlmsg_len);
        nlh->nlmsg_type = cmd;
        nlh->nlmsg_flags = NLM_F_CREATE;

        /* Fill nhmsg */
        struct nhmsg *nhm = (struct nhmsg *)NLMSG_DATA(nlh);
        nhm->nh_family = af;

        /* Add NHA_ID attribute */
        uint8_t *ptr = (uint8_t *)nlh + NLMSG_ALIGN(NLMSG_LENGTH(nhmsg_size));
        struct rtattr *rta_id = (struct rtattr *)ptr;
        rta_id->rta_type = NHA_ID;
        rta_id->rta_len = RTA_LENGTH(sizeof(uint32_t));
        *(uint32_t *)RTA_DATA(rta_id) = nhg.id;

        /* Add NHA_JSON_STR attribute */
        ptr += RTA_ALIGN(nha_id_size);
        struct rtattr *rta_json = (struct rtattr *)ptr;
        rta_json->rta_type = NHA_JSON_STR;
        rta_json->rta_len = static_cast<unsigned short>(RTA_LENGTH(jsonStr.size() + 1));
        memcpy(RTA_DATA(rta_json), jsonStr.c_str(), jsonStr.size() + 1);

        return buf;
    }

    // --- Phase 1 reconcile ---

    TEST_F(FpmSyncdNhgWarmAssist, WarmRestart_Phase1AddsSingleHop)
    {
        /* Fresh fixture has empty maps, set FSM directly */
        setWrState(NhgWarmRestartAssist::NHG_WR_RESTORED);

        /* Build raw NHG message for single-hop */
        auto nhg1 = createSingleIPv4NextHopNHGFull("10.0.0.1", "0.0.0.0", 100);
        auto rawMsg = buildNHGRawMsg(nhg1, RTM_NEWNHGFIB, AF_INET);

        std::vector<std::vector<uint8_t>> buffer;
        buffer.push_back(rawMsg);

        m_assist->m_nhg_raw_buffer = buffer;
        m_assist->reconcileNormalSingleHopNHGs();

        /* Verify NHG was added to RIB table */
        RIBNHGEntry *entry = getRibNhgTable()->getEntry(ribID(100));
        EXPECT_NE(entry, nullptr);

        /* Verify it was marked as reconciled */
        EXPECT_TRUE(getReconciledIds().count(ribID(100)));
    }

    TEST_F(FpmSyncdNhgWarmAssist, WarmRestart_Phase1SkipsMultiHop)
    {
        /* Fresh fixture has empty maps, set FSM directly */
        setWrState(NhgWarmRestartAssist::NHG_WR_RESTORED);

        /* First add single-hop dependencies */
        auto nhg1 = createSingleIPv4NextHopNHGFull("10.0.0.1", "0.0.0.0", 100);
        auto nhg2 = createSingleIPv4NextHopNHGFull("10.0.0.2", "0.0.0.0", 200);

        /* Build multi-hop NHG */
        std::map<uint32_t, NextHopGroupFull> members = {{100, nhg1}, {200, nhg2}};
        std::map<uint32_t, uint32_t> weights = {{100, 1}, {200, 1}};
        std::map<uint32_t, uint32_t> numDirects = {{100, 0}, {200, 0}};
        auto nhg3 = createMultiNextHopNHGFull(members, weights, numDirects, {100, 200}, {}, 300);

        auto rawMsg1 = buildNHGRawMsg(nhg1, RTM_NEWNHGFIB, AF_INET);
        auto rawMsg2 = buildNHGRawMsg(nhg2, RTM_NEWNHGFIB, AF_INET);
        auto rawMsg3 = buildNHGRawMsg(nhg3, RTM_NEWNHGFIB, AF_INET);

        std::vector<std::vector<uint8_t>> buffer = {rawMsg1, rawMsg2, rawMsg3};

        m_assist->m_nhg_raw_buffer = buffer;
        m_assist->reconcileNormalSingleHopNHGs();

        /* Single-hop NHGs should be reconciled */
        EXPECT_NE(getRibNhgTable()->getEntry(ribID(100)), nullptr);
        EXPECT_NE(getRibNhgTable()->getEntry(ribID(200)), nullptr);

        /* Multi-hop NHG should NOT be reconciled (left for Phase 2) */
        EXPECT_EQ(getRibNhgTable()->getEntry(ribID(300)), nullptr);
        EXPECT_FALSE(getReconciledIds().count(ribID(300)));
    }

    // --- addNHGFullWithSonicId ---

    TEST_F(FpmSyncdNhgWarmAssist, WarmRestart_AddWithSonicIdReusesId)
    {
        /* Add single-hop dependencies first */
        auto nhg1 = createSingleIPv4NextHopNHGFull("10.0.0.1", "0.0.0.0", 100);
        auto nhg2 = createSingleIPv4NextHopNHGFull("10.0.0.2", "0.0.0.0", 200);
        m_nhgmgr->addNHGFull(nhg1, AF_INET);
        m_nhgmgr->addNHGFull(nhg2, AF_INET);

        /* Pre-populate APP_DB with the expected NHG entry (simulating warm restart state) */
        /* Use sonicObjectID(42) as the reused ID */
        std::vector<swss::FieldValueTuple> appFvs;
        appFvs.emplace_back("nexthop", "10.0.0.1,10.0.0.2");
        appFvs.emplace_back("ifname", ",");
        m_nextHopTable->set("42", appFvs);

        /* Create multi-hop NHG and add with reused sonic ID */
        std::map<uint32_t, NextHopGroupFull> members = {{100, nhg1}, {200, nhg2}};
        std::map<uint32_t, uint32_t> weights = {{100, 1}, {200, 1}};
        std::map<uint32_t, uint32_t> numDirects = {{100, 0}, {200, 0}};
        auto nhg3 = createMultiNextHopNHGFull(members, weights, numDirects, {100, 200}, {}, 300);

        int ret = m_nhgmgr->addNHGFullWithSonicId(nhg3, AF_INET, sonicObjectID(42), sonicObjectID(0));
        EXPECT_EQ(ret, 0);

        /* Verify the entry uses the reused sonic ID */
        RIBNHGEntry *entry = getRibNhgTable()->getEntry(ribID(300));
        ASSERT_NE(entry, nullptr);
        EXPECT_EQ(entry->getSonicObjIDNum(), 42u);
    }

    // --- Save->Load->Reconcile E2E ---

    TEST_F(FpmSyncdNhgWarmAssist, WarmRestart_EndToEnd)
    {
        createStateTable();

        // === Phase A: Normal operation -- add NHGs ===
        auto nhg1 = createSingleIPv4NextHopNHGFull("10.0.0.1", "0.0.0.0", 100);
        auto nhg2 = createSingleIPv4NextHopNHGFull("10.0.0.2", "0.0.0.0", 200);
        m_nhgmgr->addNHGFull(nhg1, AF_INET);
        m_nhgmgr->addNHGFull(nhg2, AF_INET);

        std::map<uint32_t, NextHopGroupFull> members = {{100, nhg1}, {200, nhg2}};
        std::map<uint32_t, uint32_t> weights = {{100, 1}, {200, 1}};
        std::map<uint32_t, uint32_t> numDirects = {{100, 0}, {200, 0}};
        auto nhg3 = createMultiNextHopNHGFull(members, weights, numDirects, {100, 200}, {}, 300);
        m_nhgmgr->addNHGFull(nhg3, AF_INET);

        /* Record the sonic ID assigned to the multi-hop NHG */
        RIBNHGEntry *originalEntry = getRibNhgTable()->getEntry(ribID(300));
        ASSERT_NE(originalEntry, nullptr);
        uint32_t originalSonicId = originalEntry->getSonicObjIDNum();
        EXPECT_GT(originalSonicId, 0u);

        /* Read APP_DB entry for later comparison */
        std::vector<swss::FieldValueTuple> originalAppFvs;
        m_nextHopTable->get(std::to_string(originalSonicId), originalAppFvs);
        EXPECT_FALSE(originalAppFvs.empty());

        // === Phase B: Graceful shutdown -- save state ===
        m_assist->saveState(*m_stateTable);

        // === Phase C: Simulate restart -- recreate NHGMgr ===
        /*
         * A real restart loses only in-memory state; APP_DB and APPL_STATE_DB
         * persist. Do NOT use delEntry() here: it calls removeFromDB() and would
         * delete the APP_DB entries that warm restart relies on.
         */
        m_nhgmgr = std::make_shared<NHGMgr>(pipeline.get(), APP_NEXTHOP_GROUP_TABLE_NAME, APP_PIC_CONTEXT_TABLE_NAME, true);

        // === Phase D: Warm start -- load state ===
        /* Rebind the assist to the restarted NHGMgr (the old assist references
         * the destroyed pre-restart instance) */
        m_assist = std::make_shared<NhgWarmRestartAssist>(pipeline.get(), m_nhgmgr.get());
        m_assist->initWarmStart();
        m_assist->loadState(*m_stateTable, *m_nextHopTable);
        EXPECT_EQ(getWrState(), NhgWarmRestartAssist::NHG_WR_RESTORED);

        // === Phase E: Reconcile ===
        /* Build raw messages (simulating zebra re-sending the same NHGs) */
        auto rawMsg1 = buildNHGRawMsg(nhg1, RTM_NEWNHGFIB, AF_INET);
        auto rawMsg2 = buildNHGRawMsg(nhg2, RTM_NEWNHGFIB, AF_INET);
        auto rawMsg3 = buildNHGRawMsg(nhg3, RTM_NEWNHGFIB, AF_INET);

        std::vector<std::vector<uint8_t>> buffer = {rawMsg1, rawMsg2, rawMsg3};

        /* Phase 1: single-hop */
        m_assist->m_nhg_raw_buffer = buffer;
        m_assist->reconcileNormalSingleHopNHGs();
        EXPECT_NE(getRibNhgTable()->getEntry(ribID(100)), nullptr);
        EXPECT_NE(getRibNhgTable()->getEntry(ribID(200)), nullptr);

        /* Phase 2: multi-hop with FV matching */
        m_assist->reconcileNHGsWithSonicObj();

        // === Phase F: Verify ===
        /* Multi-hop NHG should exist */
        RIBNHGEntry *reconciledEntry = getRibNhgTable()->getEntry(ribID(300));
        ASSERT_NE(reconciledEntry, nullptr);

        EXPECT_EQ(reconciledEntry->getSonicObjIDNum(), originalSonicId);

        /* FSM should be RECONCILED */
        EXPECT_EQ(getWrState(), NhgWarmRestartAssist::NHG_WR_RECONCILED);

        /* The APP_DB entry should still exist with the same key */
        std::vector<swss::FieldValueTuple> finalAppFvs;
        m_nextHopTable->get(std::to_string(originalSonicId), finalAppFvs);
        EXPECT_FALSE(finalAppFvs.empty());
    }

    // --- message interception ---

    /* 构造一个最小 nlmsghdr，仅设置类型（buffer 只拷贝字节，不解析） */
    static std::vector<uint8_t> makeRawNlMsg(uint16_t type)
    {
        struct nlmsghdr h = {};
        h.nlmsg_len = NLMSG_LENGTH(0);
        h.nlmsg_type = type;
        return std::vector<uint8_t>((uint8_t *)&h, (uint8_t *)&h + h.nlmsg_len);
    }

    TEST_F(FpmSyncdNhgWarmAssist, WarmRestart_ShouldInterceptClassifiesTypes)
    {
        m_assist->initWarmStart();   /* FSM → INITIALIZED，in-progress */

        auto nhgMsg   = makeRawNlMsg(RTM_NEWNHGFIB);
        auto routeMsg = makeRawNlMsg(RTM_NEWROUTE);
        auto linkMsg  = makeRawNlMsg(RTM_NEWLINK);

        struct nlmsghdr *nhgH   = (struct nlmsghdr *)nhgMsg.data();
        struct nlmsghdr *routeH = (struct nlmsghdr *)routeMsg.data();
        struct nlmsghdr *linkH  = (struct nlmsghdr *)linkMsg.data();

        EXPECT_TRUE(m_assist->shouldIntercept(nhgH));
        EXPECT_TRUE(m_assist->shouldIntercept(routeH));
        EXPECT_FALSE(m_assist->shouldIntercept(linkH));
    }

    TEST_F(FpmSyncdNhgWarmAssist, WarmRestart_ShouldInterceptFalseWhenNotInProgress)
    {
        /* 未 initWarmStart，FSM = NHG_WR_NONE */
        auto nhgMsg = makeRawNlMsg(RTM_NEWNHGFIB);
        struct nlmsghdr *h = (struct nlmsghdr *)nhgMsg.data();
        EXPECT_FALSE(m_assist->shouldIntercept(h));

        m_assist->bufferNHG(h);   /* 非 in-progress 时不应累积 */
        EXPECT_TRUE(m_assist->m_nhg_raw_buffer.empty());
    }
}
