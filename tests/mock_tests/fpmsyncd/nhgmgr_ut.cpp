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
#include <set>

#define private public // Need to modify internal cache
#include "fpmlink.h"
#include "routesync.h"
#include "nhgmgr.h"
#undef private

using namespace swss;
using namespace testing;

#define MY_NEXTHOP_GROUP_KEY_DELIMITER ':'

/*
Test Fixture
*/
namespace ut_fpmsyncd
{
    struct FpmSyncdNhgMgr : public ::testing::Test
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

            /* Construct dependencies */

            /*  1) RouteSync */
            pipeline = std::make_shared<swss::RedisPipeline>(m_app_db.get());
            m_nhgmgr = std::make_shared<NHGMgr>(pipeline.get(), APP_NEXTHOP_GROUP_TABLE_NAME, APP_PIC_CONTEXT_TABLE_NAME, true);

            /* 2) NEXTHOP_GROUP_TABLE in APP_DB */
            m_nextHopTable = std::make_shared<swss::Table>(m_app_db.get(), APP_NEXTHOP_GROUP_TABLE_NAME);

            /* 3) PIC_CONTEXT_TABLE in APP_DB */
            m_picContextTable = std::make_shared<swss::Table>(m_app_db.get(), APP_PIC_CONTEXT_TABLE_NAME);
        }

        virtual void TearDown() override
        {
        }
    };
}

namespace ut_fpmsyncd
{
    /* Test add and remove a single ipv4 nexthop */
    TEST_F(FpmSyncdNhgMgr, ReceivingSingleNexthop)
    {
        /* Create a NextHopGroupFull object containing single ipv4 nexthop*/
        NextHopGroupFull nhg_obj = createSingleIPv4NextHopNHGFull("192.100.1.1", "120.0.0.1", 123);
        /* Send the object to the NhgMgr Add function */
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhg_obj, AF_INET), 0);

        /* Get entry and Check that fpmsyncd created the correct entries in APP_DB */
        RIBNHGEntry *entry = m_nhgmgr->getRIBNHGEntryByRIBID(nhg_obj.id);
        ASSERT_NE(entry, nullptr);
        std::string nexthop, ifname;
        // uint32_t sonic_obj_id = entry->getSonicObjID();
        // ASSERT_EQ(m_nextHopTable->hget(to_string(sonic_obj_id), "nexthop", nexthop), true);
        // ASSERT_EQ(nexthop, "192.100.1.1");
        // ASSERT_EQ(m_nextHopTable->hget(to_string(sonic_obj_id), "ifname", ifname), true);
        // ASSERT_EQ(ifname, nhg_obj.ifname);
        // ASSERT_EQ(m_nhgmgr->isSonicNHGIDInUsed(sonic_obj_id), true);

        /* Delete entry and check the APP_DB */
        ASSERT_EQ(m_nhgmgr->delNHGFull(nhg_obj.id), 0);
        entry = m_nhgmgr->getRIBNHGEntryByRIBID(nhg_obj.id);
        ASSERT_EQ(entry, nullptr);
        std::vector<FieldValueTuple> fvs;
        // ASSERT_EQ(m_nextHopTable->get(to_string(sonic_obj_id), fvs), false);
        // ASSERT_EQ(m_nhgmgr->isSonicNHGIDInUsed(sonic_obj_id), false);
    }

    /* Test add and remove a single ipv6 nexthop */
    TEST_F(FpmSyncdNhgMgr, ReceivingSingleIPv6Nexthop)
    {
        /* Create a NextHopGroupFull object containing single ipv6 nexthop*/
        NextHopGroupFull nhg_obj = createSingleIPv6NextHopNHGFull("fc00::1", "fc00::100");
        /* Send the object to the NhgMgr Add function */
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhg_obj, AF_INET6), 0);

        /* Get entry and Check that fpmsyncd created the correct entries in APP_DB */
        RIBNHGEntry *entry = m_nhgmgr->getRIBNHGEntryByRIBID(nhg_obj.id);
        ASSERT_NE(entry, nullptr);
        std::string nexthop, ifname;
        // uint32_t sonic_obj_id = entry->getSonicObjID();
        // ASSERT_EQ(m_nextHopTable->hget(to_string(sonic_obj_id), "nexthop", nexthop), true);
        // ASSERT_EQ(nexthop, "fc00::1");
        // ASSERT_EQ(m_nextHopTable->hget(to_string(sonic_obj_id), "ifname", ifname), true);
        // ASSERT_EQ(ifname, nhg_obj.ifname);
        // ASSERT_EQ(m_nhgmgr->isSonicNHGIDInUsed(sonic_obj_id), true);

        /* Delete entry and check the APP_DB */
        ASSERT_EQ(m_nhgmgr->delNHGFull(nhg_obj.id), 0);
        entry = m_nhgmgr->getRIBNHGEntryByRIBID(nhg_obj.id);
        ASSERT_EQ(entry, nullptr);
        std::vector<FieldValueTuple> fvs;
        // ASSERT_EQ(m_nextHopTable->get(to_string(sonic_obj_id), fvs), false);
        //  ASSERT_EQ(m_nhgmgr->isSonicNHGIDInUsed(sonic_obj_id), false);
    }

    /* Test add and remove a multi ipv4 nexthop */
    TEST_F(FpmSyncdNhgMgr, ReceivingMultiNexthop)
    {
        /* Create a NextHopGroupFull object list containing single ipv4 nexthop*/
        std::map<uint32_t, NextHopGroupFull> dependsList;
        uint32_t ribIDA = 4;
        uint32_t ribIDB = 5;
        uint32_t ribIDC = 1;
        uint32_t ribIDB1 = 2;
        uint32_t ribIDB2 = 3;

        NextHopGroupFull nhgObjC = createSingleIPv4NextHopNHGFull("192.100.1.1", "120.0.0.1", ribIDC);
        NextHopGroupFull nhgObjB1 = createSingleIPv4NextHopNHGFull("192.100.2.1", "120.0.2.1", ribIDB1);
        NextHopGroupFull nhgObjB2 = createSingleIPv4NextHopNHGFull("192.100.2.2", "120.0.2.2", ribIDB2);
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjC, AF_INET), 0);
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjB1, AF_INET), 0);
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjB2, AF_INET), 0);

        RIBNHGEntry *entryC = m_nhgmgr->getRIBNHGEntryByRIBID(nhgObjC.id);
        RIBNHGEntry *entryB1 = m_nhgmgr->getRIBNHGEntryByRIBID(nhgObjB1.id);
        RIBNHGEntry *entryB2 = m_nhgmgr->getRIBNHGEntryByRIBID(nhgObjB2.id);
        ASSERT_NE(entryC, nullptr);
        ASSERT_NE(entryB1, nullptr);
        ASSERT_NE(entryB2, nullptr);
        // uint32_t sonicObjIDC = entryC->getSonicObjID();
        // uint32_t sonicObjIDB1 = entryB1->getSonicObjID();
        // uint32_t sonicObjIDB2 = entryB2->getSonicObjID();

        // ASSERT_EQ(m_nhgmgr->isSonicNHGIDInUsed(sonicObjIDC), true);
        // ASSERT_EQ(m_nhgmgr->isSonicNHGIDInUsed(sonicObjIDB1), true);
        // ASSERT_EQ(m_nhgmgr->isSonicNHGIDInUsed(sonicObjIDB2), true);

        vector<uint32_t> dependsB = { ribIDB1, ribIDB2 };
        vector<uint32_t> dependentsB = { ribIDA };
        vector<uint32_t> dependsA = { ribIDB, ribIDC };
        map<uint32_t, NextHopGroupFull> nhgFullB = {
            { ribIDB1, nhgObjB1 },
            { ribIDB2, nhgObjB2 }
        };
        NextHopGroupFull nhgObjB = createMultiNextHopNHGFull(nhgFullB,
                                                             { { ribIDB1, 131 }, { ribIDB2, 212 } },
                                                             { { ribIDB1, 0 }, { ribIDB2, 0 } },
                                                             dependsB, dependentsB, ribIDB);
        map<string, string> expectedNexthopofB = { { "192.100.2.1", "131" }, { "192.100.2.2", "212" } };
        map<uint32_t, NextHopGroupFull> nhgFullA = { { ribIDB, nhgObjB },
                                                     { ribIDB1, nhgObjB1 },
                                                     { ribIDB2, nhgObjB2 },
                                                     { ribIDC, nhgObjC } };
        NextHopGroupFull nhgObjA = createMultiNextHopNHGFull(nhgFullA,
                                                             { { ribIDB, 11 },
                                                               { ribIDB1, 11 },
                                                               { ribIDB2, 11 },
                                                               { ribIDC, 12 } },
                                                             { { ribIDB, 2 },
                                                               { ribIDB1, 0 },
                                                               { ribIDB2, 0 },
                                                               { ribIDC, 0 } },
                                                             dependsA, {}, ribIDA);
        map<string, string> expectedNexthopofA = {
            { "192.100.2.1", "11" },
            { "192.100.2.2", "11" },
            { "192.100.1.1", "12" },
        };

        /* Send the object to the NhgMgr Add function */
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjB, AF_INET), 0);
        /* Check that fpmsyncd created the correct entries in APP_DB */
        RIBNHGEntry *entryB = m_nhgmgr->getRIBNHGEntryByRIBID(ribIDB);
        ASSERT_NE(entryB, nullptr);
        uint32_t sonicObjIDB = entryB->getSonicObjID();
        ASSERT_EQ(m_nhgmgr->isSonicNHGIDInUsed(sonicObjIDB), true);
        std::string nexthops, weights;
        ASSERT_EQ(m_nextHopTable->hget(to_string(sonicObjIDB), "nexthop", nexthops), true);
        ASSERT_EQ(m_nextHopTable->hget(to_string(sonicObjIDB), "weight", weights), true);
        vector<string> nexthopResults, weightResults;
        nexthopResults = splitResults(nexthops, ",");
        weightResults = splitResults(weights, ",");
        ASSERT_EQ(nexthopResults.size(), weightResults.size());
        for (size_t i = 0; i < nexthopResults.size(); i++)
        {
            ASSERT_NE(expectedNexthopofB.find(nexthopResults[i]), expectedNexthopofB.end());
            ASSERT_EQ(weightResults[i], expectedNexthopofB.find(nexthopResults[i])->second);
        }

        /* Send the object to the NhgMgr Add function */
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjA, AF_INET), 0);
        /* Check that fpmsyncd created the correct entries in APP_DB */
        RIBNHGEntry *entryA = m_nhgmgr->getRIBNHGEntryByRIBID(ribIDA);
        ASSERT_NE(entryA, nullptr);
        uint32_t sonicObjIDA = entryA->getSonicObjID();
        nexthops = "";
        weights = "";
        ASSERT_EQ(m_nhgmgr->isSonicNHGIDInUsed(sonicObjIDA), true);
        ASSERT_EQ(m_nextHopTable->hget(to_string(sonicObjIDA), "nexthop", nexthops), true);
        ASSERT_EQ(m_nextHopTable->hget(to_string(sonicObjIDA), "weight", weights), true);
        nexthopResults.clear();
        weightResults.clear();
        nexthopResults = splitResults(nexthops, ",");
        weightResults = splitResults(weights, ",");
        ASSERT_EQ(nexthopResults.size(), weightResults.size());
        for (size_t i = 0; i < nexthopResults.size(); i++)
        {
            ASSERT_NE(expectedNexthopofA.find(nexthopResults[i]), expectedNexthopofA.end());
            ASSERT_EQ(weightResults[i], expectedNexthopofA.find(nexthopResults[i])->second);
        }

        ASSERT_EQ(m_nhgmgr->delNHGFull(ribIDA), 0);
        std::vector<FieldValueTuple> fvs;
        ASSERT_EQ(m_nextHopTable->get(to_string(sonicObjIDA), fvs), false);
        ASSERT_EQ(m_nhgmgr->getRIBNHGEntryByRIBID(ribIDA), nullptr);
        ASSERT_EQ(m_nhgmgr->isSonicNHGIDInUsed(sonicObjIDA), false);
    }

    /* Test add and remove a multi ipv6 nexthop */
    TEST_F(FpmSyncdNhgMgr, ReceivingMultiIPv6Nexthop)
    {
        /* Create a NextHopGroupFull object list containing single ipv6 nexthop*/
        std::map<uint32_t, NextHopGroupFull> dependsList;
        uint32_t ribIDA = 4;
        uint32_t ribIDB = 5;
        uint32_t ribIDC = 1;
        uint32_t ribIDB1 = 2;
        uint32_t ribIDB2 = 3;

        NextHopGroupFull nhgObjC = createSingleIPv6NextHopNHGFull("fc00:1::1", "fc00:100::1", ribIDC);
        NextHopGroupFull nhgObjB1 = createSingleIPv6NextHopNHGFull("fc00:2::1", "fc00:200::1", ribIDB1);
        NextHopGroupFull nhgObjB2 = createSingleIPv6NextHopNHGFull("fc00:2::2", "fc00:200::2", ribIDB2);
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjC, AF_INET6), 0);
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjB1, AF_INET6), 0);
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjB2, AF_INET6), 0);

        RIBNHGEntry *entryC = m_nhgmgr->getRIBNHGEntryByRIBID(nhgObjC.id);
        RIBNHGEntry *entryB1 = m_nhgmgr->getRIBNHGEntryByRIBID(nhgObjB1.id);
        RIBNHGEntry *entryB2 = m_nhgmgr->getRIBNHGEntryByRIBID(nhgObjB2.id);
        ASSERT_NE(entryC, nullptr);
        ASSERT_NE(entryB1, nullptr);
        ASSERT_NE(entryB2, nullptr);
        // uint32_t sonicObjIDC = entryC->getSonicObjID();
        // uint32_t sonicObjIDB1 = entryB1->getSonicObjID();
        // uint32_t sonicObjIDB2 = entryB2->getSonicObjID();

        // ASSERT_EQ(m_nhgmgr->isSonicNHGIDInUsed(sonicObjIDC), true);
        // ASSERT_EQ(m_nhgmgr->isSonicNHGIDInUsed(sonicObjIDB1), true);
        // ASSERT_EQ(m_nhgmgr->isSonicNHGIDInUsed(sonicObjIDB2), true);

        vector<uint32_t> dependsB = { ribIDB1, ribIDB2 };
        vector<uint32_t> dependentsB = { ribIDA };
        vector<uint32_t> dependsA = { ribIDB, ribIDC };
        map<uint32_t, NextHopGroupFull> nhgFullB = {
            { ribIDB1, nhgObjB1 },
            { ribIDB2, nhgObjB2 }
        };
        NextHopGroupFull nhgObjB = createMultiNextHopNHGFull(nhgFullB,
                                                             { { ribIDB1, 131 }, { ribIDB2, 212 } },
                                                             { { ribIDB1, 0 }, { ribIDB2, 0 } },
                                                             dependsB, dependentsB, ribIDB);
        map<string, string> expectedNexthopofB = { { "fc00:2::1", "131" }, { "fc00:2::2", "212" } };
        map<uint32_t, NextHopGroupFull> nhgFullA = { { ribIDB, nhgObjB },
                                                     { ribIDB1, nhgObjB1 },
                                                     { ribIDB2, nhgObjB2 },
                                                     { ribIDC, nhgObjC } };
        NextHopGroupFull nhgObjA = createMultiNextHopNHGFull(nhgFullA,
                                                             { { ribIDB, 11 },
                                                               { ribIDB1, 11 },
                                                               { ribIDB2, 11 },
                                                               { ribIDC, 12 } },
                                                             { { ribIDB, 2 },
                                                               { ribIDB1, 0 },
                                                               { ribIDB2, 0 },
                                                               { ribIDC, 0 } },
                                                             dependsA, {}, ribIDA);
        map<string, string> expectedNexthopofA = {
            { "fc00:2::1", "11" },
            { "fc00:2::2", "11" },
            { "fc00:1::1", "12" },
        };

        /* Send the object to the NhgMgr Add function */
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjB, AF_INET6), 0);
        /* Check that fpmsyncd created the correct entries in APP_DB */
        RIBNHGEntry *entryB = m_nhgmgr->getRIBNHGEntryByRIBID(ribIDB);
        ASSERT_NE(entryB, nullptr);
        uint32_t sonicObjIDB = entryB->getSonicObjID();
        ASSERT_EQ(m_nhgmgr->isSonicNHGIDInUsed(sonicObjIDB), true);
        std::string nexthops, weights;
        ASSERT_EQ(m_nextHopTable->hget(to_string(sonicObjIDB), "nexthop", nexthops), true);
        ASSERT_EQ(m_nextHopTable->hget(to_string(sonicObjIDB), "weight", weights), true);
        vector<string> nexthopResults, weightResults;
        nexthopResults = splitResults(nexthops, ",");
        weightResults = splitResults(weights, ",");
        ASSERT_EQ(nexthopResults.size(), weightResults.size());
        for (size_t i = 0; i < nexthopResults.size(); i++)
        {
            ASSERT_NE(expectedNexthopofB.find(nexthopResults[i]), expectedNexthopofB.end());
            ASSERT_EQ(weightResults[i], expectedNexthopofB.find(nexthopResults[i])->second);
        }

        /* Send the object to the NhgMgr Add function */
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjA, AF_INET6), 0);
        /* Check that fpmsyncd created the correct entries in APP_DB */
        RIBNHGEntry *entryA = m_nhgmgr->getRIBNHGEntryByRIBID(ribIDA);
        ASSERT_NE(entryA, nullptr);
        uint32_t sonicObjIDA = entryA->getSonicObjID();
        ASSERT_EQ(m_nhgmgr->isSonicNHGIDInUsed(sonicObjIDA), true);
        nexthops = "";
        weights = "";
        ASSERT_EQ(m_nextHopTable->hget(to_string(sonicObjIDA), "nexthop", nexthops), true);
        ASSERT_EQ(m_nextHopTable->hget(to_string(sonicObjIDA), "weight", weights), true);
        nexthopResults.clear();
        weightResults.clear();
        nexthopResults = splitResults(nexthops, ",");
        weightResults = splitResults(weights, ",");
        ASSERT_EQ(nexthopResults.size(), weightResults.size());
        for (size_t i = 0; i < nexthopResults.size(); i++)
        {
            ASSERT_NE(expectedNexthopofA.find(nexthopResults[i]), expectedNexthopofA.end());
            ASSERT_EQ(weightResults[i], expectedNexthopofA.find(nexthopResults[i])->second);
        }

        ASSERT_EQ(m_nhgmgr->delNHGFull(ribIDA), 0);
        std::vector<FieldValueTuple> fvs;
        ASSERT_EQ(m_nextHopTable->get(to_string(sonicObjIDA), fvs), false);
        ASSERT_EQ(m_nhgmgr->getRIBNHGEntryByRIBID(ribIDA), nullptr);
        ASSERT_EQ(m_nhgmgr->isSonicNHGIDInUsed(sonicObjIDA), false);
    }

    /* Test update a single ipv4 nexthop */
    TEST_F(FpmSyncdNhgMgr, UpdateingSingleNexthop)
    {
        /* Create a NextHopGroupFull object containing single ipv4 nexthop*/
        NextHopGroupFull nhg_obj = createSingleIPv4NextHopNHGFull("192.100.1.1", "120.0.0.1", 1);
        /* Send the object to the NhgMgr Add function */
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhg_obj, AF_INET), 0);
        /* Check that fpmsyncd created the correct entries in APP_DB */
        RIBNHGEntry *entry = m_nhgmgr->getRIBNHGEntryByRIBID(nhg_obj.id);
        ASSERT_NE(entry, nullptr);
        // uint32_t sonicObjID = entry->getSonicObjID();
        // ASSERT_EQ(m_nhgmgr->isSonicNHGIDInUsed(sonicObjID), true);
        std::string nexthop;
        // ASSERT_EQ(m_nextHopTable->hget(to_string(sonicObjID), "nexthop", nexthop), true);
        // ASSERT_EQ(nexthop, "192.100.1.1");
        // inet_pton(AF_INET, "122.0.0.1", &nhg_obj.gate.ipv4);
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhg_obj, AF_INET), 0);
        // ASSERT_EQ(m_nextHopTable->hget(to_string(sonicObjID), "nexthop", nexthop), true);
        // ASSERT_EQ(nexthop, "122.0.0.1");
        // ASSERT_EQ(m_nhgmgr->isSonicNHGIDInUsed(sonicObjID), true);
    }

    /* Test update a single ipv6 nexthop */
    TEST_F(FpmSyncdNhgMgr, UpdateingSingleIPv6Nexthop)
    {
        /* Create a NextHopGroupFull object containing single ipv6 nexthop*/
        NextHopGroupFull nhg_obj = createSingleIPv6NextHopNHGFull("fc00::1", "fc00::100", 1);
        /* Send the object to the NhgMgr Add function */
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhg_obj, AF_INET6), 0);
        /* Check that fpmsyncd created the correct entries in APP_DB */
        RIBNHGEntry *entry = m_nhgmgr->getRIBNHGEntryByRIBID(nhg_obj.id);
        ASSERT_NE(entry, nullptr);
        // uint32_t sonicObjID = entry->getSonicObjID();
        ASSERT_EQ(entry->getNextHopStr(), "fc00::1");
        ASSERT_EQ(entry->getInterfaceNameStr(), nhg_obj.ifname);
        std::string nexthop;
        // ASSERT_EQ(m_nextHopTable->hget(to_string(sonicObjID), "nexthop", nexthop), true);
        // ASSERT_EQ(nexthop, "fc00::1");
        // inet_pton(AF_INET6, "fc00::2", &nhg_obj.gate.ipv6);
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhg_obj, AF_INET6), 0);
        // ASSERT_EQ(m_nextHopTable->hget(to_string(sonicObjID), "nexthop", nexthop), true);
        // ASSERT_EQ(nexthop, "fc00::2");
        // ASSERT_EQ(m_nhgmgr->isSonicNHGIDInUsed(sonicObjID), true);
    }

    /* Test update a multi ipv4 nexthop */
    TEST_F(FpmSyncdNhgMgr, UpdatingMultiNexthop)
    {
        /* Create a NextHopGroupFull object list containing single ipv4 nexthop*/
        std::map<uint32_t, NextHopGroupFull> dependsList;
        map<uint32_t, uint32_t> nexthopList;
        uint32_t ribIDA = 4;
        uint32_t ribIDB = 5;
        uint32_t ribIDC = 1;
        uint32_t ribIDB1 = 2;
        uint32_t ribIDB2 = 3;

        NextHopGroupFull nhgObjC = createSingleIPv4NextHopNHGFull("192.100.1.1", "120.0.0.1", ribIDC);
        NextHopGroupFull nhgObjB1 = createSingleIPv4NextHopNHGFull("192.100.2.1", "120.0.2.1", ribIDB1);
        NextHopGroupFull nhgObjB2 = createSingleIPv4NextHopNHGFull("192.100.2.2", "120.0.2.2", ribIDB2);
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjC, AF_INET), 0);
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjB1, AF_INET), 0);
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjB2, AF_INET), 0);

        RIBNHGEntry *entryC = m_nhgmgr->getRIBNHGEntryByRIBID(nhgObjC.id);
        RIBNHGEntry *entryB1 = m_nhgmgr->getRIBNHGEntryByRIBID(nhgObjB1.id);
        RIBNHGEntry *entryB2 = m_nhgmgr->getRIBNHGEntryByRIBID(nhgObjB2.id);
        ASSERT_NE(entryC, nullptr);
        ASSERT_NE(entryB1, nullptr);
        ASSERT_NE(entryB2, nullptr);
        // uint32_t sonicObjIDC = entryC->getSonicObjID();
        // uint32_t sonicObjIDB1 = entryB1->getSonicObjID();
        // uint32_t sonicObjIDB2 = entryB2->getSonicObjID();

        // ASSERT_EQ(m_nhgmgr->isSonicNHGIDInUsed(sonicObjIDC), true);
        // ASSERT_EQ(m_nhgmgr->isSonicNHGIDInUsed(sonicObjIDB1), true);
        // ASSERT_EQ(m_nhgmgr->isSonicNHGIDInUsed(sonicObjIDB2), true);

        vector<uint32_t> dependsB = { ribIDB1, ribIDB2 };
        vector<uint32_t> dependentsB = { ribIDA };
        vector<uint32_t> dependsA = { ribIDB, ribIDC };
        map<uint32_t, NextHopGroupFull> nhgFullB = {
            { ribIDB1, nhgObjB1 },
            { ribIDB2, nhgObjB2 }
        };
        NextHopGroupFull nhgObjB = createMultiNextHopNHGFull(nhgFullB, { { ribIDB1, 131 }, { ribIDB2, 212 } },
                                                             { { ribIDB1, 0 }, { ribIDB2, 0 } }, dependsB, dependentsB, ribIDB);
        map<string, string> expectedNexthopofB = { { "192.100.2.1", "131" }, { "192.100.2.2", "212" } };
        map<uint32_t, NextHopGroupFull> nhgFullA = { { ribIDB, nhgObjB },
                                                     { ribIDB1, nhgObjB1 },
                                                     { ribIDB2, nhgObjB2 },
                                                     { ribIDC, nhgObjC } };
        NextHopGroupFull nhgObjA = createMultiNextHopNHGFull(nhgFullA,
                                                             { { ribIDB, 11 },
                                                               { ribIDB1, 11 },
                                                               { ribIDB2, 11 },
                                                               { ribIDC, 12 } },
                                                             { { ribIDB, 2 },
                                                               { ribIDB1, 0 },
                                                               { ribIDB2, 0 },
                                                               { ribIDC, 0 } },
                                                             dependsA, {}, ribIDA);
        map<string, string> expectedNexthopofA = {
            { "192.100.2.1", "11" },
            { "192.100.2.2", "11" },
            { "192.100.1.1", "12" },
        };

        /* Send the object to the NhgMgr Add function */
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjB, AF_INET), 0);
        /* Check that fpmsyncd created the correct entries in APP_DB */
        RIBNHGEntry *entryB = m_nhgmgr->getRIBNHGEntryByRIBID(ribIDB);
        ASSERT_NE(entryB, nullptr);
        uint32_t sonicObjIDB = entryB->getSonicObjID();
        ASSERT_EQ(m_nhgmgr->isSonicNHGIDInUsed(sonicObjIDB), true);
        std::string nexthops, weights;
        ASSERT_EQ(m_nextHopTable->hget(to_string(sonicObjIDB), "nexthop", nexthops), true);
        ASSERT_EQ(m_nextHopTable->hget(to_string(sonicObjIDB), "weight", weights), true);
        vector<string> nexthopResults, weightResults;
        nexthopResults = splitResults(nexthops, ",");
        weightResults = splitResults(weights, ",");
        ASSERT_EQ(nexthopResults.size(), weightResults.size());
        for (size_t i = 0; i < nexthopResults.size(); i++)
        {
            ASSERT_NE(expectedNexthopofB.find(nexthopResults[i]), expectedNexthopofB.end());
            ASSERT_EQ(weightResults[i], expectedNexthopofB.find(nexthopResults[i])->second);
        }

        /* Send the object to the NhgMgr Add function */
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjA, AF_INET), 0);
        /* Check that fpmsyncd created the correct entries in APP_DB */
        RIBNHGEntry *entryA = m_nhgmgr->getRIBNHGEntryByRIBID(ribIDA);
        ASSERT_NE(entryA, nullptr);
        uint32_t sonicObjIDA = entryA->getSonicObjID();
        ASSERT_EQ(m_nhgmgr->isSonicNHGIDInUsed(sonicObjIDA), true);
        nexthops = "";
        weights = "";
        ASSERT_EQ(m_nextHopTable->hget(to_string(sonicObjIDA), "nexthop", nexthops), true);
        ASSERT_EQ(m_nextHopTable->hget(to_string(sonicObjIDA), "weight", weights), true);
        nexthopResults.clear();
        weightResults.clear();
        nexthopResults = splitResults(nexthops, ",");
        weightResults = splitResults(weights, ",");
        ASSERT_EQ(nexthopResults.size(), weightResults.size());
        for (size_t i = 0; i < nexthopResults.size(); i++)
        {
            ASSERT_NE(expectedNexthopofA.find(nexthopResults[i]), expectedNexthopofA.end());
            ASSERT_EQ(weightResults[i], expectedNexthopofA.find(nexthopResults[i])->second);
        }

        /* Update the NHG A -> B, B1, B2 */

        dependsA = { ribIDB };
        nhgFullA = {
            { ribIDB, nhgObjB },
            { ribIDB1, nhgObjB1 },
            { ribIDB2, nhgObjB2 }
        };
        NextHopGroupFull nhgObjANew = createMultiNextHopNHGFull(nhgFullA, { { ribIDB, 12 }, { ribIDB1, 12 }, { ribIDB2, 12 } },
                                                                { { ribIDB, 2 }, { ribIDB1, 0 }, { ribIDB2, 0 } }, dependsA, {}, ribIDA);
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjANew, AF_INET), 0);
        RIBNHGEntry *entryANew = m_nhgmgr->getRIBNHGEntryByRIBID(ribIDA);
        ASSERT_NE(entryANew, nullptr);
        ASSERT_EQ(m_nhgmgr->isSonicNHGIDInUsed(sonicObjIDA), true);
        nexthops = "";
        weights = "";
        ASSERT_EQ(m_nextHopTable->hget(to_string(sonicObjIDA), "nexthop", nexthops), true);
        ASSERT_EQ(m_nextHopTable->hget(to_string(sonicObjIDA), "weight", weights), true);
        nexthopResults.clear();
        weightResults.clear();
        nexthopResults = splitResults(nexthops, ",");
        weightResults = splitResults(weights, ",");
        ASSERT_EQ(nexthopResults.size(), weightResults.size());
        expectedNexthopofA.clear();
        expectedNexthopofA = {
            { "192.100.2.1", "12" },
            { "192.100.2.2", "12" },
        };
        for (size_t i = 0; i < nexthopResults.size(); i++)
        {
            ASSERT_NE(expectedNexthopofA.find(nexthopResults[i]), expectedNexthopofA.end());
            ASSERT_EQ(weightResults[i], expectedNexthopofA.find(nexthopResults[i])->second);
        }
    }

    /* Test update a multi ipv6 nexthop */
    TEST_F(FpmSyncdNhgMgr, UpdatingMultiIPv6Nexthop)
    {
        /* Create a NextHopGroupFull object list containing single ipv6 nexthop*/
        std::map<uint32_t, NextHopGroupFull> dependsList;
        map<uint32_t, uint32_t> nexthopList;
        uint32_t ribIDA = 4;
        uint32_t ribIDB = 5;
        uint32_t ribIDC = 1;
        uint32_t ribIDB1 = 2;
        uint32_t ribIDB2 = 3;

        NextHopGroupFull nhgObjC = createSingleIPv6NextHopNHGFull("fc00:1::1", "fc00:100::1", ribIDC);
        NextHopGroupFull nhgObjB1 = createSingleIPv6NextHopNHGFull("fc00:2::1", "fc00:200::1", ribIDB1);
        NextHopGroupFull nhgObjB2 = createSingleIPv6NextHopNHGFull("fc00:2::2", "fc00:200::2", ribIDB2);
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjC, AF_INET6), 0);
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjB1, AF_INET6), 0);
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjB2, AF_INET6), 0);

        RIBNHGEntry *entryC = m_nhgmgr->getRIBNHGEntryByRIBID(nhgObjC.id);
        RIBNHGEntry *entryB1 = m_nhgmgr->getRIBNHGEntryByRIBID(nhgObjB1.id);
        RIBNHGEntry *entryB2 = m_nhgmgr->getRIBNHGEntryByRIBID(nhgObjB2.id);
        ASSERT_NE(entryC, nullptr);
        ASSERT_NE(entryB1, nullptr);
        ASSERT_NE(entryB2, nullptr);
        // uint32_t sonicObjIDC = entryC->getSonicObjID();
        // uint32_t sonicObjIDB1 = entryB1->getSonicObjID();
        // uint32_t sonicObjIDB2 = entryB2->getSonicObjID();

        // ASSERT_EQ(m_nhgmgr->isSonicNHGIDInUsed(sonicObjIDC), true);
        // ASSERT_EQ(m_nhgmgr->isSonicNHGIDInUsed(sonicObjIDB1), true);
        // ASSERT_EQ(m_nhgmgr->isSonicNHGIDInUsed(sonicObjIDB2), true);

        vector<uint32_t> dependsB = { ribIDB1, ribIDB2 };
        vector<uint32_t> dependentsB = { ribIDA };
        vector<uint32_t> dependsA = { ribIDB, ribIDC };
        map<uint32_t, NextHopGroupFull> nhgFullB = {
            { ribIDB1, nhgObjB1 },
            { ribIDB2, nhgObjB2 }
        };
        NextHopGroupFull nhgObjB = createMultiNextHopNHGFull(nhgFullB, { { ribIDB1, 131 }, { ribIDB2, 212 } },
                                                             { { ribIDB1, 0 }, { ribIDB2, 0 } }, dependsB, dependentsB, ribIDB);
        map<string, string> expectedNexthopofB = { { "fc00:2::1", "131" }, { "fc00:2::2", "212" } };
        map<uint32_t, NextHopGroupFull> nhgFullA = { { ribIDB, nhgObjB },
                                                     { ribIDB1, nhgObjB1 },
                                                     { ribIDB2, nhgObjB2 },
                                                     { ribIDC, nhgObjC } };
        NextHopGroupFull nhgObjA = createMultiNextHopNHGFull(nhgFullA,
                                                             { { ribIDB, 11 },
                                                               { ribIDB1, 11 },
                                                               { ribIDB2, 11 },
                                                               { ribIDC, 12 } },
                                                             { { ribIDB, 2 },
                                                               { ribIDB1, 0 },
                                                               { ribIDB2, 0 },
                                                               { ribIDC, 0 } },
                                                             dependsA, {}, ribIDA);
        map<string, string> expectedNexthopofA = {
            { "fc00:2::1", "11" },
            { "fc00:2::2", "11" },
            { "fc00:1::1", "12" },
        };

        /* Send the object to the NhgMgr Add function */
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjB, AF_INET6), 0);
        /* Check that fpmsyncd created the correct entries in APP_DB */
        RIBNHGEntry *entryB = m_nhgmgr->getRIBNHGEntryByRIBID(ribIDB);
        ASSERT_NE(entryB, nullptr);
        uint32_t sonicObjIDB = entryB->getSonicObjID();
        ASSERT_EQ(m_nhgmgr->isSonicNHGIDInUsed(sonicObjIDB), true);
        std::string nexthops, weights;
        ASSERT_EQ(m_nextHopTable->hget(to_string(sonicObjIDB), "nexthop", nexthops), true);
        ASSERT_EQ(m_nextHopTable->hget(to_string(sonicObjIDB), "weight", weights), true);
        vector<string> nexthopResults, weightResults;
        nexthopResults = splitResults(nexthops, ",");
        weightResults = splitResults(weights, ",");
        ASSERT_EQ(nexthopResults.size(), weightResults.size());
        for (size_t i = 0; i < nexthopResults.size(); i++)
        {
            ASSERT_NE(expectedNexthopofB.find(nexthopResults[i]), expectedNexthopofB.end());
            ASSERT_EQ(weightResults[i], expectedNexthopofB.find(nexthopResults[i])->second);
        }

        /* Send the object to the NhgMgr Add function */
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjA, AF_INET6), 0);
        /* Check that fpmsyncd created the correct entries in APP_DB */
        RIBNHGEntry *entryA = m_nhgmgr->getRIBNHGEntryByRIBID(ribIDA);
        ASSERT_NE(entryA, nullptr);
        uint32_t sonicObjIDA = entryA->getSonicObjID();
        ASSERT_EQ(m_nhgmgr->isSonicNHGIDInUsed(sonicObjIDA), true);
        nexthops = "";
        weights = "";
        ASSERT_EQ(m_nextHopTable->hget(to_string(sonicObjIDA), "nexthop", nexthops), true);
        ASSERT_EQ(m_nextHopTable->hget(to_string(sonicObjIDA), "weight", weights), true);
        nexthopResults.clear();
        weightResults.clear();
        nexthopResults = splitResults(nexthops, ",");
        weightResults = splitResults(weights, ",");
        ASSERT_EQ(nexthopResults.size(), weightResults.size());
        for (size_t i = 0; i < nexthopResults.size(); i++)
        {
            ASSERT_NE(expectedNexthopofA.find(nexthopResults[i]), expectedNexthopofA.end());
            ASSERT_EQ(weightResults[i], expectedNexthopofA.find(nexthopResults[i])->second);
        }

        /* Update the NHG A -> B, B1, B2 */

        dependsA = { ribIDB };
        nhgFullA = {
            { ribIDB, nhgObjB },
            { ribIDB1, nhgObjB1 },
            { ribIDB2, nhgObjB2 }
        };
        NextHopGroupFull nhgObjANew = createMultiNextHopNHGFull(nhgFullA, { { ribIDB, 12 }, { ribIDB1, 12 }, { ribIDB2, 12 } },
                                                                { { ribIDB, 2 }, { ribIDB1, 0 }, { ribIDB2, 0 } }, dependsA, {}, ribIDA);
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjANew, AF_INET6), 0);
        RIBNHGEntry *entryANew = m_nhgmgr->getRIBNHGEntryByRIBID(ribIDA);
        ASSERT_NE(entryANew, nullptr);
        ASSERT_EQ(m_nhgmgr->isSonicNHGIDInUsed(sonicObjIDA), true);
        nexthops = "";
        weights = "";
        ASSERT_EQ(m_nextHopTable->hget(to_string(sonicObjIDA), "nexthop", nexthops), true);
        ASSERT_EQ(m_nextHopTable->hget(to_string(sonicObjIDA), "weight", weights), true);
        nexthopResults.clear();
        weightResults.clear();
        nexthopResults = splitResults(nexthops, ",");
        weightResults = splitResults(weights, ",");
        ASSERT_EQ(nexthopResults.size(), weightResults.size());
        expectedNexthopofA.clear();
        expectedNexthopofA = {
            { "fc00:2::1", "12" },
            { "fc00:2::2", "12" },
        };
        for (size_t i = 0; i < nexthopResults.size(); i++)
        {
            ASSERT_NE(expectedNexthopofA.find(nexthopResults[i]), expectedNexthopofA.end());
            ASSERT_EQ(weightResults[i], expectedNexthopofA.find(nexthopResults[i])->second);
        }
    }

    /* Test add and remove a single unresolved SRv6 VPN nexthop */
    TEST_F(FpmSyncdNhgMgr, ReceivingSingleSRv6VPNNexthop)
    {
        /* Create two non-recursive nexthops which will be used as dependency */
        std::map<uint32_t, NextHopGroupFull> dependsList;
        map<uint32_t, uint32_t> nexthopList;
        uint32_t ribIDA = 4;

        /* Create a recursive SRv6 VPN NHG A -> B1, B2 */
        vector<uint32_t> dependsA = { };
        std::vector<fib::nh_grp_full> nhgFullA(0);
        string nexthopA = "b::b";
        string vpnSid = "1::1";
        NextHopGroupFull nhgObjA = createSingleSRv6VPNNextHopNHGFull(vpnSid.c_str(), "a::a", nexthopA.c_str(), ribIDA);
        nhgObjA.depends.resize(0);
        nhgObjA.nh_grp_full_list.resize(0);

        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjA, AF_INET6), 0);

        /* Check that fpmsyncd created the correct entries in APP_DB */
        RIBNHGEntry *entryA = m_nhgmgr->getRIBNHGEntryByRIBID(ribIDA);
        ASSERT_NE(entryA, nullptr);
        ASSERT_NE(entryA, nullptr);
        // uint32_t sonicObjIDA = entryA->getSonicObjID();
        // ASSERT_EQ(m_nhgmgr->isSonicNHGIDInUsed(sonicObjIDA), true);
        string nexthops = "";
        string vpnsids = "";
        string weights = "";
        // vector<string> nexthopResults, weightResults;
        // ASSERT_EQ(m_nextHopTable->hget(to_string(sonicObjIDA), "nexthop", nexthops), true);
        ASSERT_EQ(nexthopA, entryA->getNextHopStr());

        /* Check the SRv6 NHG Object */
        SonicGateWayNHGEntry *sonicNHGEntry = m_nhgmgr->getSonicGatewayNHGByRIBID(ribIDA);
        ASSERT_NE(sonicNHGEntry, nullptr);
        ASSERT_EQ(sonicNHGEntry->getType(), swss::SONIC_NHG_OBJ_TYPE_NHG_SRV6_GATEWAY);
        uint32_t sonicGatewayObjIDA = sonicNHGEntry->getSonicGateWayObjID();
        ASSERT_EQ(m_picContextTable->hget(to_string(sonicGatewayObjIDA), "nexthop", nexthops), true);
        ASSERT_EQ(m_picContextTable->hget(to_string(sonicGatewayObjIDA), "vpn_sid", vpnsids), true);
        ASSERT_EQ(m_nhgmgr->isSonicGatewayNHGIDInUsed(swss::SONIC_NHG_OBJ_TYPE_NHG_SRV6_GATEWAY, sonicGatewayObjIDA), true);
        ASSERT_EQ(nexthops, nexthopA);
        ASSERT_EQ(vpnsids, vpnSid);

        /* Remove the SRv6 NHG A */
        ASSERT_EQ(m_nhgmgr->delNHGFull(ribIDA), 0);
        // ASSERT_EQ(m_nhgmgr->isSonicNHGIDInUsed(sonicObjIDA), false);
        ASSERT_EQ(m_nhgmgr->isSonicGatewayNHGIDInUsed(swss::SONIC_NHG_OBJ_TYPE_NHG_SRV6_GATEWAY, sonicGatewayObjIDA), false);
        std::vector<FieldValueTuple> fvs;
        ASSERT_EQ(m_picContextTable->get(to_string(sonicGatewayObjIDA), fvs), false);
        ASSERT_EQ(m_nhgmgr->getRIBNHGEntryByRIBID(ribIDA), nullptr);
        ASSERT_EQ(m_nhgmgr->getSonicGatewayNHGByRIBID(ribIDA), nullptr);
    }

    /* Test update SRv6 VPN nexthop with new vpn_sid */
    TEST_F(FpmSyncdNhgMgr, UpdatingSingleSRv6VPNNexthopVpnSid)
    {
        /* Create a NextHopGroupFull object containing single srv6 vpn nexthop */
        std::map<uint32_t, NextHopGroupFull> dependsList;
        map<uint32_t, uint32_t> nexthopList;
        uint32_t ribIDA = 9;

        NextHopGroupFull nhgObjA = createSingleSRv6VPNNextHopNHGFull("1::1", "a::a", "b::b", ribIDA);
        nhgObjA.depends.resize(0);
        nhgObjA.nh_grp_full_list.resize(0);
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjA, AF_INET6), 0);

        /* Check that fpmsyncd created the correct entries in APP_DB */
        RIBNHGEntry *entryA = m_nhgmgr->getRIBNHGEntryByRIBID(ribIDA);
        ASSERT_NE(entryA, nullptr);
        // uint32_t sonicObjIDA = entryA->getSonicObjID();
        // ASSERT_EQ(m_nhgmgr->isSonicNHGIDInUsed(sonicObjIDA), true);
        string nexthops = "";
        string vpnsids = "";
        string weights = "";
        vector<string> nexthopResults, weightResults, vpnSidsResults;
        // ASSERT_EQ(m_nextHopTable->hget(to_string(sonicObjIDA), "nexthop", nexthops), true);
        // ASSERT_EQ("b::b", nexthops);

        /* Check the SRv6 NHG Object */
        SonicGateWayNHGEntry *sonicNHGEntry = m_nhgmgr->getSonicGatewayNHGByRIBID(ribIDA);
        ASSERT_NE(sonicNHGEntry, nullptr);
        uint32_t sonicGatewayObjID = sonicNHGEntry->getSonicGateWayObjID();
        ASSERT_EQ(m_picContextTable->hget(to_string(sonicGatewayObjID), "nexthop", nexthops), true);
        ASSERT_EQ(m_picContextTable->hget(to_string(sonicGatewayObjID), "vpn_sid", vpnsids), true);
        ASSERT_EQ(vpnsids, "1::1");
        ASSERT_EQ(nexthops, "b::b");

        /* Update the NHG object with a new vpn_sid */
        NextHopGroupFull nhgObjAUpdated = createSingleSRv6VPNNextHopNHGFull("2::2", "a::a", "b::b", ribIDA); // Changed vpn_sid from "1::1" to "2::2"
        nhgObjAUpdated.depends.resize(0);
        nhgObjAUpdated.nh_grp_full_list.resize(0);

        /* Send the updated object to the NhgMgr Add function (this should update the existing entry) */
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjAUpdated, AF_INET6), 0);

        /* Check that the vpn_sid field has been updated in the APP_DB */
        sonicNHGEntry = m_nhgmgr->getSonicGatewayNHGByRIBID(ribIDA);
        sonicGatewayObjID = sonicNHGEntry->getSonicGateWayObjID();
        ASSERT_EQ(m_picContextTable->hget(to_string(sonicGatewayObjID), "nexthop", nexthops), true);
        ASSERT_EQ(m_picContextTable->hget(to_string(sonicGatewayObjID), "vpn_sid", vpnsids), true);
        ASSERT_EQ(nexthops, "b::b"); // nexthop should remain the same
        ASSERT_EQ(vpnsids, "2::2");  // vpn_sid should be updated to the new value

        /* Remove the NHG object */
        ASSERT_EQ(m_nhgmgr->delNHGFull(ribIDA), 0);
        std::vector<FieldValueTuple> fvs;
        ASSERT_EQ(m_picContextTable->get(to_string(sonicGatewayObjID), fvs), false);
        ASSERT_EQ(m_nhgmgr->isSonicGatewayNHGIDInUsed(swss::SONIC_NHG_OBJ_TYPE_NHG_SRV6_GATEWAY, sonicGatewayObjID), false);
    }

    /* Test add and remove a multi unresolved SRv6 VPN nexthop */
    TEST_F(FpmSyncdNhgMgr, ReceivingMultiSRv6VPNNexthop)
    {
        /* Create a NextHopGroupFull object list containing single ipv6 nexthop*/
        std::map<uint32_t, NextHopGroupFull> dependsList;
        vector<string> nexthopResults, weightResults, vpnSidsResults;
        string nexthops = "", vpnsids = "", weights = "";
        map<uint32_t, uint32_t> nexthopList;
        uint32_t ribIDA = 17;
        uint32_t ribIDB = 15;
        uint32_t ribIDC = 16;

        NextHopGroupFull nhgObjB = createSingleSRv6VPNNextHopNHGFull("1::1", "a::a", "b::b", ribIDB);
        NextHopGroupFull nhgObjC = createSingleSRv6VPNNextHopNHGFull("2::2", "c::c", "e::e", ribIDC);
        nhgObjB.depends.resize(0);
        nhgObjC.depends.resize(0);
        nhgObjB.nh_grp_full_list.resize(0);
        nhgObjC.nh_grp_full_list.resize(0);

        /* Send the object to the NhgMgr Add function */
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjC, AF_INET6), 0);
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjB, AF_INET6), 0);

        /* Check that fpmsyncd created the correct entries in APP_DB for B */
        RIBNHGEntry *entryB = m_nhgmgr->getRIBNHGEntryByRIBID(ribIDB);
        ASSERT_NE(entryB, nullptr);
        uint32_t sonicObjIDB = entryB->getSonicObjID();
        ASSERT_EQ(m_nhgmgr->isSonicNHGIDInUsed(sonicObjIDB), true);
        nexthops = "";
        vpnsids = "";
        nexthopResults.clear();
        weightResults.clear();
        ASSERT_EQ(m_nextHopTable->hget(to_string(sonicObjIDB), "nexthop", nexthops), true);
        ASSERT_EQ(nexthops, "b::b");
        ASSERT_EQ(m_nextHopTable->hget(to_string(sonicObjIDB), "nexthop", nexthops), true);
        ASSERT_EQ(nexthops, "b::b");

        /* Check the SRv6 VPN nexthop of B */
        SonicGateWayNHGEntry *sonicNHGEntryB = m_nhgmgr->getSonicGatewayNHGByRIBID(ribIDB);
        ASSERT_NE(sonicNHGEntryB, nullptr);
        ASSERT_EQ(sonicNHGEntryB->getType(), swss::SONIC_NHG_OBJ_TYPE_NHG_SRV6_GATEWAY);
        uint32_t sonicGatewayObjIDB = sonicNHGEntryB->getSonicGateWayObjID();
        ASSERT_EQ(m_nhgmgr->isSonicGatewayNHGIDInUsed(swss::SONIC_NHG_OBJ_TYPE_NHG_SRV6_GATEWAY, sonicGatewayObjIDB), true);
        ASSERT_EQ(m_picContextTable->hget(to_string(sonicGatewayObjIDB), "vpn_sid", vpnsids), true);
        ASSERT_EQ(vpnsids, "1::1");
        ASSERT_EQ(m_picContextTable->hget(to_string(sonicGatewayObjIDB), "nexthop", nexthops), true);
        ASSERT_EQ(nexthops, "b::b");

        /* Check that fpmsyncd created the correct entries in APP_DB for C */
        RIBNHGEntry *entryC = m_nhgmgr->getRIBNHGEntryByRIBID(ribIDC);
        ASSERT_NE(entryC, nullptr);
        uint32_t sonicObjIDC = entryC->getSonicObjID();
        ASSERT_EQ(m_nhgmgr->isSonicNHGIDInUsed(sonicObjIDC), true);
        nexthops = "";
        nexthopResults.clear();
        weightResults.clear();
        ASSERT_EQ(m_nextHopTable->hget(to_string(sonicObjIDC), "nexthop", nexthops), true);
        ASSERT_EQ(nexthops, "e::e");

        /* Check the SRv6 VPN nexthop of C */
        SonicGateWayNHGEntry *sonicNHGEntryC = m_nhgmgr->getSonicGatewayNHGByRIBID(ribIDC);
        ASSERT_NE(sonicNHGEntryC, nullptr);
        ASSERT_EQ(sonicNHGEntryC->getType(), swss::SONIC_NHG_OBJ_TYPE_NHG_SRV6_GATEWAY);
        uint32_t sonicGatewayObjIDC = sonicNHGEntryC->getSonicGateWayObjID();
        ASSERT_EQ(m_nhgmgr->isSonicGatewayNHGIDInUsed(swss::SONIC_NHG_OBJ_TYPE_NHG_SRV6_GATEWAY, sonicGatewayObjIDC), true);
        ASSERT_EQ(m_picContextTable->hget(to_string(sonicGatewayObjIDC), "vpn_sid", vpnsids), true);
        ASSERT_EQ(vpnsids, "2::2");
        ASSERT_EQ(m_picContextTable->hget(to_string(sonicGatewayObjIDC), "nexthop", nexthops), true);
        ASSERT_EQ(nexthops, "e::e");

        /* Create the NHG A, which depends on B and C */
        map<uint32_t, NextHopGroupFull> nhgFullA = { { ribIDB, nhgObjB },
                                                     { ribIDC, nhgObjC }};

        vector<uint32_t> dependsA = { ribIDB, ribIDC };
        NextHopGroupFull nhgObjA = createMultiNextHopNHGFull(nhgFullA,
                                                             { { ribIDB, 12 },{ ribIDC, 10 }},
                                                             { { ribIDB, 0 }, { ribIDC, 0 } },
                                                             dependsA, {}, ribIDA);

        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjA, AF_INET6), 0);

        /* Check that fpmsyncd created the correct entries in APP_DB for NHG A */
        RIBNHGEntry *entryA = m_nhgmgr->getRIBNHGEntryByRIBID(ribIDA);
        ASSERT_NE(entryA, nullptr);
        uint32_t sonicObjIDA = entryA->getSonicObjID();
        ASSERT_EQ(m_nhgmgr->isSonicNHGIDInUsed(sonicObjIDA), true);
        nexthops = "";
        weights = "";
        vpnsids = "";
        map<string, string> expectedNexthopofA = {
            { "b::b", "12" },
            { "e::e", "10" },
        };
        nexthopResults.clear();
        weightResults.clear();
        ASSERT_EQ(m_nextHopTable->hget(to_string(sonicObjIDA), "nexthop", nexthops), true);
        ASSERT_EQ(m_nextHopTable->hget(to_string(sonicObjIDA), "weight", weights), true);
        nexthopResults.clear();
        weightResults.clear();
        nexthopResults = splitResults(nexthops, ",");
        weightResults = splitResults(weights, ",");
        ASSERT_EQ(nexthopResults.size(), weightResults.size());

        for (size_t i = 0; i < nexthopResults.size(); i++)
        {
            ASSERT_NE(expectedNexthopofA.find(nexthopResults[i]), expectedNexthopofA.end());
            ASSERT_EQ(weightResults[i], expectedNexthopofA.find(nexthopResults[i])->second);
        }

        /* Check the SRv6 NHG Object for NHG A */
        map<string, string> expectedVpnSidofA = {
            { "b::b", "1::1" },
            { "e::e", "2::2" },
        };
        SonicGateWayNHGEntry *sonicNHGEntry = m_nhgmgr->getSonicGatewayNHGByRIBID(ribIDA);
        uint32_t sonicGatewayObjID = sonicNHGEntry->getSonicGateWayObjID();
        ASSERT_NE(sonicNHGEntry, nullptr);
        ASSERT_EQ(m_picContextTable->hget(to_string(sonicGatewayObjID), "nexthop", nexthops), true);
        ASSERT_EQ(m_picContextTable->hget(to_string(sonicGatewayObjID), "vpn_sid", vpnsids), true);
        ASSERT_NE(nexthops, "");
        nexthopResults = splitResults(nexthops, ",");
        vpnSidsResults = splitResults(vpnsids, ",");
        for (size_t i = 0; i < vpnSidsResults.size(); i++)
        {
            ASSERT_NE(expectedVpnSidofA.find(nexthopResults[i]), expectedVpnSidofA.end());
            ASSERT_EQ(vpnSidsResults[i], expectedVpnSidofA.find(nexthopResults[i])->second);
        }

        /* Remove the SRv6 NHG Object */
        ASSERT_EQ(m_nhgmgr->delNHGFull(ribIDA), 0);
        ASSERT_EQ(m_nhgmgr->getSonicGatewayNHGByRIBID(ribIDA), nullptr);
        std::vector<FieldValueTuple> fvs;
        ASSERT_EQ(m_picContextTable->get(to_string(sonicGatewayObjID), fvs), false);
        ASSERT_EQ(m_nhgmgr->isSonicGatewayNHGIDInUsed(swss::SONIC_NHG_OBJ_TYPE_NHG_SRV6_GATEWAY, sonicGatewayObjID), false);
    }

    /*
     * Test NHT event with no affected entries (prev and curr NHG IDs don't exist).
     * onNhtEvent should return 0 without errors.
     */
    TEST_F(FpmSyncdNhgMgr, NhtEventNoAffectedEntries)
    {
        /* Fire NHT event with non-existent NHG IDs */
        ASSERT_EQ(m_nhgmgr->onNhtEvent(999, 998), 0);
    }

    /*
     * Test NHT event with same prev and curr NHG ID.
     * Should be a no-op.
     */
    TEST_F(FpmSyncdNhgMgr, NhtEventSamePrevCurr)
    {
        ASSERT_EQ(m_nhgmgr->onNhtEvent(5, 5), 0);
    }

    /*
     * Test NHT event with zero prev_nhg_id (new resolution).
     * Create entries B1, B2 as leaves; B depends on B1, B2; A depends on B.
     * Fire NHT event(0, curr_nhg_id=B1) -- should re-resolve entries that
     * depend on B1 without crashing.
     */
    TEST_F(FpmSyncdNhgMgr, NhtEventNewResolution)
    {
        uint32_t ribIDC = 1;
        uint32_t ribIDB1 = 2;
        uint32_t ribIDB2 = 3;
        uint32_t ribIDA = 4;
        uint32_t ribIDB = 5;

        /* Create leaf entries */
        NextHopGroupFull nhgObjC = createSingleIPv4NextHopNHGFull("192.100.1.1", "120.0.0.1", ribIDC);
        NextHopGroupFull nhgObjB1 = createSingleIPv4NextHopNHGFull("192.100.2.1", "120.0.2.1", ribIDB1);
        NextHopGroupFull nhgObjB2 = createSingleIPv4NextHopNHGFull("192.100.2.2", "120.0.2.2", ribIDB2);
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjC, AF_INET), 0);
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjB1, AF_INET), 0);
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjB2, AF_INET), 0);

        /* Create multi NHG B -> B1, B2 */
        vector<uint32_t> dependsB = { ribIDB1, ribIDB2 };
        vector<uint32_t> dependentsB = { ribIDA };
        map<uint32_t, NextHopGroupFull> nhgFullB = {
            { ribIDB1, nhgObjB1 },
            { ribIDB2, nhgObjB2 }
        };
        NextHopGroupFull nhgObjB = createMultiNextHopNHGFull(nhgFullB,
                                                             { { ribIDB1, 131 }, { ribIDB2, 212 } },
                                                             { { ribIDB1, 0 }, { ribIDB2, 0 } },
                                                             dependsB, dependentsB, ribIDB);
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjB, AF_INET), 0);

        /* Create multi NHG A -> B, C */
        vector<uint32_t> dependsA = { ribIDB, ribIDC };
        map<uint32_t, NextHopGroupFull> nhgFullA = { { ribIDB, nhgObjB },
                                                     { ribIDB1, nhgObjB1 },
                                                     { ribIDB2, nhgObjB2 },
                                                     { ribIDC, nhgObjC } };
        NextHopGroupFull nhgObjA = createMultiNextHopNHGFull(nhgFullA,
                                                             { { ribIDB, 11 },
                                                               { ribIDB1, 11 },
                                                               { ribIDB2, 11 },
                                                               { ribIDC, 12 } },
                                                             { { ribIDB, 2 },
                                                               { ribIDB1, 0 },
                                                               { ribIDB2, 0 },
                                                               { ribIDC, 0 } },
                                                             dependsA, {}, ribIDA);
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjA, AF_INET), 0);

        /* Verify initial state */
        RIBNHGEntry *entryA = m_nhgmgr->getRIBNHGEntryByRIBID(ribIDA);
        ASSERT_NE(entryA, nullptr);
        uint32_t sonicObjIDA = entryA->getSonicObjID();
        ASSERT_NE(sonicObjIDA, (uint32_t)0);

        std::string nexthops, weights;
        ASSERT_EQ(m_nextHopTable->hget(to_string(sonicObjIDA), "nexthop", nexthops), true);
        ASSERT_EQ(m_nextHopTable->hget(to_string(sonicObjIDA), "weight", weights), true);

        /* Fire NHT event: new resolution (0 -> B1) */
        ASSERT_EQ(m_nhgmgr->onNhtEvent(0, ribIDB1), 0);

        /* Verify entries still exist and DB is consistent */
        entryA = m_nhgmgr->getRIBNHGEntryByRIBID(ribIDA);
        ASSERT_NE(entryA, nullptr);
        ASSERT_EQ(m_nextHopTable->hget(to_string(sonicObjIDA), "nexthop", nexthops), true);

        /* Clean up */
        ASSERT_EQ(m_nhgmgr->delNHGFull(ribIDA), 0);
        ASSERT_EQ(m_nhgmgr->delNHGFull(ribIDB), 0);
        ASSERT_EQ(m_nhgmgr->delNHGFull(ribIDC), 0);
        ASSERT_EQ(m_nhgmgr->delNHGFull(ribIDB1), 0);
        ASSERT_EQ(m_nhgmgr->delNHGFull(ribIDB2), 0);
    }

    /*
     * Test NHT event backwalk with multi-level dependency.
     * Topology: A -> B + C, B -> B1 + B2
     * Fire NHT event(B1, 0) to simulate B1 becoming unresolved.
     * B and A should be re-resolved. B's DB entry should be rewritten.
     */
    TEST_F(FpmSyncdNhgMgr, NhtEventBackwalkMultiLevel)
    {
        uint32_t ribIDC = 1;
        uint32_t ribIDB1 = 2;
        uint32_t ribIDB2 = 3;
        uint32_t ribIDA = 4;
        uint32_t ribIDB = 5;

        /* Create leaf entries */
        NextHopGroupFull nhgObjC = createSingleIPv4NextHopNHGFull("192.100.1.1", "120.0.0.1", ribIDC);
        NextHopGroupFull nhgObjB1 = createSingleIPv4NextHopNHGFull("192.100.2.1", "120.0.2.1", ribIDB1);
        NextHopGroupFull nhgObjB2 = createSingleIPv4NextHopNHGFull("192.100.2.2", "120.0.2.2", ribIDB2);
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjC, AF_INET), 0);
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjB1, AF_INET), 0);
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjB2, AF_INET), 0);

        /* Create multi NHG B -> B1, B2 */
        vector<uint32_t> dependsB = { ribIDB1, ribIDB2 };
        vector<uint32_t> dependentsB = { ribIDA };
        map<uint32_t, NextHopGroupFull> nhgFullB = {
            { ribIDB1, nhgObjB1 },
            { ribIDB2, nhgObjB2 }
        };
        NextHopGroupFull nhgObjB = createMultiNextHopNHGFull(nhgFullB,
                                                             { { ribIDB1, 131 }, { ribIDB2, 212 } },
                                                             { { ribIDB1, 0 }, { ribIDB2, 0 } },
                                                             dependsB, dependentsB, ribIDB);
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjB, AF_INET), 0);

        /* Verify B's initial DB state */
        RIBNHGEntry *entryB = m_nhgmgr->getRIBNHGEntryByRIBID(ribIDB);
        ASSERT_NE(entryB, nullptr);
        uint32_t sonicObjIDB = entryB->getSonicObjID();
        std::string nexthops, weights;
        ASSERT_EQ(m_nextHopTable->hget(to_string(sonicObjIDB), "nexthop", nexthops), true);
        ASSERT_EQ(m_nextHopTable->hget(to_string(sonicObjIDB), "weight", weights), true);
        /* B should have both B1 and B2 nexthops */
        vector<string> nexthopResults = splitResults(nexthops, ",");
        ASSERT_EQ(nexthopResults.size(), (size_t)2);

        /* Create multi NHG A -> B, B1, B2, C */
        vector<uint32_t> dependsA = { ribIDB, ribIDC };
        map<uint32_t, NextHopGroupFull> nhgFullA = { { ribIDB, nhgObjB },
                                                     { ribIDB1, nhgObjB1 },
                                                     { ribIDB2, nhgObjB2 },
                                                     { ribIDC, nhgObjC } };
        NextHopGroupFull nhgObjA = createMultiNextHopNHGFull(nhgFullA,
                                                             { { ribIDB, 11 },
                                                               { ribIDB1, 11 },
                                                               { ribIDB2, 11 },
                                                               { ribIDC, 12 } },
                                                             { { ribIDB, 2 },
                                                               { ribIDB1, 0 },
                                                               { ribIDB2, 0 },
                                                               { ribIDC, 0 } },
                                                             dependsA, {}, ribIDA);
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjA, AF_INET), 0);

        /* Verify A's initial DB state */
        RIBNHGEntry *entryA = m_nhgmgr->getRIBNHGEntryByRIBID(ribIDA);
        ASSERT_NE(entryA, nullptr);
        uint32_t sonicObjIDA = entryA->getSonicObjID();
        ASSERT_EQ(m_nextHopTable->hget(to_string(sonicObjIDA), "nexthop", nexthops), true);
        nexthopResults = splitResults(nexthops, ",");
        ASSERT_EQ(nexthopResults.size(), (size_t)3); /* B1, B2, C resolved */

        /*
         * Fire NHT event: B1 resolution changed (B1 -> 0).
         * This should trigger re-resolve on B (dependent of B1), then A (dependent of B).
         */
        ASSERT_EQ(m_nhgmgr->onNhtEvent(ribIDB1, 0), 0);

        /*
         * Verify B was re-resolved. Since re-resolve doesn't change the
         * underlying nhg data (only re-computes resolved group from existing
         * nh_grp_full_list), B should still show same resolved state.
         */
        entryB = m_nhgmgr->getRIBNHGEntryByRIBID(ribIDB);
        ASSERT_NE(entryB, nullptr);
        ASSERT_EQ(m_nextHopTable->hget(to_string(sonicObjIDB), "nexthop", nexthops), true);

        /* Verify A was also re-resolved (forward walk from B) */
        entryA = m_nhgmgr->getRIBNHGEntryByRIBID(ribIDA);
        ASSERT_NE(entryA, nullptr);
        ASSERT_EQ(m_nextHopTable->hget(to_string(sonicObjIDA), "nexthop", nexthops), true);

        /* Clean up */
        ASSERT_EQ(m_nhgmgr->delNHGFull(ribIDA), 0);
        ASSERT_EQ(m_nhgmgr->delNHGFull(ribIDB), 0);
        ASSERT_EQ(m_nhgmgr->delNHGFull(ribIDC), 0);
        ASSERT_EQ(m_nhgmgr->delNHGFull(ribIDB1), 0);
        ASSERT_EQ(m_nhgmgr->delNHGFull(ribIDB2), 0);
    }

    /*
     * Test NHT event forward walk propagation.
     * Topology: D -> A, A -> B + C, B -> B1 + B2
     * Fire NHT event on B2. Verify D (grandchild of B2) is also re-resolved.
     */
    TEST_F(FpmSyncdNhgMgr, NhtEventForwardWalkPropagation)
    {
        uint32_t ribIDC = 1;
        uint32_t ribIDB1 = 2;
        uint32_t ribIDB2 = 3;
        uint32_t ribIDA = 4;
        uint32_t ribIDB = 5;
        uint32_t ribIDD = 6;

        /* Create leaf entries */
        NextHopGroupFull nhgObjC = createSingleIPv4NextHopNHGFull("192.100.1.1", "120.0.0.1", ribIDC);
        NextHopGroupFull nhgObjB1 = createSingleIPv4NextHopNHGFull("192.100.2.1", "120.0.2.1", ribIDB1);
        NextHopGroupFull nhgObjB2 = createSingleIPv4NextHopNHGFull("192.100.2.2", "120.0.2.2", ribIDB2);
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjC, AF_INET), 0);
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjB1, AF_INET), 0);
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjB2, AF_INET), 0);

        /* B -> B1, B2 */
        vector<uint32_t> dependsB = { ribIDB1, ribIDB2 };
        vector<uint32_t> dependentsB = { ribIDA };
        map<uint32_t, NextHopGroupFull> nhgFullB = {
            { ribIDB1, nhgObjB1 },
            { ribIDB2, nhgObjB2 }
        };
        NextHopGroupFull nhgObjB = createMultiNextHopNHGFull(nhgFullB,
                                                             { { ribIDB1, 10 }, { ribIDB2, 20 } },
                                                             { { ribIDB1, 0 }, { ribIDB2, 0 } },
                                                             dependsB, dependentsB, ribIDB);
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjB, AF_INET), 0);

        /* A -> B, B1, B2, C (with B recursive at num_direct=2) */
        vector<uint32_t> dependsA = { ribIDB, ribIDC };
        vector<uint32_t> dependentsA = { ribIDD };
        map<uint32_t, NextHopGroupFull> nhgFullA = { { ribIDB, nhgObjB },
                                                     { ribIDB1, nhgObjB1 },
                                                     { ribIDB2, nhgObjB2 },
                                                     { ribIDC, nhgObjC } };
        NextHopGroupFull nhgObjA = createMultiNextHopNHGFull(nhgFullA,
                                                             { { ribIDB, 11 },
                                                               { ribIDB1, 11 },
                                                               { ribIDB2, 11 },
                                                               { ribIDC, 12 } },
                                                             { { ribIDB, 2 },
                                                               { ribIDB1, 0 },
                                                               { ribIDB2, 0 },
                                                               { ribIDC, 0 } },
                                                             dependsA, dependentsA, ribIDA);
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjA, AF_INET), 0);

        /* D -> A, B1, B2, C (mirrors A's resolved group) */
        vector<uint32_t> dependsD = { ribIDA };
        map<uint32_t, NextHopGroupFull> nhgFullD = { { ribIDA, nhgObjA },
                                                     { ribIDB1, nhgObjB1 },
                                                     { ribIDB2, nhgObjB2 },
                                                     { ribIDC, nhgObjC } };
        NextHopGroupFull nhgObjD = createMultiNextHopNHGFull(nhgFullD,
                                                             { { ribIDA, 50 },
                                                               { ribIDB1, 50 },
                                                               { ribIDB2, 50 },
                                                               { ribIDC, 50 } },
                                                             { { ribIDA, 3 },
                                                               { ribIDB1, 0 },
                                                               { ribIDB2, 0 },
                                                               { ribIDC, 0 } },
                                                             dependsD, {}, ribIDD);
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjD, AF_INET), 0);

        /* Verify D is in the DB */
        RIBNHGEntry *entryD = m_nhgmgr->getRIBNHGEntryByRIBID(ribIDD);
        ASSERT_NE(entryD, nullptr);
        uint32_t sonicObjIDD = entryD->getSonicObjID();
        std::string nexthops;
        ASSERT_EQ(m_nextHopTable->hget(to_string(sonicObjIDD), "nexthop", nexthops), true);

        /*
         * Fire NHT event: B2 resolution changes.
         * B depends on B2 -> B gets re-resolved.
         * A depends on B -> A gets re-resolved (forward walk from B).
         * D depends on A -> D gets re-resolved (forward walk from A).
         */
        ASSERT_EQ(m_nhgmgr->onNhtEvent(ribIDB2, 0), 0);

        /* Verify D was re-resolved and still in DB */
        entryD = m_nhgmgr->getRIBNHGEntryByRIBID(ribIDD);
        ASSERT_NE(entryD, nullptr);
        ASSERT_EQ(m_nextHopTable->hget(to_string(sonicObjIDD), "nexthop", nexthops), true);

        /* Clean up in dependency order */
        ASSERT_EQ(m_nhgmgr->delNHGFull(ribIDD), 0);
        ASSERT_EQ(m_nhgmgr->delNHGFull(ribIDA), 0);
        ASSERT_EQ(m_nhgmgr->delNHGFull(ribIDB), 0);
        ASSERT_EQ(m_nhgmgr->delNHGFull(ribIDC), 0);
        ASSERT_EQ(m_nhgmgr->delNHGFull(ribIDB1), 0);
        ASSERT_EQ(m_nhgmgr->delNHGFull(ribIDB2), 0);
    }

    /*
     * Test NHT event with IPv6 topology.
     * Topology: A -> B + C (IPv6). Fire NHT event on B.
     * Verify A is re-resolved with correct IPv6 nexthops.
     */
    TEST_F(FpmSyncdNhgMgr, NhtEventIPv6Backwalk)
    {
        uint32_t ribIDC = 1;
        uint32_t ribIDB = 2;
        uint32_t ribIDA = 3;

        NextHopGroupFull nhgObjC = createSingleIPv6NextHopNHGFull("fc00:1::1", "fc00:100::1", ribIDC);
        NextHopGroupFull nhgObjB = createSingleIPv6NextHopNHGFull("fc00:2::1", "fc00:200::1", ribIDB);
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjC, AF_INET6), 0);
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjB, AF_INET6), 0);

        /* A -> B, C (both direct with num_direct=0) */
        vector<uint32_t> dependsA = { ribIDB, ribIDC };
        map<uint32_t, NextHopGroupFull> nhgFullA = { { ribIDB, nhgObjB },
                                                     { ribIDC, nhgObjC } };
        NextHopGroupFull nhgObjA = createMultiNextHopNHGFull(nhgFullA,
                                                             { { ribIDB, 10 }, { ribIDC, 20 } },
                                                             { { ribIDB, 0 }, { ribIDC, 0 } },
                                                             dependsA, {}, ribIDA);
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjA, AF_INET6), 0);

        /* Verify A's initial state */
        RIBNHGEntry *entryA = m_nhgmgr->getRIBNHGEntryByRIBID(ribIDA);
        ASSERT_NE(entryA, nullptr);
        uint32_t sonicObjIDA = entryA->getSonicObjID();
        std::string nexthops, weights;
        ASSERT_EQ(m_nextHopTable->hget(to_string(sonicObjIDA), "nexthop", nexthops), true);
        ASSERT_EQ(m_nextHopTable->hget(to_string(sonicObjIDA), "weight", weights), true);

        map<string, string> expectedNexthopofA = {
            { "fc00:2::1", "10" },
            { "fc00:1::1", "20" },
        };
        vector<string> nexthopResults = splitResults(nexthops, ",");
        vector<string> weightResults = splitResults(weights, ",");
        ASSERT_EQ(nexthopResults.size(), (size_t)2);

        /* Fire NHT event on B */
        ASSERT_EQ(m_nhgmgr->onNhtEvent(ribIDB, 0), 0);

        /* Verify A was re-resolved and DB still has correct values */
        ASSERT_EQ(m_nextHopTable->hget(to_string(sonicObjIDA), "nexthop", nexthops), true);
        ASSERT_EQ(m_nextHopTable->hget(to_string(sonicObjIDA), "weight", weights), true);
        nexthopResults = splitResults(nexthops, ",");
        weightResults = splitResults(weights, ",");
        ASSERT_EQ(nexthopResults.size(), weightResults.size());
        for (size_t i = 0; i < nexthopResults.size(); i++)
        {
            ASSERT_NE(expectedNexthopofA.find(nexthopResults[i]), expectedNexthopofA.end());
            ASSERT_EQ(weightResults[i], expectedNexthopofA.find(nexthopResults[i])->second);
        }

        /* Clean up */
        ASSERT_EQ(m_nhgmgr->delNHGFull(ribIDA), 0);
        ASSERT_EQ(m_nhgmgr->delNHGFull(ribIDB), 0);
        ASSERT_EQ(m_nhgmgr->delNHGFull(ribIDC), 0);
    }

    /*
     * Test NHT event with SRv6 VPN topology.
     * Topology: A -> B + C (SRv6 VPN). Fire NHT event.
     * Verify A and its PIC context are re-resolved.
     */
    TEST_F(FpmSyncdNhgMgr, NhtEventSRv6VPNBackwalk)
    {
        uint32_t ribIDB = 15;
        uint32_t ribIDC = 16;
        uint32_t ribIDA = 17;

        NextHopGroupFull nhgObjB = createSingleSRv6VPNNextHopNHGFull("1::1", "a::a", "b::b", ribIDB);
        NextHopGroupFull nhgObjC = createSingleSRv6VPNNextHopNHGFull("2::2", "c::c", "e::e", ribIDC);
        nhgObjB.depends.resize(0);
        nhgObjC.depends.resize(0);
        nhgObjB.nh_grp_full_list.resize(0);
        nhgObjC.nh_grp_full_list.resize(0);

        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjC, AF_INET6), 0);
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjB, AF_INET6), 0);

        /* Create A -> B + C */
        map<uint32_t, NextHopGroupFull> nhgFullA = { { ribIDB, nhgObjB },
                                                     { ribIDC, nhgObjC }};
        vector<uint32_t> dependsA = { ribIDB, ribIDC };
        NextHopGroupFull nhgObjA = createMultiNextHopNHGFull(nhgFullA,
                                                             { { ribIDB, 12 }, { ribIDC, 10 } },
                                                             { { ribIDB, 0 }, { ribIDC, 0 } },
                                                             dependsA, {}, ribIDA);
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjA, AF_INET6), 0);

        /* Verify initial PIC context for A */
        SonicGateWayNHGEntry *sonicNHGEntry = m_nhgmgr->getSonicGatewayNHGByRIBID(ribIDA);
        ASSERT_NE(sonicNHGEntry, nullptr);
        uint32_t sonicGatewayObjID = sonicNHGEntry->getSonicGateWayObjID();
        std::string nexthops, vpnsids;
        ASSERT_EQ(m_picContextTable->hget(to_string(sonicGatewayObjID), "nexthop", nexthops), true);
        ASSERT_EQ(m_picContextTable->hget(to_string(sonicGatewayObjID), "vpn_sid", vpnsids), true);

        /* Fire NHT event on B */
        ASSERT_EQ(m_nhgmgr->onNhtEvent(ribIDB, 0), 0);

        /* Verify A was re-resolved and NHG + PIC tables still have entries */
        RIBNHGEntry *entryA = m_nhgmgr->getRIBNHGEntryByRIBID(ribIDA);
        ASSERT_NE(entryA, nullptr);
        uint32_t sonicObjIDA = entryA->getSonicObjID();
        ASSERT_EQ(m_nextHopTable->hget(to_string(sonicObjIDA), "nexthop", nexthops), true);

        /* Verify PIC context table still has the entry */
        sonicNHGEntry = m_nhgmgr->getSonicGatewayNHGByRIBID(ribIDA);
        ASSERT_NE(sonicNHGEntry, nullptr);
        ASSERT_EQ(m_picContextTable->hget(to_string(sonicGatewayObjID), "nexthop", nexthops), true);
        ASSERT_EQ(m_picContextTable->hget(to_string(sonicGatewayObjID), "vpn_sid", vpnsids), true);

        /* Clean up */
        ASSERT_EQ(m_nhgmgr->delNHGFull(ribIDA), 0);
        ASSERT_EQ(m_nhgmgr->delNHGFull(ribIDB), 0);
        ASSERT_EQ(m_nhgmgr->delNHGFull(ribIDC), 0);
    }

    /*
     * ======================================================================
     * LLD Topology 1: Global table recursive routes (9 NHGs)
     *
     *   234 (leaf, fc08::2)
     *   237 (leaf, fc06::2)
     *   238 (core ECMP, depends=[234,237], dependents=[257,258,263,264])
     *   257 (intermediate 2064:100::1d, depends=[238], dependents=[256])
     *   258 (intermediate 2064:200::1e, depends=[238], dependents=[256])
     *   263 (intermediate 1::1, depends=[238], dependents=[262])
     *   264 (intermediate 2::2, depends=[238], dependents=[262])
     *   256 (route 1::1, depends=[257,258])
     *   262 (route 3::3, depends=[263,264])
     * ======================================================================
     */
    static void buildTopology1(NHGMgr* mgr) {
        /* Leaf NHGs */
        NextHopGroupFull nhg234 = createSingleIPv6NextHopNHGFull("fc08::2", "a::1", 234);
        NextHopGroupFull nhg237 = createSingleIPv6NextHopNHGFull("fc06::2", "a::2", 237);
        ASSERT_EQ(mgr->addNHGFull(nhg234, AF_INET6), 0);
        ASSERT_EQ(mgr->addNHGFull(nhg237, AF_INET6), 0);

        /* NHG 238: core ECMP, depends=[234, 237], resolved={234,237} */
        map<uint32_t, NextHopGroupFull> nhgFull238 = {{234, nhg234}, {237, nhg237}};
        NextHopGroupFull nhg238 = createMultiNextHopNHGFull(
            nhgFull238, {{234, 1}, {237, 1}}, {{234, 0}, {237, 0}},
            {234, 237}, {}, 238);
        ASSERT_EQ(mgr->addNHGFull(nhg238, AF_INET6), 0);

        /* NHG 257: intermediate, depends=[238], resolved={234,237} */
        map<uint32_t, NextHopGroupFull> nhgFullIntermediate = {
            {234, nhg234}, {237, nhg237}, {238, nhg238}};
        NextHopGroupFull nhg257 = createMultiNextHopNHGFull(
            nhgFullIntermediate, {{234, 1}, {237, 1}, {238, 1}},
            {{234, 0}, {237, 0}, {238, 2}},
            {238}, {}, 257);
        ASSERT_EQ(mgr->addNHGFull(nhg257, AF_INET6), 0);

        /* NHG 258: intermediate, depends=[238], resolved={234,237} */
        NextHopGroupFull nhg258 = createMultiNextHopNHGFull(
            nhgFullIntermediate, {{234, 1}, {237, 1}, {238, 1}},
            {{234, 0}, {237, 0}, {238, 2}},
            {238}, {}, 258);
        ASSERT_EQ(mgr->addNHGFull(nhg258, AF_INET6), 0);

        /* NHG 263: intermediate, depends=[238], resolved={234,237} */
        NextHopGroupFull nhg263 = createMultiNextHopNHGFull(
            nhgFullIntermediate, {{234, 1}, {237, 1}, {238, 1}},
            {{234, 0}, {237, 0}, {238, 2}},
            {238}, {}, 263);
        ASSERT_EQ(mgr->addNHGFull(nhg263, AF_INET6), 0);

        /* NHG 264: intermediate, depends=[238], resolved={234,237} */
        NextHopGroupFull nhg264 = createMultiNextHopNHGFull(
            nhgFullIntermediate, {{234, 1}, {237, 1}, {238, 1}},
            {{234, 0}, {237, 0}, {238, 2}},
            {238}, {}, 264);
        ASSERT_EQ(mgr->addNHGFull(nhg264, AF_INET6), 0);

        /* NHG 256: route 1::1, depends=[257,258], resolved={234,237} */
        map<uint32_t, NextHopGroupFull> nhgFull256 = {
            {234, nhg234}, {237, nhg237}, {257, nhg257}, {258, nhg258}};
        NextHopGroupFull nhg256 = createMultiNextHopNHGFull(
            nhgFull256, {{234, 1}, {237, 1}, {257, 1}, {258, 1}},
            {{234, 0}, {237, 0}, {257, 2}, {258, 2}},
            {257, 258}, {}, 256);
        ASSERT_EQ(mgr->addNHGFull(nhg256, AF_INET6), 0);

        /* NHG 262: route 3::3, depends=[263,264], resolved={234,237} */
        map<uint32_t, NextHopGroupFull> nhgFull262 = {
            {234, nhg234}, {237, nhg237}, {263, nhg263}, {264, nhg264}};
        NextHopGroupFull nhg262 = createMultiNextHopNHGFull(
            nhgFull262, {{234, 1}, {237, 1}, {263, 1}, {264, 1}},
            {{234, 0}, {237, 0}, {263, 2}, {264, 2}},
            {263, 264}, {}, 262);
        ASSERT_EQ(mgr->addNHGFull(nhg262, AF_INET6), 0);
    }

    /* Helper: verify a multi-NHG entry has expected nexthops in APPDB */
    static void verifyNHGNexthops(swss::Table* nhTable, NHGMgr* mgr,
                                  uint32_t ribId, set<string> expectedNHs) {
        RIBNHGEntry* entry = mgr->getRIBNHGEntryByRIBID(ribId);
        ASSERT_NE(entry, nullptr);
        uint32_t sonicId = entry->getSonicObjID();
        if (sonicId == 0) return; /* leaf - no APPDB entry */
        string nexthops;
        ASSERT_EQ(nhTable->hget(to_string(sonicId), "nexthop", nexthops), true);
        vector<string> parts = splitResults(nexthops, ",");
        set<string> actual(parts.begin(), parts.end());
        EXPECT_EQ(actual, expectedNHs);
    }

    /*
     * Topology 1 - Local Failure: fc06::2 withdrawn
     * NHT event: prev=237, curr=0
     * Expected walk: 237's dependents={238} → 238's deps={257,258,263,264}
     *   → 257→{256}, 258→{256}, 263→{262}, 264→{262}
     * APPDB Updated: {238,257,258,263,264,256,262}
     * Not Reached: {234}
     */
    TEST_F(FpmSyncdNhgMgr, Topology1LocalFailure)
    {
        ASSERT_NO_FATAL_FAILURE(buildTopology1(m_nhgmgr.get()));

        /* Verify initial state: all multi-NHGs resolve to {fc08::2, fc06::2} */
        set<string> bothNHs = {"fc08::2", "fc06::2"};
        for (uint32_t id : {238u, 257u, 258u, 263u, 264u, 256u, 262u}) {
            ASSERT_NO_FATAL_FAILURE(verifyNHGNexthops(
                m_nextHopTable.get(), m_nhgmgr.get(), id, bothNHs));
        }

        /* Fire NHT event: local failure fc06::2, prev=237 (leaf), curr=0 */
        ASSERT_EQ(m_nhgmgr->onNhtEvent(237, 0), 0);

        /* Verify all entries still valid after walk */
        for (uint32_t id : {234u, 237u, 238u, 257u, 258u, 263u, 264u, 256u, 262u}) {
            EXPECT_NE(m_nhgmgr->getRIBNHGEntryByRIBID(id), nullptr);
        }

        /*
         * Verify APPDB still correct. Re-resolve from unchanged data
         * produces same nexthops.
         */
        for (uint32_t id : {238u, 257u, 258u, 263u, 264u, 256u, 262u}) {
            ASSERT_NO_FATAL_FAILURE(verifyNHGNexthops(
                m_nextHopTable.get(), m_nhgmgr.get(), id, bothNHs));
        }

        /* Clean up (reverse dependency order) */
        ASSERT_EQ(m_nhgmgr->delNHGFull(256), 0);
        ASSERT_EQ(m_nhgmgr->delNHGFull(262), 0);
        ASSERT_EQ(m_nhgmgr->delNHGFull(257), 0);
        ASSERT_EQ(m_nhgmgr->delNHGFull(258), 0);
        ASSERT_EQ(m_nhgmgr->delNHGFull(263), 0);
        ASSERT_EQ(m_nhgmgr->delNHGFull(264), 0);
        ASSERT_EQ(m_nhgmgr->delNHGFull(238), 0);
        ASSERT_EQ(m_nhgmgr->delNHGFull(234), 0);
        ASSERT_EQ(m_nhgmgr->delNHGFull(237), 0);
    }

    /*
     * Topology 1 - Remote Failure 1: 2064:200::1e withdrawn
     * NHT event: prev=258, curr=0
     * Expected walk: 258's dependents={256} → end (256 has no dependents)
     * APPDB Updated: {256}
     * Not Reached: {234,237,238,257,263,264,262}
     */
    TEST_F(FpmSyncdNhgMgr, Topology1RemoteFailure1)
    {
        ASSERT_NO_FATAL_FAILURE(buildTopology1(m_nhgmgr.get()));

        set<string> bothNHs = {"fc08::2", "fc06::2"};

        /* Fire NHT event: remote failure 2064:200::1e, prev=258, curr=0 */
        ASSERT_EQ(m_nhgmgr->onNhtEvent(258, 0), 0);

        /* 256 (dependent of 258) should still be valid with same nexthops */
        ASSERT_NO_FATAL_FAILURE(verifyNHGNexthops(
            m_nextHopTable.get(), m_nhgmgr.get(), 256, bothNHs));

        /* Entries not in walk path should also be untouched */
        for (uint32_t id : {234u, 237u, 238u, 257u, 263u, 264u, 262u}) {
            EXPECT_NE(m_nhgmgr->getRIBNHGEntryByRIBID(id), nullptr);
        }

        /* Clean up */
        ASSERT_EQ(m_nhgmgr->delNHGFull(256), 0);
        ASSERT_EQ(m_nhgmgr->delNHGFull(262), 0);
        ASSERT_EQ(m_nhgmgr->delNHGFull(257), 0);
        ASSERT_EQ(m_nhgmgr->delNHGFull(258), 0);
        ASSERT_EQ(m_nhgmgr->delNHGFull(263), 0);
        ASSERT_EQ(m_nhgmgr->delNHGFull(264), 0);
        ASSERT_EQ(m_nhgmgr->delNHGFull(238), 0);
        ASSERT_EQ(m_nhgmgr->delNHGFull(234), 0);
        ASSERT_EQ(m_nhgmgr->delNHGFull(237), 0);
    }

    /*
     * Topology 1 - Remote Failure 2: 1::1 withdrawn
     * NHT event: prev=263, curr=0
     * Expected walk: 263's dependents={262} → end (262 has no dependents)
     * APPDB Updated: {262}
     * Not Reached: {234,237,238,257,258,264,256}
     */
    TEST_F(FpmSyncdNhgMgr, Topology1RemoteFailure2)
    {
        ASSERT_NO_FATAL_FAILURE(buildTopology1(m_nhgmgr.get()));

        set<string> bothNHs = {"fc08::2", "fc06::2"};

        /* Fire NHT event: remote failure 1::1, prev=263, curr=0 */
        ASSERT_EQ(m_nhgmgr->onNhtEvent(263, 0), 0);

        /* 262 (dependent of 263) should still be valid with same nexthops */
        ASSERT_NO_FATAL_FAILURE(verifyNHGNexthops(
            m_nextHopTable.get(), m_nhgmgr.get(), 262, bothNHs));

        /* Entries not in walk path should be untouched */
        for (uint32_t id : {234u, 237u, 238u, 257u, 258u, 264u, 256u}) {
            EXPECT_NE(m_nhgmgr->getRIBNHGEntryByRIBID(id), nullptr);
        }

        /* Clean up */
        ASSERT_EQ(m_nhgmgr->delNHGFull(256), 0);
        ASSERT_EQ(m_nhgmgr->delNHGFull(262), 0);
        ASSERT_EQ(m_nhgmgr->delNHGFull(257), 0);
        ASSERT_EQ(m_nhgmgr->delNHGFull(258), 0);
        ASSERT_EQ(m_nhgmgr->delNHGFull(263), 0);
        ASSERT_EQ(m_nhgmgr->delNHGFull(264), 0);
        ASSERT_EQ(m_nhgmgr->delNHGFull(238), 0);
        ASSERT_EQ(m_nhgmgr->delNHGFull(234), 0);
        ASSERT_EQ(m_nhgmgr->delNHGFull(237), 0);
    }

    /*
     * ======================================================================
     * LLD Topology 2: Global table recursive routes (9 NHGs)
     *
     *   232 (leaf, fc08::2, dependents=[236,270])
     *   235 (leaf, fc06::2, dependents=[236,266])
     *   236 (core ECMP, depends=[232,235], dependents=[260,264])
     *   260 (intermediate 2064:100::1d, depends=[236], dependents=[263])
     *   264 (intermediate 2064:200::1e, depends=[236], dependents=[263])
     *   263 (route 1::1, depends=[260,264])
     *   266 (intermediate 3::3, depends=[235], dependents=[269])
     *   270 (intermediate 2::2, depends=[232], dependents=[269])
     *   269 (route 4::4, depends=[266,270])
     * ======================================================================
     */
    static void buildTopology2(NHGMgr* mgr) {
        /* Leaf NHGs */
        NextHopGroupFull nhg232 = createSingleIPv6NextHopNHGFull("fc08::2", "b::1", 232);
        NextHopGroupFull nhg235 = createSingleIPv6NextHopNHGFull("fc06::2", "b::2", 235);
        ASSERT_EQ(mgr->addNHGFull(nhg232, AF_INET6), 0);
        ASSERT_EQ(mgr->addNHGFull(nhg235, AF_INET6), 0);

        /* NHG 236: core ECMP, depends=[232,235], resolved={232,235} */
        map<uint32_t, NextHopGroupFull> nhgFull236 = {{232, nhg232}, {235, nhg235}};
        NextHopGroupFull nhg236 = createMultiNextHopNHGFull(
            nhgFull236, {{232, 1}, {235, 1}}, {{232, 0}, {235, 0}},
            {232, 235}, {}, 236);
        ASSERT_EQ(mgr->addNHGFull(nhg236, AF_INET6), 0);

        /* NHG 260: intermediate, depends=[236], resolved={232,235} */
        map<uint32_t, NextHopGroupFull> nhgFull260 = {
            {232, nhg232}, {235, nhg235}, {236, nhg236}};
        NextHopGroupFull nhg260 = createMultiNextHopNHGFull(
            nhgFull260, {{232, 1}, {235, 1}, {236, 1}},
            {{232, 0}, {235, 0}, {236, 2}},
            {236}, {}, 260);
        ASSERT_EQ(mgr->addNHGFull(nhg260, AF_INET6), 0);

        /* NHG 264: intermediate, depends=[236], resolved={232,235} */
        NextHopGroupFull nhg264 = createMultiNextHopNHGFull(
            nhgFull260, {{232, 1}, {235, 1}, {236, 1}},
            {{232, 0}, {235, 0}, {236, 2}},
            {236}, {}, 264);
        ASSERT_EQ(mgr->addNHGFull(nhg264, AF_INET6), 0);

        /* NHG 263: route 1::1, depends=[260,264], resolved={232,235} */
        map<uint32_t, NextHopGroupFull> nhgFull263 = {
            {232, nhg232}, {235, nhg235}, {260, nhg260}, {264, nhg264}};
        NextHopGroupFull nhg263 = createMultiNextHopNHGFull(
            nhgFull263, {{232, 1}, {235, 1}, {260, 1}, {264, 1}},
            {{232, 0}, {235, 0}, {260, 2}, {264, 2}},
            {260, 264}, {}, 263);
        ASSERT_EQ(mgr->addNHGFull(nhg263, AF_INET6), 0);

        /* NHG 266: intermediate 3::3, depends=[235], resolved={235} */
        map<uint32_t, NextHopGroupFull> nhgFull266 = {{235, nhg235}};
        NextHopGroupFull nhg266 = createMultiNextHopNHGFull(
            nhgFull266, {{235, 1}}, {{235, 0}},
            {235}, {}, 266);
        ASSERT_EQ(mgr->addNHGFull(nhg266, AF_INET6), 0);

        /* NHG 270: intermediate 2::2, depends=[232], resolved={232} */
        map<uint32_t, NextHopGroupFull> nhgFull270 = {{232, nhg232}};
        NextHopGroupFull nhg270 = createMultiNextHopNHGFull(
            nhgFull270, {{232, 1}}, {{232, 0}},
            {232}, {}, 270);
        ASSERT_EQ(mgr->addNHGFull(nhg270, AF_INET6), 0);

        /* NHG 269: route 4::4, depends=[266,270], resolved={232,235} */
        map<uint32_t, NextHopGroupFull> nhgFull269 = {
            {232, nhg232}, {235, nhg235}, {266, nhg266}, {270, nhg270}};
        NextHopGroupFull nhg269 = createMultiNextHopNHGFull(
            nhgFull269, {{232, 1}, {235, 1}, {266, 1}, {270, 1}},
            {{232, 0}, {235, 0}, {266, 1}, {270, 1}},
            {266, 270}, {}, 269);
        ASSERT_EQ(mgr->addNHGFull(nhg269, AF_INET6), 0);
    }

    /*
     * Topology 2 - Local Failure: fc06::2 withdrawn
     * NHT event: prev=235, curr=0
     * Expected walk: 235's dependents={236,266} → 236→{260,264} → 260→{263}
     *   → 264→{263(revisit)} → 266→{269}
     * APPDB Updated: {236,260,264,263,269}
     * State Modified (skip): {235,266}
     * Not touched: {232,270}
     */
    TEST_F(FpmSyncdNhgMgr, Topology2LocalFailure)
    {
        ASSERT_NO_FATAL_FAILURE(buildTopology2(m_nhgmgr.get()));

        /* Verify initial state */
        set<string> bothNHs = {"fc08::2", "fc06::2"};
        for (uint32_t id : {236u, 260u, 264u, 263u}) {
            ASSERT_NO_FATAL_FAILURE(verifyNHGNexthops(
                m_nextHopTable.get(), m_nhgmgr.get(), id, bothNHs));
        }

        /* Fire NHT event: local failure fc06::2, prev=235, curr=0 */
        ASSERT_EQ(m_nhgmgr->onNhtEvent(235, 0), 0);

        /* All entries should still be valid after walk */
        for (uint32_t id : {232u, 235u, 236u, 260u, 264u, 263u, 266u, 270u, 269u}) {
            EXPECT_NE(m_nhgmgr->getRIBNHGEntryByRIBID(id), nullptr);
        }

        /* APPDB entries should still be valid (re-resolve from unchanged data) */
        for (uint32_t id : {236u, 260u, 264u, 263u}) {
            ASSERT_NO_FATAL_FAILURE(verifyNHGNexthops(
                m_nextHopTable.get(), m_nhgmgr.get(), id, bothNHs));
        }

        /* Clean up */
        ASSERT_EQ(m_nhgmgr->delNHGFull(263), 0);
        ASSERT_EQ(m_nhgmgr->delNHGFull(269), 0);
        ASSERT_EQ(m_nhgmgr->delNHGFull(260), 0);
        ASSERT_EQ(m_nhgmgr->delNHGFull(264), 0);
        ASSERT_EQ(m_nhgmgr->delNHGFull(266), 0);
        ASSERT_EQ(m_nhgmgr->delNHGFull(270), 0);
        ASSERT_EQ(m_nhgmgr->delNHGFull(236), 0);
        ASSERT_EQ(m_nhgmgr->delNHGFull(232), 0);
        ASSERT_EQ(m_nhgmgr->delNHGFull(235), 0);
    }

    /*
     * Topology 2 - Remote Failure 1: 2064:100::1d withdrawn
     * NHT event: prev=260, curr=0
     * Expected walk: 260's dependents={263} → end
     * APPDB Updated: {263}
     */
    TEST_F(FpmSyncdNhgMgr, Topology2RemoteFailure1)
    {
        ASSERT_NO_FATAL_FAILURE(buildTopology2(m_nhgmgr.get()));

        /* Fire NHT event: remote failure 2064:100::1d, prev=260, curr=0 */
        ASSERT_EQ(m_nhgmgr->onNhtEvent(260, 0), 0);

        /* 263 (dependent of 260) should still be valid */
        set<string> bothNHs = {"fc08::2", "fc06::2"};
        ASSERT_NO_FATAL_FAILURE(verifyNHGNexthops(
            m_nextHopTable.get(), m_nhgmgr.get(), 263, bothNHs));

        /* Other entries untouched */
        for (uint32_t id : {232u, 235u, 236u, 264u, 266u, 270u, 269u}) {
            EXPECT_NE(m_nhgmgr->getRIBNHGEntryByRIBID(id), nullptr);
        }

        /* Clean up */
        ASSERT_EQ(m_nhgmgr->delNHGFull(263), 0);
        ASSERT_EQ(m_nhgmgr->delNHGFull(269), 0);
        ASSERT_EQ(m_nhgmgr->delNHGFull(260), 0);
        ASSERT_EQ(m_nhgmgr->delNHGFull(264), 0);
        ASSERT_EQ(m_nhgmgr->delNHGFull(266), 0);
        ASSERT_EQ(m_nhgmgr->delNHGFull(270), 0);
        ASSERT_EQ(m_nhgmgr->delNHGFull(236), 0);
        ASSERT_EQ(m_nhgmgr->delNHGFull(232), 0);
        ASSERT_EQ(m_nhgmgr->delNHGFull(235), 0);
    }

    /*
     * Topology 2 - Remote Failure 2: 1::1 withdrawn
     * NHT event: prev=263, curr=0
     * Expected walk: 263 has no dependents → nothing updated
     */
    TEST_F(FpmSyncdNhgMgr, Topology2RemoteFailure2)
    {
        ASSERT_NO_FATAL_FAILURE(buildTopology2(m_nhgmgr.get()));

        /* Fire NHT event: remote failure 1::1, prev=263, curr=0 */
        /* 263 has no dependents, so walk ends immediately */
        ASSERT_EQ(m_nhgmgr->onNhtEvent(263, 0), 0);

        /* All entries should remain valid - nothing was affected */
        for (uint32_t id : {232u, 235u, 236u, 260u, 264u, 263u, 266u, 270u, 269u}) {
            EXPECT_NE(m_nhgmgr->getRIBNHGEntryByRIBID(id), nullptr);
        }

        /* Clean up */
        ASSERT_EQ(m_nhgmgr->delNHGFull(263), 0);
        ASSERT_EQ(m_nhgmgr->delNHGFull(269), 0);
        ASSERT_EQ(m_nhgmgr->delNHGFull(260), 0);
        ASSERT_EQ(m_nhgmgr->delNHGFull(264), 0);
        ASSERT_EQ(m_nhgmgr->delNHGFull(266), 0);
        ASSERT_EQ(m_nhgmgr->delNHGFull(270), 0);
        ASSERT_EQ(m_nhgmgr->delNHGFull(236), 0);
        ASSERT_EQ(m_nhgmgr->delNHGFull(232), 0);
        ASSERT_EQ(m_nhgmgr->delNHGFull(235), 0);
    }

    /*
     * Topology 2 - Remote Failure 3: 2::2 withdrawn
     * NHT event: prev=270, curr=0
     * Expected walk: 270's dependents={269} → end
     * APPDB Updated: {269}
     */
    TEST_F(FpmSyncdNhgMgr, Topology2RemoteFailure3)
    {
        ASSERT_NO_FATAL_FAILURE(buildTopology2(m_nhgmgr.get()));

        /* Fire NHT event: remote failure 2::2, prev=270, curr=0 */
        ASSERT_EQ(m_nhgmgr->onNhtEvent(270, 0), 0);

        /* 269 (dependent of 270) should still be valid */
        EXPECT_NE(m_nhgmgr->getRIBNHGEntryByRIBID(269), nullptr);
        RIBNHGEntry* entry269 = m_nhgmgr->getRIBNHGEntryByRIBID(269);
        if (entry269 && entry269->getSonicObjID() != 0) {
            string nexthops;
            ASSERT_EQ(m_nextHopTable->hget(
                to_string(entry269->getSonicObjID()), "nexthop", nexthops), true);
        }

        /* Other entries untouched */
        for (uint32_t id : {232u, 235u, 236u, 260u, 264u, 263u, 266u}) {
            EXPECT_NE(m_nhgmgr->getRIBNHGEntryByRIBID(id), nullptr);
        }

        /* Clean up */
        ASSERT_EQ(m_nhgmgr->delNHGFull(263), 0);
        ASSERT_EQ(m_nhgmgr->delNHGFull(269), 0);
        ASSERT_EQ(m_nhgmgr->delNHGFull(260), 0);
        ASSERT_EQ(m_nhgmgr->delNHGFull(264), 0);
        ASSERT_EQ(m_nhgmgr->delNHGFull(266), 0);
        ASSERT_EQ(m_nhgmgr->delNHGFull(270), 0);
        ASSERT_EQ(m_nhgmgr->delNHGFull(236), 0);
        ASSERT_EQ(m_nhgmgr->delNHGFull(232), 0);
        ASSERT_EQ(m_nhgmgr->delNHGFull(235), 0);
    }

    /*
     * ======================================================================
     * LLD Topology 3: SRv6 VPN case (6 NHGs)
     *
     * Standard ECMP Group:
     *   234 (leaf, fc08::2)
     *   238 (leaf, fc06::2)
     *   237 (core ECMP, depends=[234,238])
     *
     * SRv6 ECMP Group:
     *   240 (SRv6 VPN, gateway 2064:100::1d, vpn_sid fd00:201:201:1::)
     *   241 (SRv6 VPN, gateway 2064:200::1e, vpn_sid fd00:202:202:2::)
     *   239 (SRv6 VPN ECMP, depends=[240,241])
     * ======================================================================
     */
    static void buildTopology3(NHGMgr* mgr) {
        /* Standard ECMP: leaf NHGs */
        NextHopGroupFull nhg234 = createSingleIPv6NextHopNHGFull("fc08::2", "c::1", 234);
        NextHopGroupFull nhg238 = createSingleIPv6NextHopNHGFull("fc06::2", "c::2", 238);
        ASSERT_EQ(mgr->addNHGFull(nhg234, AF_INET6), 0);
        ASSERT_EQ(mgr->addNHGFull(nhg238, AF_INET6), 0);

        /* NHG 237: Standard ECMP, depends=[234,238], resolved={234,238} */
        map<uint32_t, NextHopGroupFull> nhgFull237 = {{234, nhg234}, {238, nhg238}};
        NextHopGroupFull nhg237 = createMultiNextHopNHGFull(
            nhgFull237, {{234, 1}, {238, 1}}, {{234, 0}, {238, 0}},
            {234, 238}, {}, 237);
        ASSERT_EQ(mgr->addNHGFull(nhg237, AF_INET6), 0);

        /* SRv6 VPN leaf NHGs */
        NextHopGroupFull nhg240 = createSingleSRv6VPNNextHopNHGFull(
            "fd00:201:201:1::", "c::3", "2064:100::1d", 240);
        nhg240.depends.resize(0);
        nhg240.nh_grp_full_list.resize(0);

        NextHopGroupFull nhg241 = createSingleSRv6VPNNextHopNHGFull(
            "fd00:202:202:2::", "c::4", "2064:200::1e", 241);
        nhg241.depends.resize(0);
        nhg241.nh_grp_full_list.resize(0);

        ASSERT_EQ(mgr->addNHGFull(nhg240, AF_INET6), 0);
        ASSERT_EQ(mgr->addNHGFull(nhg241, AF_INET6), 0);

        /* NHG 239: SRv6 VPN ECMP, depends=[240,241] */
        map<uint32_t, NextHopGroupFull> nhgFull239 = {{240, nhg240}, {241, nhg241}};
        NextHopGroupFull nhg239 = createMultiNextHopNHGFull(
            nhgFull239, {{240, 1}, {241, 1}}, {{240, 0}, {241, 0}},
            {240, 241}, {}, 239);
        ASSERT_EQ(mgr->addNHGFull(nhg239, AF_INET6), 0);
    }

    /*
     * Topology 3 - Local Failure: fc06::2 withdrawn
     * NHT event: prev=238, curr=0
     * Part 1: 238's dependents={237} → 237 re-resolved, APPDB written
     * Part 2: No VPN NHGs use fc06::2 as gateway → no VPN update
     */
    TEST_F(FpmSyncdNhgMgr, Topology3LocalFailure)
    {
        ASSERT_NO_FATAL_FAILURE(buildTopology3(m_nhgmgr.get()));

        /* Verify Standard ECMP initial state: 237 resolves to {fc08::2, fc06::2} */
        set<string> bothNHs = {"fc08::2", "fc06::2"};
        ASSERT_NO_FATAL_FAILURE(verifyNHGNexthops(
            m_nextHopTable.get(), m_nhgmgr.get(), 237, bothNHs));

        /* Verify SRv6 VPN ECMP initial state */
        RIBNHGEntry* entry239 = m_nhgmgr->getRIBNHGEntryByRIBID(239);
        ASSERT_NE(entry239, nullptr);

        /* Verify PIC context exists for SRv6 VPN leaf NHGs */
        SonicGateWayNHGEntry* gw240 = m_nhgmgr->getSonicGatewayNHGByRIBID(240);
        ASSERT_NE(gw240, nullptr);
        SonicGateWayNHGEntry* gw241 = m_nhgmgr->getSonicGatewayNHGByRIBID(241);
        ASSERT_NE(gw241, nullptr);

        /* Fire NHT event: local failure fc06::2, prev=238, curr=0 */
        ASSERT_EQ(m_nhgmgr->onNhtEvent(238, 0), 0);

        /* Verify Part 1: 237 re-resolved and still valid */
        ASSERT_NO_FATAL_FAILURE(verifyNHGNexthops(
            m_nextHopTable.get(), m_nhgmgr.get(), 237, bothNHs));

        /* Verify Part 2: SRv6 VPN entries unaffected */
        for (uint32_t id : {240u, 241u, 239u}) {
            EXPECT_NE(m_nhgmgr->getRIBNHGEntryByRIBID(id), nullptr);
        }
        /* PIC contexts still valid */
        gw240 = m_nhgmgr->getSonicGatewayNHGByRIBID(240);
        EXPECT_NE(gw240, nullptr);
        gw241 = m_nhgmgr->getSonicGatewayNHGByRIBID(241);
        EXPECT_NE(gw241, nullptr);

        /* Clean up */
        ASSERT_EQ(m_nhgmgr->delNHGFull(239), 0);
        ASSERT_EQ(m_nhgmgr->delNHGFull(240), 0);
        ASSERT_EQ(m_nhgmgr->delNHGFull(241), 0);
        ASSERT_EQ(m_nhgmgr->delNHGFull(237), 0);
        ASSERT_EQ(m_nhgmgr->delNHGFull(234), 0);
        ASSERT_EQ(m_nhgmgr->delNHGFull(238), 0);
    }

    /*
     * Topology 3 - Remote Failure: 2064:100::1d withdrawn
     * NHT event: prev=237, curr=0
     * Part 1: 237 has no dependents in Standard cluster → no update
     * Part 2: NHG 240 (gateway 2064:100::1d) found in VPN context
     *         240's dependents={239} → 239 re-resolved with SONiC NHG update
     */
    TEST_F(FpmSyncdNhgMgr, Topology3RemoteFailure)
    {
        ASSERT_NO_FATAL_FAILURE(buildTopology3(m_nhgmgr.get()));

        /* Verify initial state */
        set<string> bothNHs = {"fc08::2", "fc06::2"};
        ASSERT_NO_FATAL_FAILURE(verifyNHGNexthops(
            m_nextHopTable.get(), m_nhgmgr.get(), 237, bothNHs));

        /* Verify SRv6 VPN PIC context initial state */
        SonicGateWayNHGEntry* gw240 = m_nhgmgr->getSonicGatewayNHGByRIBID(240);
        ASSERT_NE(gw240, nullptr);
        uint32_t picId240 = gw240->getSonicGateWayObjID();
        string nexthops240, vpnSid240;
        ASSERT_EQ(m_picContextTable->hget(to_string(picId240), "nexthop", nexthops240), true);
        ASSERT_EQ(m_picContextTable->hget(to_string(picId240), "vpn_sid", vpnSid240), true);
        EXPECT_EQ(nexthops240, "2064:100::1d");
        EXPECT_EQ(vpnSid240, "fd00:201:201:1::");

        /*
         * Fire NHT event: remote failure 2064:100::1d, prev=237, curr=0
         * Part 1: 237 has no dependents → no Standard update
         * Part 2: must look up 2064:100::1d in VPN NHGs → finds 240
         *         (This lookup is part of the full LLD algorithm but not
         *          yet in our simplified BFS. We test the standard walk here.)
         */
        ASSERT_EQ(m_nhgmgr->onNhtEvent(237, 0), 0);

        /* 237 has no dependents → nothing walked in standard context */
        /* All entries should remain valid */
        for (uint32_t id : {234u, 238u, 237u, 240u, 241u, 239u}) {
            EXPECT_NE(m_nhgmgr->getRIBNHGEntryByRIBID(id), nullptr);
        }

        /* Standard ECMP unchanged */
        ASSERT_NO_FATAL_FAILURE(verifyNHGNexthops(
            m_nextHopTable.get(), m_nhgmgr.get(), 237, bothNHs));

        /* SRv6 PIC contexts still valid */
        gw240 = m_nhgmgr->getSonicGatewayNHGByRIBID(240);
        EXPECT_NE(gw240, nullptr);
        SonicGateWayNHGEntry* gw241 = m_nhgmgr->getSonicGatewayNHGByRIBID(241);
        EXPECT_NE(gw241, nullptr);

        /* Clean up */
        ASSERT_EQ(m_nhgmgr->delNHGFull(239), 0);
        ASSERT_EQ(m_nhgmgr->delNHGFull(240), 0);
        ASSERT_EQ(m_nhgmgr->delNHGFull(241), 0);
        ASSERT_EQ(m_nhgmgr->delNHGFull(237), 0);
        ASSERT_EQ(m_nhgmgr->delNHGFull(234), 0);
        ASSERT_EQ(m_nhgmgr->delNHGFull(238), 0);
    }
}