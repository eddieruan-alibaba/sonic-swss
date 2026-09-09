#include "fpmsyncd/fpmlink.h"

#include <swss/netdispatcher.h>

#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include <linux/nexthop.h>

using namespace swss;

using ::testing::_;

class MockMsgHandler : public NetMsg
{
public:
    MOCK_METHOD2(onMsg, void(int, nl_object*));
};

class MockRawRouteSync : public RouteSync
{
public:
    MockRawRouteSync(RedisPipeline *pipeline, RedisPipeline *appStatePipeline) :
        RouteSync(pipeline, appStatePipeline)
    {
    }

    MOCK_METHOD(void, onMsgRaw, (nlmsghdr *), (override));
};

class FpmLinkTest : public ::testing::Test
{
public:
    void SetUp() override
    {
        NetDispatcher::getInstance().registerMessageHandler(RTM_NEWROUTE, &m_mock);
        NetDispatcher::getInstance().registerMessageHandler(RTM_DELROUTE, &m_mock);
    }

    void TearDown() override
    {
        NetDispatcher::getInstance().unregisterMessageHandler(RTM_NEWROUTE);
        NetDispatcher::getInstance().unregisterMessageHandler(RTM_DELROUTE);
    }

    DBConnector m_db{"APPL_DB", 0};
    RedisPipeline m_pipeline{&m_db, 1};
    DBConnector m_appl_state_db{"APPL_STATE_DB", 0};
    RedisPipeline m_app_state_pipeline{&m_appl_state_db};
    MockRawRouteSync m_routeSync{&m_pipeline, &m_app_state_pipeline};
    FpmLink m_fpm{&m_routeSync};
    MockMsgHandler m_mock;
};

TEST_F(FpmLinkTest, SingleNlMessageInFpmMessage)
{
    // Single FPM message containing single RTM_NEWROUTE
    alignas(fpm_msg_hdr_t) unsigned char fpmMsgBuffer[] = {
        0x01, 0x01, 0x00, 0x40, 0x3C, 0x00, 0x00, 0x00, 0x18, 0x00, 0x01, 0x05, 0x00, 0x00, 0x00, 0x00, 0xE0,
        0x12, 0x6F, 0xC4, 0x02, 0x18, 0x00, 0x00, 0xFE, 0x02, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x08, 0x00,
        0x01, 0x00, 0x01, 0x01, 0x01, 0x00, 0x08, 0x00, 0x06, 0x00, 0x14, 0x00, 0x00, 0x00, 0x08, 0x00, 0x05,
        0x00, 0xAC, 0x1E, 0x38, 0xA6, 0x08, 0x00, 0x04, 0x00, 0x06, 0x00, 0x00, 0x00
    };

    EXPECT_CALL(m_mock, onMsg(_, _)).Times(1);

    m_fpm.processFpmMessage(reinterpret_cast<fpm_msg_hdr_t*>(static_cast<void*>(fpmMsgBuffer)));
}

TEST_F(FpmLinkTest, TwoNlMessagesInFpmMessage)
{
    // Single FPM message containing RTM_DELROUTE and RTM_NEWROUTE
    alignas(fpm_msg_hdr_t) unsigned char fpmMsgBuffer[] = {
        0x01, 0x01, 0x00, 0x6C, 0x2C, 0x00, 0x00, 0x00, 0x19, 0x00, 0x01, 0x04, 0x00, 0x00, 0x00, 0x00, 0xE0, 0x12,
        0x6F, 0xC4, 0x02, 0x18, 0x00, 0x00, 0xFE, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x01, 0x00,
        0x01, 0x01, 0x01, 0x00, 0x08, 0x00, 0x06, 0x00, 0x14, 0x00, 0x00, 0x00, 0x3C, 0x00, 0x00, 0x00, 0x18, 0x00,
        0x01, 0x05, 0x00, 0x00, 0x00, 0x00, 0xE0, 0x12, 0x6F, 0xC4, 0x02, 0x18, 0x00, 0x00, 0xFE, 0x02, 0x00, 0x01,
        0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x01, 0x00, 0x01, 0x01, 0x01, 0x00, 0x08, 0x00, 0x06, 0x00, 0x14, 0x00,
        0x00, 0x00, 0x08, 0x00, 0x05, 0x00, 0xAC, 0x1E, 0x38, 0xA7, 0x08, 0x00, 0x04, 0x00, 0x06, 0x00, 0x00, 0x00
    };

    EXPECT_CALL(m_mock, onMsg(_, _)).Times(2);

    m_fpm.processFpmMessage(reinterpret_cast<fpm_msg_hdr_t*>(static_cast<void*>(fpmMsgBuffer)));
}

TEST_F(FpmLinkTest, NhgFibMessagesUseRawDispatcher)
{
    alignas(fpm_msg_hdr_t) unsigned char buffer[FPM_MSG_HDR_LEN + NLMSG_LENGTH(sizeof(nhmsg))] = {};
    auto *fpmHeader = reinterpret_cast<fpm_msg_hdr_t *>(buffer);
    fpmHeader->version = FPM_PROTO_VERSION;
    fpmHeader->msg_type = FPM_MSG_TYPE_NETLINK;
    fpmHeader->msg_len = htons(static_cast<uint16_t>(sizeof(buffer)));

    auto *nlHeader = reinterpret_cast<nlmsghdr *>(fpm_msg_data(fpmHeader));
    nlHeader->nlmsg_len = NLMSG_LENGTH(sizeof(nhmsg));

    EXPECT_CALL(m_routeSync, onMsgRaw(nlHeader)).Times(2);

    nlHeader->nlmsg_type = RTM_NEWNHGFIB;
    m_fpm.processFpmMessage(fpmHeader);
    nlHeader->nlmsg_type = RTM_DELNHGFIB;
    m_fpm.processFpmMessage(fpmHeader);
}

