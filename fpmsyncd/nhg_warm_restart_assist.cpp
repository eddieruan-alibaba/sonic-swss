#include "nhg_warm_restart_assist.h"
#include "logger.h"
#include "warm_restart.h"

#include <arpa/inet.h>

using namespace swss;

NHGWarmRestartAssist::NHGWarmRestartAssist(RedisPipeline *pipeline,
                                           const std::string &appName,
                                           const std::string &dockerName,
                                           NHGMgr *nhgMgr,
                                           DBConnector *stateDb) :
    AppRestartAssist(pipeline, appName, dockerName, 0),
    m_nhgMgr(nhgMgr),
    m_nhgWarmStateTable(stateDb, FPMSYNCD_NHG_WARM_STATE_TABLE)
{
}

NHGWarmRestartAssist::~NHGWarmRestartAssist()
{
}

void NHGWarmRestartAssist::readNHGStateFromDB()
{
    std::vector<std::string> keys;
    m_nhgWarmStateTable.getKeys(keys);

    for (const auto &key : keys)
    {
        if (key == FPMSYNCD_NHG_WARM_ID_STATE_KEY)
        {
            continue;
        }

        std::vector<FieldValueTuple> fvs;
        if (!m_nhgWarmStateTable.get(key, fvs))
        {
            continue;
        }

        NHGCacheEntry entry;
        entry.zebra_id = static_cast<uint32_t>(std::stoul(key));
        entry.sonic_obj_id = 0;
        entry.sonic_pic_id = 0;
        entry.sonic_obj_type = SONIC_NHG_OBJ_TYPE_NHG_NORMAL;
        entry.is_single = true;
        entry.is_shared = false;
        entry.af = 0;
        entry.state = NHG_STALE;

        for (const auto &fv : fvs)
        {
            const auto &field = fvField(fv);
            const auto &value = fvValue(fv);

            if (field == "sonic_obj_id")
            {
                entry.sonic_obj_id = static_cast<uint32_t>(std::stoul(value));
            }
            else if (field == "sonic_pic_id")
            {
                entry.sonic_pic_id = static_cast<uint32_t>(std::stoul(value));
            }
            else if (field == "sonic_obj_type")
            {
                entry.sonic_obj_type = static_cast<sonicNhgObjType>(std::stoi(value));
            }
            else if (field == "is_single")
            {
                entry.is_single = (value == "1");
            }
            else if (field == "is_shared")
            {
                entry.is_shared = (value == "1");
            }
            else if (field == "af")
            {
                entry.af = static_cast<uint8_t>(std::stoi(value));
            }
            else if (field == "sonic_nhg_key_hash")
            {
                entry.sonic_nhg_key_hash = value;
            }
        }

        m_nhgCacheMap[entry.zebra_id] = entry;
    }

    int rc = m_nhgMgr->recoverFromWarmState();
    if (rc != 0)
    {
        SWSS_LOG_ERROR("NHG warm restart: failed to recover SonicIDMgr state from DB");
    }

    m_nhgWarmStartInProgress = true;

    SWSS_LOG_NOTICE("NHG warm restart: loaded %zu entries from STATE_DB",
                    m_nhgCacheMap.size());
}

void NHGWarmRestartAssist::insertNHGToMap(const NextHopGroupFull &nhg, uint8_t af, bool is_delete)
{
    uint32_t zebra_id = nhg.id;

    if (is_delete)
    {
        auto it = m_nhgCacheMap.find(zebra_id);
        if (it != m_nhgCacheMap.end())
        {
            it->second.state = NHG_DELETE;
        }
        m_pendingEntries[zebra_id] = {nhg, af, true};
        return;
    }

    auto it = m_nhgCacheMap.find(zebra_id);
    if (it != m_nhgCacheMap.end() && it->second.state == NHG_STALE)
    {
        SonicNHGObjectKey incomingKey = buildKeyFromNHGFull(nhg, af);

        if (compareWithIncoming(it->second, incomingKey))
        {
            it->second.state = NHG_SAME;
        }
        else
        {
            it->second.state = NHG_NEW;
            m_pendingEntries[zebra_id] = {nhg, af, false};
        }
    }
    else
    {
        NHGCacheEntry newEntry;
        newEntry.zebra_id = zebra_id;
        newEntry.sonic_obj_id = 0;
        newEntry.sonic_pic_id = 0;
        newEntry.sonic_obj_type = SONIC_NHG_OBJ_TYPE_NHG_NORMAL;
        newEntry.is_single = true;
        newEntry.is_shared = false;
        newEntry.af = af;
        newEntry.state = NHG_NEW;

        // Determine is_single from resolved group count
        uint32_t resolvedCount = 0;
        for (const auto &member : nhg.nh_grp_full_list)
        {
            if (member.num_direct == 0)
            {
                resolvedCount++;
            }
        }
        newEntry.is_single = (resolvedCount <= 1);

        m_nhgCacheMap[zebra_id] = newEntry;
        m_pendingEntries[zebra_id] = {nhg, af, false};
    }
}

bool NHGWarmRestartAssist::compareWithIncoming(const NHGCacheEntry &cached,
                                                const SonicNHGObjectKey &incomingKey)
{
    if (cached.sonic_nhg_key_hash.empty())
    {
        return false;
    }

    SonicNHGObjectKey cachedKey = SonicNHGObjectKey::deserialize(cached.sonic_nhg_key_hash);
    return (cachedKey == incomingKey);
}

void NHGWarmRestartAssist::reconcileNHG()
{
    SWSS_LOG_NOTICE("NHG warm restart: starting reconciliation with %zu cache entries",
                    m_nhgCacheMap.size());

    reconcileSingleHop();
    reconcileMultiHop();

    m_nhgCacheMap.clear();
    m_pendingEntries.clear();
    m_nhgWarmStartInProgress = false;

    SWSS_LOG_NOTICE("NHG warm restart: reconciliation complete");
}

void NHGWarmRestartAssist::reconcileSingleHop()
{
    for (auto &kv : m_nhgCacheMap)
    {
        auto &entry = kv.second;
        if (!entry.is_single)
        {
            continue;
        }

        uint32_t zebra_id = entry.zebra_id;

        switch (entry.state)
        {
            case NHG_STALE:
                SWSS_LOG_INFO("NHG warm restart: deleting stale single-hop zebra_id=%u", zebra_id);
                m_nhgMgr->delNHGFull(zebra_id);
                break;

            case NHG_SAME:
                SWSS_LOG_DEBUG("NHG warm restart: single-hop zebra_id=%u unchanged", zebra_id);
                break;

            case NHG_NEW:
            {
                auto pendIt = m_pendingEntries.find(zebra_id);
                if (pendIt != m_pendingEntries.end())
                {
                    SWSS_LOG_INFO("NHG warm restart: updating single-hop zebra_id=%u", zebra_id);
                    m_nhgMgr->delNHGFull(zebra_id);
                    m_nhgMgr->addNHGFull(pendIt->second.nhg, pendIt->second.af);
                }
                break;
            }

            case NHG_DELETE:
                SWSS_LOG_INFO("NHG warm restart: delete request for single-hop zebra_id=%u", zebra_id);
                m_nhgMgr->delNHGFull(zebra_id);
                break;
        }
    }
}

void NHGWarmRestartAssist::reconcileMultiHop()
{
    for (auto &kv : m_nhgCacheMap)
    {
        auto &entry = kv.second;
        if (entry.is_single)
        {
            continue;
        }

        uint32_t zebra_id = entry.zebra_id;

        switch (entry.state)
        {
            case NHG_STALE:
                SWSS_LOG_INFO("NHG warm restart: deleting stale multi-hop zebra_id=%u", zebra_id);
                m_nhgMgr->delNHGFull(zebra_id);
                break;

            case NHG_SAME:
                SWSS_LOG_DEBUG("NHG warm restart: multi-hop zebra_id=%u unchanged", zebra_id);
                break;

            case NHG_NEW:
            {
                auto pendIt = m_pendingEntries.find(zebra_id);
                if (pendIt != m_pendingEntries.end())
                {
                    SWSS_LOG_INFO("NHG warm restart: updating multi-hop zebra_id=%u", zebra_id);
                    m_nhgMgr->delNHGFull(zebra_id);
                    m_nhgMgr->addNHGFull(pendIt->second.nhg, pendIt->second.af);
                }
                break;
            }

            case NHG_DELETE:
                SWSS_LOG_INFO("NHG warm restart: delete request for multi-hop zebra_id=%u", zebra_id);
                m_nhgMgr->delNHGFull(zebra_id);
                break;
        }
    }
}

SonicNHGObjectKey NHGWarmRestartAssist::buildKeyFromNHGFull(const NextHopGroupFull &nhg, uint8_t af)
{
    SonicNHGObjectKey key;
    key.type = SONIC_NHG_OBJ_TYPE_NHG_NORMAL;
    key.nexthop = "";
    key.ifName = "";
    key.segSrc = "";
    key.vpnSid = "";

    std::vector<std::pair<uint32_t, uint8_t>> resolvedGroup;
    for (const auto &member : nhg.nh_grp_full_list)
    {
        if (member.num_direct == 0)
        {
            resolvedGroup.push_back(std::make_pair(member.id, member.weight));
        }
    }

    bool is_single = (resolvedGroup.size() <= 1);

    if (is_single)
    {
        if (af == AF_INET)
        {
            char gateway[INET_ADDRSTRLEN] = {0};
            inet_ntop(AF_INET, &nhg.gate.ipv4, gateway, INET_ADDRSTRLEN);
            key.nexthop = gateway;
        }
        else if (af == AF_INET6)
        {
            char gateway[INET6_ADDRSTRLEN] = {0};
            inet_ntop(AF_INET6, &nhg.gate.ipv6, gateway, INET6_ADDRSTRLEN);
            key.nexthop = gateway;
        }
        key.ifName = nhg.ifname;

        if (nhg.nh_srv6 != nullptr && nhg.nh_srv6->seg6_segs != nullptr)
        {
            char sid[INET6_ADDRSTRLEN] = {0};
            char seg_src[INET6_ADDRSTRLEN] = {0};
            inet_ntop(AF_INET6, &nhg.nh_srv6->seg6_segs->seg[0], sid, INET6_ADDRSTRLEN);
            key.vpnSid = sid;
            inet_ntop(AF_INET6, &nhg.nh_srv6->seg6_src, seg_src, INET6_ADDRSTRLEN);
            key.segSrc = seg_src;
        }
    }
    else
    {
        for (const auto &member : resolvedGroup)
        {
            key.groupMember.push_back(member);
        }
    }

    return key;
}
