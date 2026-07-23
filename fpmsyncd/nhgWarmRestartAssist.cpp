#include <stdexcept>
#include <algorithm>
#include <queue>
#include <net/if.h>
#include <linux/rtnetlink.h>
#include <linux/nexthop.h>

#include "nhgWarmRestartAssist.h"
#include "routesync.h"
#include "logger.h"
#include "warm_restart.h"

/* NHG message type defines (must match fpmlink.h) */
#ifndef RTM_NEWNHGFIB
#define RTM_NEWNHGFIB 5000
#endif
#ifndef RTM_DELNHGFIB
#define RTM_DELNHGFIB 5001
#endif

/* SRv6 VPN route message type defines (must match fpmlink.h) */
#ifndef RTM_NEWSRV6VPNROUTE
#define RTM_NEWSRV6VPNROUTE 3000
#endif
#ifndef RTM_DELSRV6VPNROUTE
#define RTM_DELSRV6VPNROUTE 3001
#endif

/* NHA_JSON_STR attribute type (must match routesync.cpp) */
#ifndef NHA_JSON_STR
#define NHA_JSON_STR 2
#endif

/* NHA_RTA macro for extracting rtattrs from nhmsg (must match routesync.cpp) */
#ifndef NHA_RTA
#define NHA_RTA(r) \
    ((struct rtattr *)(((char *)(r)) + NLMSG_ALIGN(sizeof(struct nhmsg))))
#endif

/* Extern declaration for netlink rtattr parser (defined in fpmlink.cpp) */
extern void netlink_parse_rtattr(struct rtattr **tb, int max, struct rtattr *rta, int len);

using namespace swss;

/*
 * Parse a raw netlink NHG message (stored as vector<uint8_t>) into a
 * NextHopGroupFull object. This replicates the parsing logic from
 * RouteSync::onNextHopGroupFullMsg() for use by the NHG warm restart
 * reconcile functions.
 *
 * The raw buffer must contain a full nlmsghdr followed by the nhmsg payload
 * and rtattr attributes (NHA_ID, NHA_JSON_STR).
 *
 * @param rawMsg    Raw netlink message bytes (copy of nlmsghdr + payload)
 * @param nhg       [out] Parsed NextHopGroupFull object
 * @param af        [out] Address family from nhmsg
 * @param msgType   [out] Message type (RTM_NEWNHGFIB or RTM_DELNHGFIB)
 * @return 0 on success, -1 on parse error, -2 if NHG should be skipped (management interface)
 */
static int parseNHGFromRawMsg(const std::vector<uint8_t> &rawMsg,
                              fib::NextHopGroupFull &nhg,
                              uint8_t &af,
                              uint16_t &msgType)
{
    if (rawMsg.size() < sizeof(struct nlmsghdr)) {
        SWSS_LOG_ERROR("NHG warm restart: raw message too small (%zu bytes)", rawMsg.size());
        return -1;
    }

    /* The raw buffer was originally a mutable nlmsghdr; we only read from it here */
    struct nlmsghdr *h = reinterpret_cast<struct nlmsghdr *>(
            const_cast<uint8_t *>(rawMsg.data()));
    msgType = static_cast<uint16_t>(h->nlmsg_type);

    /* Compute payload length: total message length minus nlmsghdr minus nhmsg */
    int len = static_cast<int>(h->nlmsg_len) - static_cast<int>(NLMSG_LENGTH(sizeof(struct nhmsg)));
    if (len < 0) {
        SWSS_LOG_ERROR("NHG warm restart: invalid message length %u", h->nlmsg_len);
        return -1;
    }

    struct nhmsg *nhm = (struct nhmsg *)NLMSG_DATA(h);

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-align"
    struct rtattr *rta = NHA_RTA(nhm);
#pragma GCC diagnostic pop

    struct rtattr *tb[NHA_MAX + 1] = {};
    netlink_parse_rtattr(tb, NHA_MAX, rta, len);

    if (!tb[NHA_ID]) {
        SWSS_LOG_ERROR("NHG warm restart: nexthop group without NHA_ID");
        return -1;
    }

    uint32_t id = *((uint32_t *)RTA_DATA(tb[NHA_ID]));
    af = nhm->nh_family;

    /* For delete messages, only ID and msgType are needed */
    if (msgType == RTM_DELNHGFIB) {
        nhg.id = id;
        return 0;
    }

    /* Parse JSON string for new NHG messages */
    if (!tb[NHA_JSON_STR]) {
        SWSS_LOG_ERROR("NHG warm restart: NHG %u missing NHA_JSON_STR", id);
        return -1;
    }

    char *jsonStr = (char *)RTA_DATA(tb[NHA_JSON_STR]);
    try {
        nlohmann::ordered_json j = nlohmann::ordered_json::parse(jsonStr);
        fib::from_json(j, nhg);
    } catch (const std::exception &e) {
        SWSS_LOG_ERROR("NHG warm restart: JSON parse failed for NHG %u: %s", id, e.what());
        return -1;
    }

    /* Resolve interface name from ifindex (same as onNextHopGroupFullMsg) */
    char if_name[IFNAMSIZ] = {0};
    if (nhg.ifindex > 0 && if_indextoname(nhg.ifindex, if_name)) {
        nhg.ifname = std::string(if_name);
    }
    /* else: keep the ifname from JSON (or empty if not set) */

    /* Filter out management/docker interfaces (same as onNextHopGroupFullMsg) */
    if (nhg.ifname == "eth0" || nhg.ifname == "docker0") {
        SWSS_LOG_DEBUG("NHG warm restart: skip NHG %u on management interface %s",
                        id, nhg.ifname.c_str());
        return -2;
    }

    return 0;
}

/*
 * Compute a hash string from a FieldValueTuple vector for comparison.
 * The FVs are sorted by field name to produce a canonical representation,
 * so two FV vectors with the same content in different order will hash equally.
 */
static std::string computeFvHash(const std::vector<swss::FieldValueTuple> &fvs)
{
    std::vector<swss::FieldValueTuple> sorted = fvs;
    std::sort(sorted.begin(), sorted.end());
    std::string result;
    for (const auto &fv : sorted) {
        result += fvField(fv) + "=" + fvValue(fv) + ";";
    }
    return result;
}

NhgWarmRestartAssist::NhgWarmRestartAssist(RedisPipeline *pipeline, NHGMgr *nhgMgr)
    : AppRestartAssist(pipeline, "fpmsyncd-nhg", "bgp"),
      m_nhgMgr(nhgMgr)
{
    if (nhgMgr == nullptr) {
        throw std::invalid_argument("NhgWarmRestartAssist: nhgMgr must not be null");
    }
}

bool NhgWarmRestartAssist::inProgress() const
{
    return m_state > NHG_WR_NONE && m_state < NHG_WR_RECONCILED;
}

void NhgWarmRestartAssist::initWarmStart()
{
    m_state = NHG_WR_INITIALIZED;
    m_saved_nhg_infos.clear();
    m_appdb_nhg_fvs.clear();
    m_reconciled_ids.clear();
    SWSS_LOG_NOTICE("NHG warm restart: initialized");
}

/*
 * Save NHG Manager state to APPL_STATE_DB for warm restart recovery.
 *
 * Iterates all RIB NHG entries that have Sonic NHG objects or PIC objects
 * and persists their essential fields (sonic_nhg_id, af, pic_context_id,
 * JSON representation) into the given state table. Also stores the ID
 * allocator counters so that post-restart allocation resumes without
 * collisions.
 */
void NhgWarmRestartAssist::saveState(swss::Table &stateTable)
{
    SWSS_LOG_NOTICE("NHG warm restart: saving state");

    const auto &nhgMap = m_nhgMgr->getRIBNHGTable()->getNhgMap();
    int count = 0;

    for (const auto &kv : nhgMap) {
        const ribID &ribId = kv.first;
        RIBNHGEntry *entry = kv.second;

        if (entry == nullptr) {
            continue;
        }

        /* Only persist entries that have Sonic NHG objects or PIC objects */
        if (!entry->needCreateSonicObject() && !entry->hasSonicPICObj()) {
            continue;
        }

        std::vector<FieldValueTuple> fvs;

        fvs.emplace_back("sonic_nhg_id", std::to_string(entry->getSonicObjIDNum()));
        fvs.emplace_back("af", std::to_string(entry->getAddressFamily()));

        /* Store the full NHG JSON for restore and debugging */
        NextHopGroupFull nhg = entry->getNHG();
        nlohmann::ordered_json j = nhg;
        fvs.emplace_back("json", j.dump());

        if (entry->hasSonicPICObj()) {
            fvs.emplace_back("pic_context_id", std::to_string(entry->getSonicPICObjIDNum()));
        }

        stateTable.set(std::to_string(ribId.id), fvs);
        count++;
    }

    /* Store ID allocator state so post-restart allocation resumes correctly */
    std::vector<FieldValueTuple> allocFvs;
    allocFvs.emplace_back("next_nhg_id", std::to_string(m_nhgMgr->getSonicIDMgr().getNextNhgID()));
    allocFvs.emplace_back("next_pic_id", std::to_string(m_nhgMgr->getSonicIDMgr().getNextPicID()));
    stateTable.set("NHG_ID_ALLOCATOR", allocFvs);

    SWSS_LOG_NOTICE("NHG warm restart: saved %d NHG entries", count);
}

/*
 * Load NHG Manager state from APPL_STATE_DB and APP_DB for warm restart recovery.
 *
 * Reads saved sonic ID mappings and allocator counters from the state table
 * (NHG_FULL_STATE_TABLE in APPL_STATE_DB), reads existing NHG FV vectors from
 * the APP_DB NEXTHOP_GROUP_TABLE, restores the ID allocators, marks all saved
 * IDs as used, and transitions the FSM to RESTORED.
 *
 * @param stateTable      NHG_FULL_STATE_TABLE in APPL_STATE_DB (saved state)
 * @param appDbNhgTable   NEXTHOP_GROUP_TABLE in APP_DB (existing NHG entries)
 */
void NhgWarmRestartAssist::loadState(swss::Table &stateTable, swss::Table &appDbNhgTable) {
    SWSS_LOG_NOTICE("NHG warm restart: loading saved state");

    /* ---- Phase 1: Restore ID allocator state from state table ---- */
    {
        string nextNhgIdStr, nextPicIdStr;
        if (stateTable.hget("NHG_ID_ALLOCATOR", "next_nhg_id", nextNhgIdStr)) {
            try {
                uint32_t nextNhgId = static_cast<uint32_t>(stoul(nextNhgIdStr));
                m_nhgMgr->getSonicIDMgr().setNextNhgID(nextNhgId);
                SWSS_LOG_NOTICE("NHG warm restart: restored NHG allocator next_id=%u", nextNhgId);
            } catch (const std::exception &e) {
                SWSS_LOG_WARN("NHG warm restart: failed to parse next_nhg_id '%s': %s, using default",
                              nextNhgIdStr.c_str(), e.what());
            }
        } else {
            SWSS_LOG_WARN("NHG warm restart: NHG_ID_ALLOCATOR next_nhg_id not found in state table");
        }

        if (stateTable.hget("NHG_ID_ALLOCATOR", "next_pic_id", nextPicIdStr)) {
            try {
                uint32_t nextPicId = static_cast<uint32_t>(stoul(nextPicIdStr));
                m_nhgMgr->getSonicIDMgr().setNextPicID(nextPicId);
                SWSS_LOG_NOTICE("NHG warm restart: restored PIC allocator next_id=%u", nextPicId);
            } catch (const std::exception &e) {
                SWSS_LOG_WARN("NHG warm restart: failed to parse next_pic_id '%s': %s, using default",
                              nextPicIdStr.c_str(), e.what());
            }
        } else {
            SWSS_LOG_WARN("NHG warm restart: NHG_ID_ALLOCATOR next_pic_id not found in state table");
        }
    }

    /* ---- Phase 2: Load saved NHG infos from state table ---- */
    {
        vector<string> stateKeys;
        stateTable.getKeys(stateKeys);

        int savedCount = 0;
        for (const auto &key : stateKeys) {
            /* Skip the allocator meta-key */
            if (key == "NHG_ID_ALLOCATOR") {
                continue;
            }

            string sonicIdStr, afStr, picIdStr;
            if (!stateTable.hget(key, "sonic_nhg_id", sonicIdStr)) {
                SWSS_LOG_WARN("NHG warm restart: state key %s missing sonic_nhg_id, skipping", key.c_str());
                continue;
            }

            /* Skip entries with N/A sonic_nhg_id (failed entries) */
            if (sonicIdStr == "N/A") {
                SWSS_LOG_DEBUG("NHG warm restart: state key %s has N/A sonic_nhg_id, skipping", key.c_str());
                continue;
            }

            uint32_t sonicIdNum = 0;
            try {
                sonicIdNum = static_cast<uint32_t>(stoul(sonicIdStr));
            } catch (const std::exception &e) {
                SWSS_LOG_WARN("NHG warm restart: failed to parse sonic_nhg_id '%s' for key %s: %s, skipping",
                              sonicIdStr.c_str(), key.c_str(), e.what());
                continue;
            }
            if (sonicIdNum == 0) {
                SWSS_LOG_DEBUG("NHG warm restart: state key %s has zero sonic_nhg_id, skipping", key.c_str());
                continue;
            }

            SavedNHGInfo info;
            info.sonicId = sonicObjectID(sonicIdNum);

            /* Address family */
            if (stateTable.hget(key, "af", afStr)) {
                try {
                    info.af = static_cast<uint8_t>(stoul(afStr));
                } catch (const std::exception &e) {
                    SWSS_LOG_WARN("NHG warm restart: failed to parse af '%s' for key %s: %s, defaulting to 0",
                                  afStr.c_str(), key.c_str(), e.what());
                    info.af = 0;
                }
            } else {
                info.af = 0;
            }

            /* PIC context ID (optional) */
            if (stateTable.hget(key, "pic_context_id", picIdStr) && picIdStr != "N/A") {
                try {
                    uint32_t picIdNum = static_cast<uint32_t>(stoul(picIdStr));
                    info.picObjId = sonicObjectID(picIdNum);
                } catch (const std::exception &e) {
                    SWSS_LOG_WARN("NHG warm restart: failed to parse pic_context_id '%s' for key %s: %s, defaulting to 0",
                                  picIdStr.c_str(), key.c_str(), e.what());
                    info.picObjId = sonicObjectID(0);
                }
            } else {
                info.picObjId = sonicObjectID(0);
            }

            m_saved_nhg_infos[info.sonicId] = info;

            /* Mark this sonic NHG ID as used so new allocations don't collide */
            m_nhgMgr->getSonicIDMgr().markNhgIDUsed(info.sonicId);

            /* Mark PIC ID as used if present */
            if (info.picObjId.id != 0) {
                m_nhgMgr->getSonicIDMgr().markPicIDUsed(info.picObjId);
            }

            savedCount++;
            SWSS_LOG_DEBUG("NHG warm restart: loaded state key %s, sonicId=%u, af=%u, picId=%u",
                           key.c_str(), info.sonicId.id, info.af, info.picObjId.id);
        }

        SWSS_LOG_NOTICE("NHG warm restart: loaded %d saved NHG infos from state table", savedCount);
    }

    /* ---- Phase 3: Load existing NHG FV vectors from APP_DB ---- */
    {
        vector<string> appDbKeys;
        appDbNhgTable.getKeys(appDbKeys);

        int appDbCount = 0;
        for (const auto &key : appDbKeys) {
            vector<FieldValueTuple> fvs;
            if (!appDbNhgTable.get(key, fvs)) {
                SWSS_LOG_WARN("NHG warm restart: failed to read APP_DB key %s", key.c_str());
                continue;
            }

            /* Parse sonic ID from the APP_DB key string */
            uint32_t sonicIdNum = 0;
            try {
                sonicIdNum = static_cast<uint32_t>(stoul(key));
            } catch (const std::exception &e) {
                SWSS_LOG_WARN("NHG warm restart: cannot parse APP_DB key '%s' as sonicId: %s",
                              key.c_str(), e.what());
                continue;
            }

            AppDbNHGEntry appEntry;
            appEntry.sonicId = sonicObjectID(sonicIdNum);
            appEntry.fvVector = fvs;
            appEntry.matched = false;

            m_appdb_nhg_fvs[key] = appEntry;
            appDbCount++;
            SWSS_LOG_DEBUG("NHG warm restart: loaded APP_DB NHG key %s with %zu fields",
                           key.c_str(), fvs.size());
        }

        SWSS_LOG_NOTICE("NHG warm restart: loaded %d NHG entries from APP_DB", appDbCount);
    }

    /* ---- Phase 4: Transition FSM to RESTORED ---- */
    m_state = NHG_WR_RESTORED;
    SWSS_LOG_NOTICE("NHG warm restart: state restored (saved_nhgs=%zu, appdb_nhgs=%zu)",
                    m_saved_nhg_infos.size(), m_appdb_nhg_fvs.size());
}

bool NhgWarmRestartAssist::shouldIntercept(const struct nlmsghdr *nlh) const
{
    if (!inProgress()) {
        return false;
    }
    uint16_t type = nlh->nlmsg_type;
    return type == RTM_NEWNHGFIB || type == RTM_DELNHGFIB ||
           type == RTM_NEWROUTE || type == RTM_DELROUTE ||
           type == RTM_NEWSRV6VPNROUTE || type == RTM_DELSRV6VPNROUTE;
}

void NhgWarmRestartAssist::bufferNHG(const struct nlmsghdr *nlh)
{
    if (!inProgress()) {
        return;
    }
    m_nhg_raw_buffer.emplace_back((const uint8_t *)nlh,
                                  (const uint8_t *)nlh + nlh->nlmsg_len);
}

void NhgWarmRestartAssist::bufferRoute(const struct nlmsghdr *nlh)
{
    if (!inProgress()) {
        return;
    }
    m_route_raw_buffer.emplace_back((const uint8_t *)nlh,
                                    (const uint8_t *)nlh + nlh->nlmsg_len);
}

void NhgWarmRestartAssist::reconcileNHGs()
{
    reconcileNormalSingleHopNHGs();
    reconcileNHGsWithSonicObj();
    m_nhg_raw_buffer.clear();
}

void NhgWarmRestartAssist::replayBufferedRoutes(RouteSync &routeSync)
{
    SWSS_LOG_NOTICE("NHG warm restart: replaying %zu buffered route messages",
                    m_route_raw_buffer.size());

    for (auto &rawMsg : m_route_raw_buffer)
    {
        struct nlmsghdr *nlh = reinterpret_cast<struct nlmsghdr *>(rawMsg.data());
        routeSync.dispatchBufferedRouteMsg(nlh);
    }
    m_route_raw_buffer.clear();
    SWSS_LOG_NOTICE("NHG warm restart: route replay complete");
}

/*
 * Warm restart Phase 1: Reconcile normal single-hop NHGs.
 *
 * These are NHGs that have no group members and no SRv6 info -- they don't
 * create Sonic NHG objects in APP_DB, so they can be added directly without
 * any FV matching. This phase must run before Phase 2 because multi-hop NHGs
 * require their member NHGs to already exist in the RIB table.
 */
void NhgWarmRestartAssist::reconcileNormalSingleHopNHGs()
{
    SWSS_LOG_NOTICE("NHG warm restart Phase 1: reconciling normal single-hop NHGs");

    for (auto &rawMsg : m_nhg_raw_buffer) {
        fib::NextHopGroupFull nhg;
        uint8_t af;
        uint16_t msgType;

        int parseRet = parseNHGFromRawMsg(rawMsg, nhg, af, msgType);
        if (parseRet != 0) {
            /* -2 = filtered interface (eth0/docker0), -1 = parse error; both skip */
            continue;
        }

        /* Skip delete messages */
        if (msgType == RTM_DELNHGFIB) {
            continue;
        }

        /* Normal single-hop: no group list, no SRv6 info */
        bool hasGroup = !nhg.nh_grp_full_list.empty();
        bool hasSrv6 = (nhg.nh_srv6 != nullptr);

        if (!hasGroup && !hasSrv6) {
            m_nhgMgr->addNHGFull(nhg, af);
            m_reconciled_ids.insert(ribID(nhg.id));
        }
    }

    SWSS_LOG_NOTICE("NHG warm restart Phase 1: %zu normal single-hop NHGs reconciled",
                     m_reconciled_ids.size());
}

/*
 * Warm restart Phase 2: Reconcile NHGs that have Sonic objects (APP_DB entries).
 *
 * This covers multi-hop NHGs, SRv6 NHGs, and any NHG that creates entries in
 * NEXTHOP_GROUP_TABLE or PIC_CONTEXT_TABLE. The algorithm:
 *
 *   A) Parse remaining (not yet reconciled) NHGs from the raw buffer into
 *      tempMap with just the NHG data (no setEntry call yet, since multi-level
 *      NHGs may reference other Phase 2 NHGs that don't exist in the table).
 *   B) Topologically sort entries based on nh_grp_full_list dependencies
 *      so that leaf NHGs are processed before parents.
 *   C) Process in topo order: for each entry, call setEntry (dependencies
 *      exist because we add in order), compute FV, match against APP_DB
 *      entries, look up saved PIC IDs, and add via addNHGFullWithSonicId
 *      or addNHGFull.
 *   D) Clean up unmatched old APP_DB entries (stale from before restart).
 */
void NhgWarmRestartAssist::reconcileNHGsWithSonicObj()
{
    SWSS_LOG_NOTICE("NHG warm restart Phase 2: reconciling NHGs with Sonic objects");
    m_state = NHG_WR_RECONCILING;

    std::map<ribID, TempReconcileEntry> tempMap;

    /*
     * Step A: Parse remaining NHGs from raw buffer into tempMap.
     * Only store the parsed NHG data -- do NOT call setEntry() yet because
     * multi-level NHGs reference other Phase 2 NHGs that don't exist in the
     * RIB table yet. setEntry() validates group members via isNHGExist()
     * and would fail for such dependencies.
     */
    for (auto &rawMsg : m_nhg_raw_buffer) {
        fib::NextHopGroupFull nhg;
        uint8_t af;
        uint16_t msgType;

        int parseRet = parseNHGFromRawMsg(rawMsg, nhg, af, msgType);
        if (parseRet != 0) {
            /* -2 = filtered interface (eth0/docker0), -1 = parse error; both skip */
            continue;
        }

        /* Skip delete messages */
        if (msgType == RTM_DELNHGFIB) {
            continue;
        }

        /* Skip already reconciled in Phase 1 */
        if (m_reconciled_ids.count(ribID(nhg.id))) {
            continue;
        }

        TempReconcileEntry te;
        te.nhg = nhg;
        te.af = af;
        te.needsSonicObj = false;
        te.reuseSonicId = sonicObjectID(0);
        te.reusePicObjId = sonicObjectID(0);

        tempMap[ribID(nhg.id)] = te;
    }

    /*
     * Step B: Topological sort based on nh_grp_full_list dependencies.
     * This ensures leaf NHGs (single-hop) are processed before parent NHGs
     * (multi-hop) that reference them as group members.
     */
    auto sorted = topologicalSort(tempMap);

    /*
     * Step C: Process entries in topological order.
     * For each entry: call setEntry() (dependencies now exist because we add
     * in order), compute FV, match against APP_DB, and add via
     * addNHGFullWithSonicId or addNHGFull.
     */
    int matchCount = 0;
    for (auto ribId : sorted) {
        auto it = tempMap.find(ribId);
        if (it == tempMap.end()) {
            continue;
        }
        TempReconcileEntry &entry = it->second;

        /*
         * Dry-run setEntry to compute FV vector. At this point all
         * dependency NHGs have already been added to the RIB table
         * (processed earlier in topo order), so group member validation
         * will succeed even for multi-level hierarchies.
         */
        RIBNHGEntry tmpEntry(m_nhgMgr->getRIBNHGTable());
        int ret = tmpEntry.setEntry(entry.nhg, entry.af);
        if (ret != 0) {
            SWSS_LOG_WARN("NHG warm restart: setEntry failed for rib_id %u, skipping", entry.nhg.id);
            continue;
        }

        entry.fvVector = tmpEntry.getFvVector();
        entry.needsSonicObj = tmpEntry.needCreateSonicObject() || tmpEntry.hasSonicPICObj();

        /* Match FV vector against APP_DB entries */
        if (entry.needsSonicObj) {
            std::string fvHash = computeFvHash(entry.fvVector);
            AppDbNHGEntry *match = findMatchingAppDbEntry(fvHash);

            if (match) {
                entry.reuseSonicId = match->sonicId;
                /* Look up saved PIC ID for this sonic ID */
                auto savedIt = m_saved_nhg_infos.find(match->sonicId);
                if (savedIt != m_saved_nhg_infos.end()) {
                    entry.reusePicObjId = savedIt->second.picObjId;
                }
                match->matched = true;
                matchCount++;
            }
        }

        /* Add the entry, reusing matched Sonic ID if available */
        if (entry.reuseSonicId.id != 0) {
            m_nhgMgr->addNHGFullWithSonicId(entry.nhg, entry.af, entry.reuseSonicId, entry.reusePicObjId);
        } else {
            m_nhgMgr->addNHGFull(entry.nhg, entry.af);
        }
        m_reconciled_ids.insert(ribId);
    }

    SWSS_LOG_NOTICE("NHG warm restart Phase 2: matched %d NHGs to existing APP_DB entries",
                     matchCount);

    /* Step D: Clean up unmatched old APP_DB entries (stale from before restart) */
    int cleanCount = 0;
    for (auto &kv : m_appdb_nhg_fvs) {
        if (!kv.second.matched) {
            m_nhgMgr->getRIBNHGTable()->removeFromDB(kv.second.sonicId);
            cleanCount++;
        }
    }

    /* Clear warm restart temporary data structures */
    m_appdb_nhg_fvs.clear();
    m_saved_nhg_infos.clear();
    m_reconciled_ids.clear();
    m_state = NHG_WR_RECONCILED;

    /* Report completion to STATE_DB so the warm-restart finalizer does not wait on fpmsyncd-nhg */
    WarmStart::setWarmStartState("fpmsyncd-nhg", WarmStart::RECONCILED);

    SWSS_LOG_NOTICE("NHG warm restart Phase 2: complete. Cleaned %d stale entries", cleanCount);
}

/*
 * Find an unmatched APP_DB NHG entry whose FV vector hash matches the given hash.
 * Used during reconciliation to identify pre-existing APP_DB entries that can be
 * reused (avoid unnecessary delete+recreate).
 *
 * @param fvHash  Canonical hash string computed by computeFvHash()
 * @return Pointer to matching entry, or nullptr if no match found
 */
NhgWarmRestartAssist::AppDbNHGEntry *NhgWarmRestartAssist::findMatchingAppDbEntry(const std::string &fvHash)
{
    for (auto &kv : m_appdb_nhg_fvs) {
        if (!kv.second.matched && computeFvHash(kv.second.fvVector) == fvHash) {
            return &kv.second;
        }
    }
    return nullptr;
}

/*
 * Topological sort of NHG entries by their group member dependencies.
 * Ensures that single-hop NHGs (leaves) are processed before multi-hop
 * NHGs (parents) that reference them as group members.
 *
 * Returns the sorted order as a vector of ribIDs. If cycles are detected,
 * the skipped entries are logged as a warning.
 */
std::vector<ribID> NhgWarmRestartAssist::topologicalSort(
        const std::map<ribID, TempReconcileEntry> &entries)
{
    std::map<uint32_t, int> inDegree;
    std::map<uint32_t, std::set<uint32_t>> adj;

    /* Initialize in-degree for all entries */
    for (const auto &kv : entries) {
        inDegree[kv.first.id] = 0;
    }

    /* Build dependency graph: if entry X has group member Y, then Y -> X */
    for (const auto &kv : entries) {
        for (const auto &grp : kv.second.nhg.nh_grp_full_list) {
            if (entries.count(swss::ribID(grp.id))) {
                adj[grp.id].insert(kv.first.id);
                inDegree[kv.first.id]++;
            }
        }
    }

    /* BFS-based topological sort (Kahn's algorithm) */
    std::queue<uint32_t> q;
    for (const auto &kv : inDegree) {
        if (kv.second == 0) {
            q.push(kv.first);
        }
    }

    std::vector<swss::ribID> result;
    while (!q.empty()) {
        uint32_t cur = q.front();
        q.pop();
        result.push_back(swss::ribID(cur));
        for (uint32_t next : adj[cur]) {
            if (--inDegree[next] == 0) {
                q.push(next);
            }
        }
    }

    if (result.size() < entries.size()) {
        SWSS_LOG_WARN("NHG warm restart: detected cyclic dependencies, %zu of %zu entries skipped",
                       entries.size() - result.size(), entries.size());
    }

    return result;
}
