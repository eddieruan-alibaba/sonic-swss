#include "nhgmgr.h"
#include "logger.h"
#include <string.h>

/* zebra nexthop flag bits (see FRR lib/nexthop.h). Only ONLINK is exported
 * by the shipped nexthopgroupfull.h, so define ACTIVE here if not present. */
#ifndef NEXTHOP_FLAG_ACTIVE
#define NEXTHOP_FLAG_ACTIVE (1 << 0) /* This nexthop is alive. */
#endif


using namespace std;
using namespace swss;

namespace {
/*
 * Thread-safe SWSS logger callback function.
 * All sonic_fib log messages will be forwarded to SWSS logger.
 */
// ADD THIS ATTRIBUTE BEFORE THE FUNCTION DEFINITION:
__attribute__((format(printf, 5, 0)))
void swssLogBridge(fib::LogLevel level, const char* file, int line,
                   const char* func, const char* format, va_list args) {
    // Map fib levels → SWSS levels
    swss::Logger::Priority swssLevel;
    switch (level) {
        case fib::LogLevel::DEBUG: swssLevel = swss::Logger::SWSS_DEBUG; break;
        case fib::LogLevel::INFO:  swssLevel = swss::Logger::SWSS_NOTICE; break;
        case fib::LogLevel::WARN:  swssLevel = swss::Logger::SWSS_WARN; break;
        case fib::LogLevel::ERROR: swssLevel = swss::Logger::SWSS_ERROR; break;
        default: swssLevel = swss::Logger::SWSS_WARN;
    }

    // Format message into buffer, max 2KB (adjust as needed)
    constexpr size_t BUFFER_SIZE = 2048;
    std::array<char, BUFFER_SIZE> buffer;

    va_list args_copy;
    va_copy(args_copy, args);
    int len = vsnprintf(buffer.data(), buffer.size(), format, args_copy);
    va_end(args_copy);

    if (len < 0) {
        // Formatting failed – log error with literal format
        swss::Logger::getInstance().write(
            swss::Logger::SWSS_ERROR,
            "Log formatting failed (vsnprintf error)"
        );
        return;
    }

    if (static_cast<size_t>(len) >= buffer.size()) {
        // Truncate gracefully with "..."
        constexpr size_t trunc_len = BUFFER_SIZE - 4;
        buffer[trunc_len] = '.';
        buffer[trunc_len + 1] = '.';
        buffer[trunc_len + 2] = '.';
        buffer[trunc_len + 3] = '\0';
    }

    // Log with LITERAL format string "%s" → satisfies -Wformat-nonliteral
    swss::Logger::getInstance().write(swssLevel, "%s", buffer.data());
}

} // anonymous namespace

// Registration helper
void registerSwssLogger() {
    fib::registerLogCallback(swssLogBridge);
    fib::setLogLevel(fib::LogLevel::INFO); // INFO for production as default
    SWSS_LOG_NOTICE("FIB logging initialized, log level set to %d",
                    static_cast<int>(fib::getLogLevel()));
}
/*
 * Function used to compare two NextHopGroupFull is equal or not
 * Not include nh_grp_full_list and srv6 fields
 */
static bool compareDependsAndDependents(const NextHopGroupFull *newNHG, const NextHopGroupFull *oldNHG) {
    set<int> newDepends(newNHG->depends.begin(), newNHG->depends.end());
    set<int> oldDepends(oldNHG->depends.begin(), oldNHG->depends.end());
    if (newDepends != oldDepends) {
        return false;
    }
    set<int> newDependents(newNHG->dependents.begin(), newNHG->dependents.end());
    set<int> oldDependents(oldNHG->dependents.begin(), oldNHG->dependents.end());
    return newDependents == oldDependents;
}

/*
 * Function used to compare nh_grp_full_list in NextHopGroupFull is equal or not
 */
static bool compareNHGFullList(const NextHopGroupFull *newNHG, const NextHopGroupFull *oldNHG) {
    if ((newNHG->nh_grp_full_list.size()) != (oldNHG->nh_grp_full_list.size())) {
        return false;
    }
    for (size_t i = 0; i < newNHG->nh_grp_full_list.size(); i++){
        if (newNHG->nh_grp_full_list[i].id != oldNHG->nh_grp_full_list[i].id){
            return false;
        }
        if (newNHG->nh_grp_full_list[i].weight != oldNHG->nh_grp_full_list[i].weight){
            return false;
        }
        if (newNHG->nh_grp_full_list[i].num_direct != oldNHG->nh_grp_full_list[i].num_direct){
            return false;
        }
    }

    return true;
}

/*
 * Function used to compare nh_srv6 in NextHopGroupFull is equal or not
 */
static bool compareNHGSRv6Fields(const NextHopGroupFull *new_nhg, const NextHopGroupFull *oldNHG) {
    if (new_nhg->nh_srv6 == nullptr && oldNHG->nh_srv6 == nullptr) {
        return true;
    }
    if (new_nhg->nh_srv6 == nullptr || oldNHG->nh_srv6 == nullptr){
        return false;
    }

    // Compare seg6_segs (vpn_sid)
    bool new_has_segs = (new_nhg->nh_srv6->seg6_segs != nullptr);
    bool old_has_segs = (oldNHG->nh_srv6->seg6_segs != nullptr);
    if (new_has_segs != old_has_segs) {
        return false;
    }
    if (new_has_segs && old_has_segs) {
        if (new_nhg->nh_srv6->seg6_segs->num_segs != oldNHG->nh_srv6->seg6_segs->num_segs) {
            return false;
        }
        for (uint8_t i = 0; i < new_nhg->nh_srv6->seg6_segs->num_segs; i++) {
            if (memcmp(&new_nhg->nh_srv6->seg6_segs->seg[i],
                       &oldNHG->nh_srv6->seg6_segs->seg[i],
                       sizeof(struct in6_addr)) != 0) {
                return false;
            }
        }
    }

    return true;
}

NHGMgr::NHGMgr(RedisPipeline *pipeline, const std::string &nexthopTableName, const std::string &picTableName, bool isStateTable) {
    m_rib_nhg_table = new RIBNHGTable(pipeline, nexthopTableName, isStateTable);
    m_sonic_pic_table = new SonicPICContentTable(pipeline, picTableName, isStateTable);
    m_sonic_id_manager.init({SONIC_NHG_OBJ_TYPE_NHG_WITH_SRV6_PIC, SONIC_NHG_OBJ_TYPE_NHG_NORMAL});
    m_rib_nhg_table->setSonicIDManager(&m_sonic_id_manager);

    // register SWSS logger as the callback for sonic_fib logs
    registerSwssLogger();
}

/*
 * NHG add entrance for FIB Block
 */
int NHGMgr::addNHGFull(NextHopGroupFull nhg, uint8_t af) {

    SWSS_LOG_NOTICE("Receiving NHG %d, type %d, af %d", nhg.id, nhg.type, af);
    dumpNHGGroupFull(nhg);

    int ret = 0;
    if (m_rib_nhg_table->isNHGExist(nhg.id)) {
        SWSS_LOG_DEBUG("NHG %d already exists, need to update", nhg.id);
        ret = updateExistingNHGFull(nhg, af);
        if (ret != 0){
            SWSS_LOG_ERROR("Failed to update NHG %d", nhg.id);
            return ret;
        }
    } else {
        SWSS_LOG_DEBUG("NHG %d not found, create new entry", nhg.id);
        ret = addNewNHGFull(nhg, af);
        if (ret != 0){
            SWSS_LOG_ERROR("Failed to add NHG %d", nhg.id);
            return ret;
        }
    }
    return 0;
}

/*
 * process of add new NHG full
 */
int NHGMgr::addNewNHGFull(NextHopGroupFull nhg, uint8_t af) {

    int ret = 0;
    RIBNHGEntry *entry;
    ret = m_rib_nhg_table->addEntry(nhg, af);
    if (ret != 0) {
        SWSS_LOG_ERROR("Failed to add NHG %d", nhg.id);
        return ret;
    }

    entry = m_rib_nhg_table->getEntry(nhg.id);
    if (entry == nullptr) {
        SWSS_LOG_ERROR("Failed to get NHG entry after add for %d", nhg.id);
        return -1;
    }

    /* Populate nexthop-to-NHG index maps for PIC backwalk */
    const string& gw = entry->getGatewayAddress();
    if (!gw.empty()) {
        if (!entry->hasSonicGatewayObj()) {
            /* Global table context (Part 1 fallback) */
            m_rib_nhg_table->addGlobalEntry(gw, entry);
        } else {
            /* VRF/VPN context (Part 2) */
            m_rib_nhg_table->addVrfEntry(gw, entry);
        }
    }

    /*
     * Process NHG offload
     */
    if (entry->needCreateSonicObject()) {

        // no sonic nhg object created, allocate a new id
        uint32_t sonicId = m_sonic_id_manager.allocateID(SONIC_NHG_OBJ_TYPE_NHG_NORMAL);
        if (sonicId == 0) {
            SWSS_LOG_ERROR("Failed to allocate sonic nhg id");
            m_rib_nhg_table->delEntry(nhg.id);
            return -1;
        }

        entry->setSonicNHGObjId(sonicId);
        ret = m_rib_nhg_table->writeToDB(entry);
        if (ret != 0) {
            m_rib_nhg_table->delEntry(nhg.id);
            m_sonic_id_manager.freeID(SONIC_NHG_OBJ_TYPE_NHG_NORMAL, sonicId);
            SWSS_LOG_ERROR("Failed to write to DB for %d", nhg.id);
            return ret;
        }

        // store the shared sonic nhg object
        if (entry->isSharedSonicNHG()){
            SonicNHGObjectKey key;
            SonicNHGObjectKey::createSonicNormalNHGObjectKey(entry, key);
            m_rib_nhg_table->insertCreatedSharedNHGObject(key, sonicId);
            SWSS_LOG_DEBUG("Create shared sonic NHG for %d, sonic id %d", nhg.id, sonicId);
        }else{
            SWSS_LOG_DEBUG("Create sonic NHG for %d, sonic id %d", nhg.id, sonicId);
        }
    }

    /*
     * Process the PICContent offload
     */
    if (entry->hasSonicPICObj()) {
        SWSS_LOG_DEBUG("Create sonic PICContent object for NHG %d", nhg.id);
        ret = createSonicPICObject(entry);
        if (ret != 0) {
            m_rib_nhg_table->removeFromDB(entry->getSonicObjID());
            // sub the ref for the shared sonic nhg object
            m_rib_nhg_table->subSonicNHGObjectRef(entry->getSonicNHGObjectKey());
            m_rib_nhg_table->delEntry(nhg.id);
            SWSS_LOG_ERROR("Failed to create sonic nhg %d", nhg.id);
            return ret;
        }
    }
    return 0;
}

/*
 * process of update NHG full
 */
int NHGMgr::updateExistingNHGFull(NextHopGroupFull nhg, uint8_t af) {
    /*
     * Not support update of NHG fields:
     * - nh_flags
     * - nh_srv6
     */

    RIBNHGEntry *entry;
    int ret = 0;
    entry = m_rib_nhg_table->getEntry(nhg.id);
    if (entry == nullptr) {
        SWSS_LOG_ERROR("Failed to get NHG %d when updateNHGFull", nhg.id);
        return -1;
    }

    SonicNHGObjectKey previousKey = entry->getSonicNHGObjectKey();

    bool updated = false;

    // check if NHG fields updated and update the rib entry
    ret = m_rib_nhg_table->updateEntry(nhg, af, updated);
    if (ret != 0) {
        SWSS_LOG_ERROR("Failed to update NHG %d", nhg.id);
        return ret;
    }

    if (!updated) {
        SWSS_LOG_NOTICE("NHG %d fields not updated", nhg.id);
        return 0;
    }

    // entry fields changed, update into the table
    entry = m_rib_nhg_table->getEntry(nhg.id);
    if (entry == nullptr) {
        SWSS_LOG_ERROR("Failed to get NHG after update %d", nhg.id);
        return -1;
    }

    // sonic NHG fields changed, update into DB
    if(previousKey != entry->getSonicNHGObjectKey() && entry->needCreateSonicObject()){

        uint32_t sonicId = 0;
        if (!entry->isSharedSonicNHG()) {
            // For the non shared NHG, we reuse the previous sonic id. If no previous sonic NHG object, allocate a new id
            sonicId = entry->getSonicObjID();
            if (sonicId == 0){
                sonicId = m_sonic_id_manager.allocateID(SONIC_NHG_OBJ_TYPE_NHG_NORMAL);
            }
        }else{
            // For the shared NHG, no NHG obj created in the shared table, sub the previous ref first, then allocate the id for the new object.
            m_rib_nhg_table->subSonicNHGObjectRef(previousKey);
            sonicId = m_sonic_id_manager.allocateID(SONIC_NHG_OBJ_TYPE_NHG_NORMAL);
        }

        if (sonicId == 0) {
            SWSS_LOG_ERROR("Failed to allocate sonic nhg id");
            return -1;
        }
        entry->setSonicNHGObjId(sonicId);
        ret = m_rib_nhg_table->writeToDB(entry);
        if (ret != 0) {
            m_rib_nhg_table->delEntry(nhg.id);
            // This sonic nhg id is reused or new allocated, so free it
            m_sonic_id_manager.freeID(SONIC_NHG_OBJ_TYPE_NHG_NORMAL, sonicId);
            SWSS_LOG_ERROR("Failed to write to DB for %d", nhg.id);
            return ret;
        }

        if (entry->isSharedSonicNHG()){
            // shared NHG track by sonic nhg key, which presents the Sonic NHG object fields, map the key to sonic nhg id
            SonicNHGObjectKey key;
            SonicNHGObjectKey::createSonicNormalNHGObjectKey(entry, key);
            m_rib_nhg_table->insertCreatedSharedNHGObject(key, sonicId);
        }else{
            // non shared NHG track by sonic nhg id, map the sonic id to rib nhg id
            m_rib_nhg_table->insertCreatedNhgObject(sonicId, nhg.id);
        }

        SWSS_LOG_NOTICE("Create sonic NHG for %d, sonic id %d", nhg.id, sonicId);
    } else if(previousKey != entry->getSonicNHGObjectKey()){

        // For the shared NHG, maybe changed to another existing shared NHG, sub the previous ref
        if (entry->isSharedSonicNHG()){
            m_rib_nhg_table->subSonicNHGObjectRef(previousKey);
        }
    }

    return 0;
}

/*
 * Create the sonic PIC Content object from RIB entry
 */
int NHGMgr::createSonicPICObject(RIBNHGEntry *entry) {

    sonicNhgObjType sType = entry->getSonicObjType();
    if (entry->getSonicObjType() != SONIC_NHG_OBJ_TYPE_NHG_WITH_SRV6_PIC){
        SWSS_LOG_ERROR("Unsupported sonic nhg type %d", sType);
        return -1;
    }

    // srv6 sonic nhg create
    SonicPICContentObject sonicObj;
    uint32_t sonicPICContentID;
    int ret = 0;

    // allocate sonic pic object id
    sonicPICContentID = m_sonic_id_manager.allocateID(sType);
    if (sonicPICContentID == 0) {
        SWSS_LOG_ERROR("Failed to allocate sonic nhg id");
        return -1;
    }

    entry->createSRv6PICObjFromRIBEntry(sonicObj);
    sonicObj.id = sonicPICContentID;

    // add the SonicPICContentEntry
    ret = m_sonic_pic_table->addEntry(sonicObj);
    if (ret != 0) {
        SWSS_LOG_ERROR("Failed to add sonic PIC obj for %d", entry->getRIBID());
        m_sonic_id_manager.freeID(sType, sonicPICContentID);
        return ret;
    }

    SonicPICContentEntry *sonicEntry = m_sonic_pic_table->getEntry(sType, sonicPICContentID);
    if (sonicEntry == nullptr) {
        SWSS_LOG_ERROR("Failed to get sonic PIC entry for %d", entry->getRIBID());
        m_sonic_id_manager.freeID(sType, sonicPICContentID);
        return ret;
    }

    // write to DB
    ret = m_sonic_pic_table->writeToDB(sonicEntry);
    if (ret != 0) {
        SWSS_LOG_ERROR("Failed to write to DB for %d", sonicPICContentID);
        m_sonic_id_manager.freeID(sType, sonicPICContentID);
        m_sonic_pic_table->delEntry(sType, sonicPICContentID);
        return ret;
    }

    // set the sonic PIC Content id of rib entry
    entry->setSonicPICObjId(sonicPICContentID);
    SWSS_LOG_NOTICE("Create sonic PIC Content object for NHG %d, sonic pic id: %d, type: %d", entry->getRIBID(), sonicPICContentID, entry->getSonicObjType());
    return 0;
}

/*
 * NHG remove entrance for FIB Block
 */
int NHGMgr::delNHGFull(uint32_t id) {

    SWSS_LOG_NOTICE("Del NextHop group id %d", id);

    if (!m_rib_nhg_table->isNHGExist(id)) {
        SWSS_LOG_ERROR("NextHop group id %d not found.", id);
        return 0;
    }

    RIBNHGEntry *entry = m_rib_nhg_table->getEntry(id);
    if (entry == nullptr) {
        return 0;
    }

    // del the sonic PIC Content first
    if (entry->hasSonicPICObj()) {
        uint32_t sonicPICContentObjID = entry->getSonicPICObjID();
        SonicPICContentEntry* sonicEntry = m_sonic_pic_table->getEntry(entry->getSonicObjType(), sonicPICContentObjID);
        if (sonicEntry != nullptr){
            m_sonic_pic_table->delEntry(entry->getSonicObjType(), sonicPICContentObjID);
            m_sonic_id_manager.freeID(entry->getSonicObjType(), sonicPICContentObjID);
        }
    }

    /* Remove from nexthop-to-NHG index maps before deletion */
    const string& gw = entry->getGatewayAddress();
    if (!gw.empty()) {
        if (!entry->hasSonicGatewayObj()) {
            m_rib_nhg_table->removeGlobalEntry(gw, entry);
        } else {
            m_rib_nhg_table->removeVrfEntry(gw, entry);
        }
    }

    if (m_rib_nhg_table->delEntry(id) != 0) {
        return -1;
    }
    return 0;
}

void NHGMgr::dumpNHGGroupFull(fib::NextHopGroupFull nhg) {
    SWSS_LOG_DEBUG("NHG ID %d, type %d, ifname %s", nhg.id, nhg.type, nhg.ifname.c_str());

    if (nhg.type == fib::NEXTHOP_TYPE_IPV4 || nhg.type == fib::NEXTHOP_TYPE_IPV4_IFINDEX) {
        char gateway[INET_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET, &nhg.gate.ipv4, gateway, INET_ADDRSTRLEN);
        SWSS_LOG_DEBUG("   gateway %s", gateway);
    }
    if (nhg.type == fib::NEXTHOP_TYPE_IPV6 || nhg.type == fib::NEXTHOP_TYPE_IPV6_IFINDEX) {
        char gateway[INET6_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET6, &nhg.gate.ipv6, gateway, INET6_ADDRSTRLEN);
        SWSS_LOG_DEBUG("   gateway %s", gateway);
    }
    for (auto it = nhg.nh_grp_full_list.begin(); it != nhg.nh_grp_full_list.end(); it++) {
        SWSS_LOG_DEBUG("   group member %d, num_direct %d", it->id, it->num_direct);
    }
    for (auto it = nhg.depends.begin(); it != nhg.depends.end(); it++) {
        SWSS_LOG_DEBUG("   depends member %d", *it);
    }
    for (auto it = nhg.dependents.begin(); it != nhg.dependents.end(); it++) {
        SWSS_LOG_DEBUG("   dependents member %d", *it);
    }
    if (nhg.nh_srv6 != nullptr && nhg.nh_srv6->seg6_segs != nullptr){
        char seg_str[INET6_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET6, &nhg.nh_srv6->seg6_segs->seg[0], seg_str, INET6_ADDRSTRLEN);
        SWSS_LOG_DEBUG("   srv6 seg6_segs %s", seg_str);
        char seg_src[INET6_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET6, &nhg.src, seg_src, INET6_ADDRSTRLEN);
        SWSS_LOG_DEBUG("   srv6 seg_src %s", seg_src);
    }
}


/*
 * get RIB NHG entry by RIB ID
 */
RIBNHGEntry *NHGMgr::getRIBNHGEntryByRIBID(uint32_t id) {
    return m_rib_nhg_table->getEntry(id);
}

/*
 * get Sonic PIC Content entry by Sonic PIC Content ID and type
 */
SonicPICContentEntry *NHGMgr::getSonicPICByID(sonicNhgObjType type, uint32_t id) {
    return m_sonic_pic_table->getEntry(type, id);
}

/*
 * check if the sonic NHG id is in used
 */
bool NHGMgr::isSonicNHGIDInUsed(uint32_t id) {
    return m_sonic_id_manager.isSonicObjIDUsed(SONIC_NHG_OBJ_TYPE_NHG_NORMAL, id);
}

/*
 * get Sonic PIC Content entry by RIB NHG ID
 */
SonicPICContentEntry *NHGMgr::getSonicPICByRIBID(uint32_t id) {
    RIBNHGEntry *ribnhgEntry = getRIBNHGEntryByRIBID(id);
    if (ribnhgEntry == nullptr) {
        return nullptr;
    }
    if (!ribnhgEntry->hasSonicPICObj()) {
        return nullptr;
    }
    return getSonicPICByID(ribnhgEntry->getSonicObjType(), ribnhgEntry->getSonicPICObjID());;
}

/*
 * check if the sonic PIC Content object id is in used
 */
bool NHGMgr::isSonicPICIDInUsed(sonicNhgObjType type, uint32_t id) {
    return m_sonic_id_manager.isSonicObjIDUsed(type, id);
}

/*
 * check if the sonic NHG object id is in used
 */
bool SonicIDMgr::isSonicObjIDUsed(sonicNhgObjType type, uint32_t id) {
    SonicIDAllocator *allocator = getAllocator(type);
    if (allocator != nullptr) {
        return allocator->isInUsed(id);
    }
    return false;
}

RIBNHGTable::RIBNHGTable(RedisPipeline *pipeline, const std::string &tableName, bool isStateTable)
    : m_nexthop_groupTable(pipeline, tableName, isStateTable) {
}

/*
 * get RIB NHG entry by RIB ID
 */
RIBNHGEntry *RIBNHGTable::getEntry(uint32_t id) {
    auto it = m_nhg_map.find(id);
    if (it == m_nhg_map.end()) {
        return nullptr;
    }
    return it->second;
}

/*
 * update RIB NHG entry from NHG full
 */
bool RIBNHGEntry::checkNeedUpdate(NextHopGroupFull newNhg, uint8_t afNew) {

    if (newNhg.weight != m_nhg.weight) {
        return true;
    }
    if (newNhg.vrf_id != m_nhg.vrf_id) {
        return true;
    }
    if (newNhg.ifindex != m_nhg.ifindex) {
        return true;
    }
    if (newNhg.ifname != m_nhg.ifname) {
        return true;
    }

    if (afNew != m_af) {
        return true;
    }

    if (newNhg.type != m_nhg.type){
        return true;
    }

    if (newNhg.type != fib::NEXTHOP_TYPE_IFINDEX && newNhg.type != fib::NEXTHOP_TYPE_BLACKHOLE){
        if(memcmp(&newNhg.gate, &m_nhg.gate, sizeof(fib::g_addr))!=0){
            return true;
        }
    }

    if (newNhg.type == fib::NEXTHOP_TYPE_BLACKHOLE){
        if (newNhg.bh_type != m_nhg.bh_type){
            return true;
        }
    }

    if (!compareDependsAndDependents(&newNhg, &m_nhg)) {
        return true;
    }

    if (!compareNHGFullList(&newNhg, &m_nhg)){
        return true;
    }

    if (!compareNHGSRv6Fields(&newNhg, &m_nhg)){
        return true;
    }

    for (const auto& kv : m_resolved_enable_group) {
        if (!kv.second) {
            return true;
        }
    }

    if (m_updated_via_backwalk) {
        return true;
    }

    return false;
}

/*
 * delete RIB NHG entry by RIB ID
 */
void RIBNHGTable::delEntry(uint32_t id) {
    if (m_nhg_map.find(id) == m_nhg_map.end()) {
        SWSS_LOG_ERROR("NextHop group id %d not found.", id);
        return;
    }

    RIBNHGEntry *entry = m_nhg_map[id];
    uint32_t sonicNHGID = entry->getSonicObjID();

    if (entry->isSharedSonicNHG()){
        this->subSonicNHGObjectRef(entry->getSonicNHGObjectKey());
    }else{
        SWSS_LOG_NOTICE("delEntry: remove entry from NHG DB for RIB_id %d, sonic_id %d",
                         entry->getRIBID(), entry->getSonicObjID());
        this->removeFromDB(sonicNHGID);
        m_sonic_id_manager->freeID(SONIC_NHG_OBJ_TYPE_NHG_NORMAL, sonicNHGID);
        m_sonic_nhg_id_2_rib_nhg_id_map.erase(sonicNHGID);
    }
    delete entry;
    m_nhg_map.erase(id);
    return;
}

/*
 * add RIB NHG entry from NextHopGroupFull
 */
int RIBNHGTable::addEntry(NextHopGroupFull nhg, uint8_t af) {
    if (m_nhg_map.find(nhg.id) != m_nhg_map.end()) {
        SWSS_LOG_ERROR("Failed to create nhg entry for %d", nhg.id);
        return -1;
    }
    RIBNHGEntry *entry = RIBNHGEntry::createNHGEntry(this);
    if (entry == nullptr) {
        SWSS_LOG_ERROR("Failed to create nhg entry for %d", nhg.id);
        return -1;
    }

    int ret = entry->setEntry(nhg, af);
    if (ret != 0) {
        delete entry;
        return -1;
    }

    m_nhg_map.insert(std::make_pair(nhg.id, entry));
    return 0;
}

/*
 * check if NHG fields updated, if updated then update rib entry
 */
int RIBNHGTable::updateEntry(NextHopGroupFull nhg, uint8_t af, bool &updated) {

    auto it = m_nhg_map.find(nhg.id);
    if (it == m_nhg_map.end()) {
        SWSS_LOG_ERROR("NextHop group id %d not exists.", nhg.id);
        return -1;
    }
    RIBNHGEntry *entry = it->second;
    int ret = 0;
    if(entry == nullptr) {
        SWSS_LOG_ERROR("NextHop group id %d not exists.", nhg.id);
        return -1;
    }

    // check NHG fields whether updated
    updated = entry->checkNeedUpdate(nhg, af);

    // update rib entry
    if (updated){
        ret = entry->setEntry(nhg, af);
        if (ret != 0) {
            SWSS_LOG_ERROR("Failed to set entry for %d", nhg.id);
            return ret;
        }

        // A valid NHGFULL from zebra means this NHG is live again. Clear stale
        // PIC disable flags in all dependents that track this NHG so a later
        // backwalk's isAllDisabled() check sees correct state. We do NOT mark
        // the dependent as backwalk-updated: re-enabling its flag does not by
        // itself change its HW NHG. The backwalk flag is reserved for real
        // backwalk-driven divergence.
        std::set<uint32_t> dependents = entry->getDependentsID();
        for (uint32_t dep_id : dependents) {
            RIBNHGEntry* dep_entry = getEntry(dep_id);
            if (dep_entry) {
                auto& dep_eg = dep_entry->getResolvedEnableGroup();
                auto it2 = dep_eg.find(nhg.id);
                if (it2 != dep_eg.end() && !it2->second) {
                    it2->second = true;
                }
            }
        }
    }

    return 0;
}

/*
 * check if NHG entry exists
 */
bool RIBNHGTable::isNHGExist(uint32_t id) {
    auto it = m_nhg_map.find(id);
    if (it != m_nhg_map.end()) {
        return true;
    }
    return false;
}

/*
 * write Sonic NHG Object to APP_DB
 */
int RIBNHGTable::writeToDB(RIBNHGEntry *entry) {

    vector<FieldValueTuple> fvVector = entry->getFvVector();
    if (fvVector.size() == 0) {
        SWSS_LOG_ERROR("Failed to sync fvVector for %d, empty fvVector", entry->getNHG().id);
        return -1;
    }

    /* Compare-and-skip deduplication: avoid redundant APPDB writes */
    string fields_str;
    for (const auto& fv : fvVector) {
        fields_str += fvField(fv) + "=" + fvValue(fv) + ";";
    }
    if (fields_str == entry->getLastAppdbFields()) {
        SWSS_LOG_DEBUG("Skip writeToDB for %d: fields unchanged", entry->getSonicObjID());
        //return 0;
    }
    entry->setLastAppdbFields(fields_str);

    SWSS_LOG_NOTICE("writeToDB : RIB_id %d sonic_id %d, fields %s",
                    entry->getRIBID(), entry->getSonicObjID(), fields_str.c_str());

    m_nexthop_groupTable.set(std::to_string(entry->getSonicObjID()), fvVector);

    // At present, the specification of NHG is not large.
    // Use NOTICE to facilitate debugging during converge.
    SWSS_LOG_NOTICE("writeToDB NEXTHOP_GROUP_TABLE : RIB_id %d sonic_id %d",
                    entry->getRIBID(), entry->getSonicObjID());
    for (auto it = fvVector.begin(); it != fvVector.end(); it++) {
        SWSS_LOG_NOTICE("==============>:   %s %s", fvField(*it).c_str(), fvValue(*it).c_str());
    }

    return 0;
}

/*
 * remove Sonic NHG Object from APP_DB by ID
 */
void RIBNHGTable::removeFromDB(uint32_t id) {
    m_nexthop_groupTable.del(std::to_string(id));
    return;
}

/*
 * clean all RIB NHG entry in table
 */
void RIBNHGTable::cleanUp() {
    for (auto it = m_nhg_map.begin(); it != m_nhg_map.end(); it++) {
        delete it->second;
    }
    m_nhg_map.clear();
    m_created_shared_nhg_map.clear();
    m_sonic_nhg_id_2_rib_nhg_id_map.clear();
}

void RIBNHGTable::insertCreatedSharedNHGObject(SonicNHGObjectKey key, uint32_t id) {
    m_created_shared_nhg_map.insert(std::make_pair(key, SonicNHGObjectInfo(id, 1)));
}

void RIBNHGTable::addSonicNHGObjectRef(SonicNHGObjectKey key) {
    m_created_shared_nhg_map[key].refCount++;
}

void RIBNHGTable::subSonicNHGObjectRef(SonicNHGObjectKey key) {
    if (m_created_shared_nhg_map.find(key) == m_created_shared_nhg_map.end()){
        return ;
    }
    if (m_created_shared_nhg_map[key].refCount == 0) {
        this->removeFromDB(m_created_shared_nhg_map[key].id);
        m_sonic_id_manager->freeID(SONIC_NHG_OBJ_TYPE_NHG_NORMAL, m_created_shared_nhg_map[key].id);
        m_created_shared_nhg_map.erase(key);
        SWSS_LOG_ERROR("Reference count underflow for shared NHG object");
        return;
    }
    (m_created_shared_nhg_map[key].refCount)--;

    /*
     * if refCount is 0, then remove the sonic nhg object
     */
    if(m_created_shared_nhg_map[key].refCount == 0){
         SWSS_LOG_NOTICE("subSonicNHGObjectRef: sonic_id %d",
                         m_created_shared_nhg_map[key].id);
        this->removeFromDB(m_created_shared_nhg_map[key].id);
        m_sonic_id_manager->freeID(SONIC_NHG_OBJ_TYPE_NHG_NORMAL, m_created_shared_nhg_map[key].id);
        m_created_shared_nhg_map.erase(key);
    }
}

int RIBNHGTable::getCreatedSharedNHGObjectID(SonicNHGObjectKey key) {
    if (m_created_shared_nhg_map.find(key) == m_created_shared_nhg_map.end()){
        return 0;
    }
    return m_created_shared_nhg_map[key].id;
}

void RIBNHGTable::insertCreatedNhgObject(uint32_t sonicNhgId, uint32_t ribNhgId) {
    m_sonic_nhg_id_2_rib_nhg_id_map.insert(std::make_pair(sonicNhgId, ribNhgId));
}

RIBNHGEntry::RIBNHGEntry(RIBNHGTable *table) : m_table(table) {
}

RIBNHGEntry::~RIBNHGEntry() {
    m_table = nullptr;
}

/* static creation func */
RIBNHGEntry *RIBNHGEntry::createNHGEntry(RIBNHGTable *mTable) {
    RIBNHGEntry *entry = new RIBNHGEntry(mTable);
    return entry;
}

/*
 * create the corresponding SonicPICContentObject from RIB NHG Entry
 */
int RIBNHGEntry::createSonicPICContentObjectFromRIBEntry(SonicPICContentObject &sonicNhgOut) {
    switch (getSonicObjType()) {
        case SONIC_NHG_OBJ_TYPE_NHG_WITH_SRV6_PIC:
            return createSRv6PICObjFromRIBEntry(sonicNhgOut);
        default:
            SWSS_LOG_ERROR("Unsupported SonicPICContentObject type: %d", getSonicObjType());
            return -1;
    }
}

/*
 * create the corresponding SonicPICContentObject in SRv6 VPN case from RIB NHG Entry
 */
int RIBNHGEntry::createSRv6PICObjFromRIBEntry(SonicPICContentObject &sonicNhgOut) {
    sonicNhgOut.groupMember.clear();
    sonicNhgOut.type = SONIC_NHG_OBJ_TYPE_NHG_WITH_SRV6_PIC;
    sonicNhgOut.id = getSonicPICObjID();

    // single hop: Use SRv6 VPN fields, use them directly.
    if (m_nhg.nh_srv6 != nullptr && m_nhg.nh_srv6->seg6_segs != nullptr) {
        sonicNhgOut.nexthop = m_nexthop;
        sonicNhgOut.segSrc = m_segSrc;
        sonicNhgOut.vpnSid = m_vpnSid;
        sonicNhgOut.ifName = m_ifName;
        return 0;
    }

    // Multi-hop: Use weight from m_group map.
    for (auto member: m_resolvedGroup) {
        RIBNHGEntry *memberEntry = m_table->getEntry(member.first);
        if (memberEntry == nullptr) {
            SWSS_LOG_ERROR("RIBNHGEntry is not exist: %d", member.first);
            return -1;
        }
        if (memberEntry->hasSonicPICObj() && memberEntry->getSonicObjType() == SONIC_NHG_OBJ_TYPE_NHG_WITH_SRV6_PIC) {
            if (memberEntry->getSonicPICObjID() != 0) {
                auto git = m_group.find(member.first);
                uint8_t weight = (git != m_group.end()) ? git->second : 1;
                sonicNhgOut.groupMember.push_back(std::make_pair(memberEntry->getSonicPICObjID(), weight));
            }
        }
    }

    return 0;
}

/* set an RIB Entry from a nexthopgroupfull */
int RIBNHGEntry::setEntry(NextHopGroupFull nhg, uint8_t af) {
    m_rib_id = nhg.id;
    m_nhg = nhg;
    m_af = af;
    m_flags = nhg.flags;
    m_group.clear();
    m_depends.clear();
    m_dependents.clear();
    m_resolvedGroup.clear();
    m_sonic_obj_type = SONIC_NHG_OBJ_TYPE_NHG_NORMAL;
    m_has_sonic_pic_obj = false;
    m_is_srv6_nhg = false;
    m_is_shared_sonic_nhg = false;
    m_create_sonic_nhg_obj = false;

    // update m_depends set
    for (auto it = nhg.depends.begin(); it != nhg.depends.end(); it++) {
        m_depends.insert(*it);
        SWSS_LOG_DEBUG("NextHop id %d add depends %d.", m_rib_id, *it);
    }

    // update m_dependents set
    for (auto it = nhg.dependents.begin(); it != nhg.dependents.end(); it++) {
        m_dependents.insert(*it);
        SWSS_LOG_DEBUG("NextHop id %d add dependents %d.", m_rib_id, *it);
    }

    // check the full list NHG entry and update m_group set
    for (auto it = nhg.nh_grp_full_list.begin(); it != nhg.nh_grp_full_list.end(); it++) {
        // validate group member
        if (!m_table->isNHGExist(it->id)) {
            SWSS_LOG_ERROR("NextHop id %d in group not found.", it->id);
            return -1;
        }
        m_group.insert(std::make_pair(it->id, it->weight));
        SWSS_LOG_DEBUG("NextHop id %d add group %d.", m_rib_id, it->id);
    }

    /*
     * check if we need to create sonic PIC Content obj
     * and sonic pic obj type
     * */
    checkNeedCreateSonicPICObj();

    /* get resolved group from nhg full */
    if(getResolvedGroupFromNHGFull()!=0){
        SWSS_LOG_ERROR("Failed to get resolved group from nhg full");
        return -1;
    }

    /* Initialize m_resolved_enable_group: all paths enabled on add/update.
     * A normal NHG update from zebra is authoritative, so clear any sticky
     * PIC backwalk marker as well. */
    m_updated_via_backwalk = false;
    m_resolved_enable_group.clear();
    if (m_depends.empty()) {
        /* Leaf NHG: add self-reference so walk_spec can mark leaf disabled */
        m_resolved_enable_group[m_rib_id] = true;
    } else {
        for (auto dep_id : m_depends) {
            m_resolved_enable_group[dep_id] = true;
        }
    }

    /* Populate m_gateway from the nexthop gateway address */
    m_gateway.clear();
    if (nhg.type == fib::NEXTHOP_TYPE_IPV4 || nhg.type == fib::NEXTHOP_TYPE_IPV4_IFINDEX) {
        char gw[INET_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET, &nhg.gate.ipv4, gw, INET_ADDRSTRLEN);
        m_gateway = gw;
    } else if (nhg.type == fib::NEXTHOP_TYPE_IPV6 || nhg.type == fib::NEXTHOP_TYPE_IPV6_IFINDEX) {
        char gw[INET6_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET6, &nhg.gate.ipv6, gw, INET6_ADDRSTRLEN);
        m_gateway = gw;
    }

    // sync fv vector
    if (this->syncFvVector() != 0) {
        SWSS_LOG_ERROR("Failed to sync fv vector for %d", nhg.id);
        return -1;
    }

    /*
     * check if we need to create sonic nhg obj, if already exists, then set the sonic obj id for this entry
     */
    checkNeedCreateSonicNHGObj();

    return 0;
}

/* sync fvVector from entry */
int RIBNHGEntry::syncFvVector(bool backwalk) {
    m_fvVector.clear();
    // get the FV vector fields
    if (getNHGFields(backwalk) != 0) {
        SWSS_LOG_ERROR("get nhg fields failed");
        return -1;
    }

    // check the nexthop str
    if (m_nexthop.empty() && m_nhg.nh_grp_full_list.size() > 0 && m_nhg.type != fib::NEXTHOP_TYPE_IFINDEX) {
        SWSS_LOG_ERROR("nexthop is empty");
        return -1;
    }
    if (m_nexthop.empty()) {
        if (m_af == AF_INET) {
            m_nexthop = "0.0.0.0";
        } else if (m_af == AF_INET6) {
            m_nexthop = "::";
        } else {
            SWSS_LOG_ERROR("sync fv vector failed, unsupported address family %d", m_af);
            return -1;
        }
    }

    FieldValueTuple nh("nexthop", m_nexthop.c_str());
    m_fvVector.push_back(nh);

    if (!m_ifName.empty()) {
        FieldValueTuple ifname("ifname", m_ifName.c_str());
        m_fvVector.push_back(ifname);
    }
    if (!m_weight.empty()) {
        FieldValueTuple wg("weight", m_weight.c_str());
        m_fvVector.push_back(wg);
    }
    if (!m_segSrc.empty() && m_sonic_obj_type == SONIC_NHG_OBJ_TYPE_NHG_WITH_SRV6_PIC) {
        FieldValueTuple wg("seg_src", m_segSrc.c_str());
        m_fvVector.push_back(wg);
    }

    SWSS_LOG_NOTICE("NextHopGroup table set: nexthop[%s] ifname[%s] weight[%s] seg_src[%s]", m_nexthop.c_str(), m_ifName.c_str(),
                  m_weight.c_str(), m_segSrc.c_str());
    return 0;
}

/* get fields from entry */
int RIBNHGEntry::getNHGFields(bool backwalk) {

    if (m_resolvedGroup.size() > 0) {
        SWSS_LOG_DEBUG("multi nexthop group");
        // nexthop with group member
        return getNextHopGroupFields(backwalk);

    } else {
        // nexthop without group member
        SWSS_LOG_DEBUG("single nexthop group");
        return getNextHopFields();
    }
}

/*
 * Resolve leaf-level enable/disable flags by walking the depends tree.
 *
 * m_resolved_enable_group tracks depends-level IDs, but m_resolvedGroup
 * contains leaf-level IDs. For intermediate nodes these don't overlap.
 * This function recursively resolves each leaf's effective status by walking
 * through enabled depends paths down to the leaves.
 */
std::unordered_map<uint32_t, bool> RIBNHGEntry::resolveLeafEnableFlags() {
    std::unordered_map<uint32_t, bool> result;

    /* Leaf node: m_resolved_enable_group has self-reference {self_id: bool} */
    if (m_depends.empty()) {
        for (const auto& kv : m_resolved_enable_group) {
            result[kv.first] = kv.second;
        }
        return result;
    }

    /* Non-leaf: initialize all leaves in m_resolvedGroup as disabled */
    for (const auto& nh : m_resolvedGroup) {
        result[nh.first] = false;
    }

    /* Walk each depends subtree */
    for (uint32_t dep_id : m_depends) {
        /* If this depends path is disabled at our level, skip entire subtree */
        auto it = m_resolved_enable_group.find(dep_id);
        if (it != m_resolved_enable_group.end() && !it->second) {
            continue;
        }

        /* Recurse into the dep entry to get its leaf flags */
        RIBNHGEntry* dep_entry = m_table->getEntry(dep_id);
        if (dep_entry == nullptr) {
            continue;
        }

        auto dep_leaf_flags = dep_entry->resolveLeafEnableFlags();

        /* Merge: a leaf is enabled if ANY enabled depends path has it enabled */
        for (const auto& lf : dep_leaf_flags) {
            if (lf.second && result.count(lf.first)) {
                result[lf.first] = true;
            }
        }
    }

    return result;
}

/*
 * get FV vector fields and Sonic gateway Objects fields from multi nexthopgroup entry
 *
 * below fields of entry will be set:
 *      m_nexthop
 *      m_ifName
 *      m_weight
 *
 * for SRv6 VPN case:
 *      m_vpnSid
 *      m_segSrc
 */
int RIBNHGEntry::getNextHopGroupFields(bool backwalk) {
    string nexthops = "";
    string ifnames = "";
    string weights = "";
    string vpnSids = "";
    string segSrcs = "";

    /* Only apply leaf-disable filter during PIC backwalk.
     * For zebra events, zebra is the source of truth — all paths are valid.
     */
    std::unordered_map<uint32_t, bool> leaf_flags;
    if (backwalk) {
        leaf_flags = resolveLeafEnableFlags();
    }

    for (const auto &nh: m_resolvedGroup) {
        uint32_t id = nh.first;

        if (backwalk) {
            auto leaf_it = leaf_flags.find(id);
            if (leaf_it != leaf_flags.end() && !leaf_it->second) {
                SWSS_LOG_NOTICE("NextHop id %d skipped (disabled via resolved leaf flags)", id);
                continue;
            }
        }

        string weight = to_string(nh.second);
        if (!m_table->isNHGExist(id)) {
            SWSS_LOG_ERROR("NextHop group is incomplete: nhg %d not found", id);
            return -1;
        }
        RIBNHGEntry *entry = this->m_table->getEntry(id);
        /* Skip an inactive nexthop as an invalid path, unless the member group
         * has NEXTHOP_GROUP_RECEIVED_FLAG set in its nhg_flags (zebra has
         * confirmed the full group), in which case we still process it. */
        if (!(entry->m_flags & NEXTHOP_FLAG_ACTIVE) &&
            !CHECK_FLAG(entry->m_nhg.nhg_flags, NEXTHOP_GROUP_RECEIVED_FLAG)) {
            SWSS_LOG_NOTICE("NextHop id %d skipped (NEXTHOP_FLAG_ACTIVE not set, flags 0x%x, nhg_flags 0x%x), path is not valid",
                            id, entry->m_flags, entry->m_nhg.nhg_flags);
            continue;
        }
        if (!nexthops.empty()) {
            nexthops += NHG_DELIMITER;
        }
        if (!ifnames.empty()) {
            ifnames += NHG_DELIMITER;
        }
        if (!weights.empty()) {
            weights += NHG_DELIMITER;
        }
        nexthops += entry->getNextHopStr();
        SWSS_LOG_NOTICE(" entry nexthop: [%s]", entry->getNextHopStr().c_str());
        ifnames += entry->getInterfaceNameStr();
        SWSS_LOG_NOTICE(" entry interface: [%s]", entry->getInterfaceNameStr().c_str());
        weights += weight;
        SWSS_LOG_DEBUG(" entry weight: [%s]", weight.c_str());

        /* SRv6 VPN SID */
        if(m_sonic_obj_type == SONIC_NHG_OBJ_TYPE_NHG_WITH_SRV6_PIC){
            if (!vpnSids.empty()){
                vpnSids += NHG_DELIMITER;
            }
            if(!segSrcs.empty()){
                segSrcs += NHG_DELIMITER;
            }
            vpnSids += entry->getVPNSIDStr();
            segSrcs += entry->getSegSrcStr();
            SWSS_LOG_DEBUG(" entry vpnSid: [%s], segSrc: [%s]", vpnSids.c_str(), segSrcs.c_str());
        }
    }

    if (nexthops.empty()) {
        SWSS_LOG_ERROR("get NextHopGroup fields failed for rib id %d: no active nexthop in group",
                       m_rib_id);
        return -1;
    }

    m_nexthop = nexthops;
    m_ifName = ifnames;
    m_vpnSid = vpnSids;
    m_segSrc = segSrcs;
    m_weight = weights;
    SWSS_LOG_NOTICE("get NextHopGroup fields done, nexthop[%s] ifname[%s] weight[%s] vpnSid[%s] segSrc[%s]", m_nexthop.c_str(), m_ifName.c_str(),
                    m_weight.c_str(), m_vpnSid.c_str(), m_segSrc.c_str());
    return 0;
}

/*
 * get FV vector fields and sonic pic Objects fields from multi nexthopgroup entry
 *
 * below fields of entry will be set:
 *      m_nexthop
 *      m_ifName
 *
 * for SRv6 VPN case:
 *      m_vpnSid
 *      m_segSrc
 */
int RIBNHGEntry::getNextHopFields() {

    if (m_af == AF_INET) {
        char gateway[INET_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET, &m_nhg.gate.ipv4, gateway, INET_ADDRSTRLEN);
        m_nexthop = gateway;
        m_ifName = m_nhg.ifname;
    }else if(m_af == AF_INET6) {
        char gateway[INET6_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET6, &m_nhg.gate.ipv6, gateway, INET6_ADDRSTRLEN);
        m_nexthop = gateway;
        m_ifName = m_nhg.ifname;
    }

    if (m_sonic_obj_type == SONIC_NHG_OBJ_TYPE_NHG_WITH_SRV6_PIC) {
        char sid[INET6_ADDRSTRLEN] = {0};
        char seg_src[INET6_ADDRSTRLEN] = {0};
        if (m_nhg.nh_srv6 != nullptr && m_nhg.nh_srv6->seg6_segs != nullptr){
            inet_ntop(AF_INET6, &m_nhg.nh_srv6->seg6_segs->seg[0], sid, INET6_ADDRSTRLEN);
            m_vpnSid = sid;
            inet_ntop(AF_INET6, &m_nhg.nh_srv6->seg6_src, seg_src, INET6_ADDRSTRLEN);
            m_segSrc = seg_src;
        }else{
            SWSS_LOG_ERROR("single nexthop id %d type srv6 gateway has no seg6_segs", m_rib_id);
            return -1;
        }
    }
    SWSS_LOG_NOTICE("get NextHopGroup fields done, nexthop[%s] ifname[%s] weight[%s] vpnSid[%s] segSrc[%s]", m_nexthop.c_str(), m_ifName.c_str(),
                    m_weight.c_str(), m_vpnSid.c_str(), m_segSrc.c_str());
    return 0;
}

/*
 * get resolved group from nexthopgroupfull
 * below fields of entry will be set:
 *      m_resolvedGroup
 *      m_is_single
 */
int RIBNHGEntry::getResolvedGroupFromNHGFull() {
    if (m_sonic_obj_type == SONIC_NHG_OBJ_TYPE_NHG_NORMAL) {
        for (auto nhg: m_nhg.nh_grp_full_list) {
            if (nhg.num_direct == 0) {
                m_resolvedGroup.insert(std::make_pair(nhg.id, nhg.weight));
                SWSS_LOG_DEBUG("NextHop id %d add resolved group %d.", m_rib_id, nhg.id);
            }
        }

    } else if (m_sonic_obj_type == SONIC_NHG_OBJ_TYPE_NHG_WITH_SRV6_PIC) {
        for (auto nhg: m_nhg.nh_grp_full_list){
            RIBNHGEntry *entry = m_table->getEntry(nhg.id);
            if (entry == nullptr){
                SWSS_LOG_ERROR("NextHop id %d in group not found.", nhg.id);
                return -1;
            }
            if (entry->hasSonicPICObj()){
                m_resolvedGroup.insert(std::make_pair(nhg.id, nhg.weight));
                SWSS_LOG_DEBUG("NextHop id %d add resolved group %d, type %d.", m_rib_id, nhg.id, entry->getSonicObjType());
            }
        }
    }
    if(m_resolvedGroup.size() <= 1){
        m_is_single = true;
    }else{
        m_is_single = false;
    }
    return 0;
}

/*
 * check if we need to create sonic pic obj and the object type
 * below fields of entry will be set:
 *      m_sonic_obj_type
 *      m_has_sonic_pic_obj
 */
void RIBNHGEntry::checkNeedCreateSonicPICObj() {

    /* now only for srv6 vpn PIC Content obj */

    /* for srv6 gateway, check if we need to create pic group obj */
    for (auto it = m_group.begin(); it != m_group.end(); it++) {
        RIBNHGEntry *entry = m_table->getEntry(it->first);
        if (entry == nullptr) {
            continue;
        }
        if (entry->hasSonicPICObj()) {
            m_sonic_obj_type = entry->getSonicObjType();
            m_has_sonic_pic_obj = true;
            m_is_shared_sonic_nhg = true;
            SWSS_LOG_DEBUG("NextHop id %d has sonic pic obj, is multi nexthop group.", it->first);
            return;
        }
        if(entry->isSRv6Nhg()) {
            m_is_srv6_nhg = true;
        }
    }

    if (m_nhg.nh_srv6 != nullptr && m_nhg.nh_srv6->seg6_segs != nullptr) {
        m_is_srv6_nhg = true;
    }

    /*
     * For srv6 received nexthop, should create a sonic pic obj and shared sonic NHG object.
     * This covers both single-hop SRv6 VPN nexthops (no group members) and
     * recursive SRv6 VPN nexthops that carry their own SRv6 VPN info.
     */
    if (m_is_srv6_nhg && CHECK_FLAG(m_nhg.nhg_flags, NEXTHOP_GROUP_RECEIVED_FLAG)) {
        m_sonic_obj_type = SONIC_NHG_OBJ_TYPE_NHG_WITH_SRV6_PIC;
        m_has_sonic_pic_obj = true;
        m_is_shared_sonic_nhg = true;
        SWSS_LOG_DEBUG("NextHop id %d has sonic pic obj.", m_rib_id);
        return;
    }

    return;
}


void RIBNHGEntry::checkNeedCreateSonicNHGObj() {
    SonicNHGObjectKey::createSonicNormalNHGObjectKey(this, m_sonic_nhg_key);

    if (m_sonic_obj_type == SONIC_NHG_OBJ_TYPE_NHG_WITH_SRV6_PIC){
        /*
         * For SRv6 VPN NHG, create shared NHG object.
         */
        uint32_t id = m_table->getCreatedSharedNHGObjectID(m_sonic_nhg_key);
        if (id == 0){
            m_create_sonic_nhg_obj = true;
        }else{
            m_create_sonic_nhg_obj = false;
            if (m_sonic_obj_id != id) {
                m_sonic_obj_id = id;
                m_table->addSonicNHGObjectRef(m_sonic_nhg_key);
            }
        }
    }else{
        /*
         * Skipped received NHG without SRv6 info.
         */
        if (CHECK_FLAG(m_nhg.nhg_flags, NEXTHOP_GROUP_RECEIVED_FLAG)){
            SWSS_LOG_NOTICE("NextHop %d is a received NHG without SRv6 info, skip create sonic object.", m_rib_id);
            m_create_sonic_nhg_obj = false;
            return ;
        }

        /*
         * Skipped NHG with SRv6 info but not received NHG.
         */
        if (!CHECK_FLAG(m_nhg.nhg_flags, NEXTHOP_GROUP_RECEIVED_FLAG) && m_is_srv6_nhg){
            SWSS_LOG_NOTICE("NextHop %d is a NHG with SRv6 VPN, but not is received NHG, skip create sonic object.", m_rib_id);
            m_create_sonic_nhg_obj = false;
            return ;
        }
        if (m_is_single == true){
            m_create_sonic_nhg_obj = false;
        } else{
            m_create_sonic_nhg_obj = true;
        }
    }
}


SonicPICContentTable::SonicPICContentTable(RedisPipeline *pipeline, const std::string &picTableName, bool isStateTable) : m_pic_contextTable(pipeline, picTableName, isStateTable) {
}

/*
 * write the sonic pic object into DB
 * SONIC_NHG_OBJ_TYPE_NHG_WITH_SRV6_PIC into PIC_CONTEXT_TABLE
 */
int SonicPICContentTable::writeToDB(SonicPICContentEntry *entry) {
    vector<FieldValueTuple> fvVector = entry->getFvVector();
    if (fvVector.size() == 0) {
        SWSS_LOG_ERROR("Failed to sync fvVector for %d, empty fvVector", entry->getObj().id);
        return -1;
    }
    if (entry->getType() == SONIC_NHG_OBJ_TYPE_NHG_WITH_SRV6_PIC) {
        m_pic_contextTable.set(std::to_string(entry->getSonicPicContentObjId()), fvVector);
    }
    return 0;
}

/*
 * remove the sonic pic object from DB
 */
void SonicPICContentTable::removeFromDB(SonicPICContentEntry *entry) {
    if (entry->getType() == SONIC_NHG_OBJ_TYPE_NHG_WITH_SRV6_PIC) {
        m_pic_contextTable.del(std::to_string(entry->getSonicPicContentObjId()));
        SWSS_LOG_DEBUG("Remove PIC Context object id %d", entry->getSonicPicContentObjId());
    }
    return;
}

/*
 * add the SonicPICContentEntry
 */
int SonicPICContentTable::addEntry(SonicPICContentObject sonicObj) {
    if (getEntry(sonicObj.type, sonicObj.id) != nullptr) {
        SWSS_LOG_ERROR("SonicPICContentObject is already exist: %d", sonicObj.id);
        return -1;
    }

    SonicPICContentEntry *entry = SonicPICContentEntry::createSonicPICContentEntry(this);
    if (entry == nullptr) {
        return -1;
    }

    // set SonicPICContentEntry fields
    int ret = entry->setEntry(sonicObj);
    if (ret != 0) {
        delete entry;
        return -1;
    }

    switch (entry->getType()) {
        case SONIC_NHG_OBJ_TYPE_NHG_WITH_SRV6_PIC: {
            m_pic_map[entry->getSonicPicContentObjId()] = entry;
            break;
        }
        default: {
            SWSS_LOG_WARN("Skip SonicPICContentObject type");
            break;
        }
    }

    return 0;
}


/*
 * delete the SonicPICContentEntry by type and id
 */
void SonicPICContentTable::delEntry(sonicNhgObjType type, uint32_t id) {
    SonicPICContentEntry *entry = nullptr;
    switch (type) {
        case SONIC_NHG_OBJ_TYPE_NHG_WITH_SRV6_PIC: {
            if (m_pic_map.find(id) != m_pic_map.end()) {
                entry = m_pic_map[id];
                m_pic_map.erase(id);
            } else {
                entry = nullptr;
            }
            break;
        }
        default: {
            entry = nullptr;
            break;
        }
    }
    if (entry == nullptr) {
        SWSS_LOG_WARN("SonicPICContentObject is not exist: %d %d", type, id);
        return;
    }

    removeFromDB(entry);
    delete entry;
    return;
}

/*
 * delete all the SonicPICContentEntry
 */
void SonicPICContentTable::cleanUp() {
    for (auto it = m_pic_map.begin(); it != m_pic_map.end(); it++) {
        delete it->second;
    }
    m_pic_map.clear();
}


/*
 * get the SonicPICContentEntry by type and id
 */
SonicPICContentEntry *SonicPICContentTable::getEntry(sonicNhgObjType type, uint32_t sonicID) {
    switch (type) {
        case SONIC_NHG_OBJ_TYPE_NHG_WITH_SRV6_PIC: {
            if (m_pic_map.find(sonicID) != m_pic_map.end()) {
                return m_pic_map[sonicID];
            }
            return nullptr;
        }
        default: {
            SWSS_LOG_ERROR("Failed to get SonicPICContentEntry: %d %d", type, sonicID);
            return nullptr;
        }
    }
}

/*
 * create SonicPICContentEntry
 */
SonicPICContentEntry *SonicPICContentEntry::createSonicPICContentEntry(SonicPICContentTable *mTable) {
    SonicPICContentEntry *entry = new SonicPICContentEntry(mTable);
    return entry;
}


/*
 * sync the fvVector for SonicPICContentEntry
 */
int SonicPICContentEntry::syncFvVector() {
    m_fvVector.clear();
    switch (m_sonic_obj.type) {
        case SONIC_NHG_OBJ_TYPE_NHG_WITH_SRV6_PIC: {
            return syncFvVectorForSRv6PIC();
        }
        default: {
            SWSS_LOG_ERROR("Unsupported SonicPICContentObject type: %d", m_sonic_obj.type);
            return -1;
        }
    }
}

/*
 * sync the fvVector for SRv6 Gateway
 */
int SonicPICContentEntry::syncFvVectorForSRv6PIC() {
    if (m_sonic_obj.groupMember.size() == 0) {
        m_fvVector.push_back(FieldValueTuple("nexthop", m_sonic_obj.nexthop));
        m_fvVector.push_back(FieldValueTuple("ifname", m_sonic_obj.ifName));
        m_fvVector.push_back(FieldValueTuple("seg_src", m_sonic_obj.segSrc));
        m_fvVector.push_back(FieldValueTuple("vpn_sid", m_sonic_obj.vpnSid));
    }else {
        string nexthops = "";
        string vpnSids = "";
        string ifnames = "";
        string weights = "";
        string segSrcs = "";
        for (auto member: m_group) {
            SonicPICContentEntry *entry = m_table->getEntry(SONIC_NHG_OBJ_TYPE_NHG_WITH_SRV6_PIC, member.first);
            if (entry == nullptr) {
                SWSS_LOG_ERROR("SonicPICContentObject is not exist: %d", member.first);
                return -1;
            }

            SonicPICContentObject obj = entry->getObj();
            if (obj.groupMember.size() > 0) {
                SWSS_LOG_ERROR("SonicPICContentObject member is a group: %d", member.first);
                return -1;
            }

            if (!nexthops.empty()) {
                nexthops += NHG_DELIMITER;
            }
            nexthops += obj.nexthop;

            if (!ifnames.empty()) {
                ifnames += NHG_DELIMITER;
            }
            if (!obj.ifName.empty()) {
                ifnames += obj.ifName;
            } else {
                ifnames += "unknown";
            }

            if (!vpnSids.empty()) {
                vpnSids += NHG_DELIMITER;
            }
            vpnSids += obj.vpnSid;

            if (!weights.empty()) {
                weights += NHG_DELIMITER;
            }
            weights += std::to_string(member.second);

            if (!segSrcs.empty()) {
                segSrcs += NHG_DELIMITER;
            }
            segSrcs += obj.segSrc;
        }
        m_fvVector.push_back(FieldValueTuple("nexthop", nexthops));
        m_fvVector.push_back(FieldValueTuple("ifname", ifnames));
        m_fvVector.push_back(FieldValueTuple("weight", weights));
        m_fvVector.push_back(FieldValueTuple("vpn_sid", vpnSids));
        m_fvVector.push_back(FieldValueTuple("seg_src", segSrcs));
    }

    for (auto it = m_fvVector.begin(); it != m_fvVector.end(); it++) {
        SWSS_LOG_DEBUG("Sync fvVector for SonicPICContentEntry %d, type %d, %s %s", m_sonic_obj_id, m_sonic_obj.type, fvField(*it).c_str(), fvValue(*it).c_str());
    }
    return 0;
}

int SonicPICContentEntry::setEntry(SonicPICContentObject nhg) {
    switch (nhg.type) {
        case SONIC_NHG_OBJ_TYPE_NHG_WITH_SRV6_PIC: {
            setSRv6PICEntry(nhg);
            break;
        }
        default: {
            SWSS_LOG_ERROR("Unsupported SonicPICContentObject type: %d", nhg.type);
            return -1;
        }
    }
    int ret = syncFvVector();
    if (ret != 0) {
        SWSS_LOG_ERROR("Failed to sync fvVector for entry");
        return ret;
    }
    return 0;
}

int SonicPICContentEntry::setSRv6PICEntry(SonicPICContentObject nhg) {
    m_sonic_obj = nhg;
    m_sonic_obj_id = nhg.id;
    m_sonic_obj_key = SonicNHGObjectKey::createSonicPICContentObjectKey(nhg);
    m_group.clear();
    for (auto member: nhg.groupMember) {
        SonicPICContentEntry *entry = m_table->getEntry(nhg.type, member.first);
        if (entry == nullptr) {
            SWSS_LOG_ERROR("SonicPICContentObject is not exist");
            return -1;
        }
        m_group.insert(std::make_pair(entry->getSonicPicContentObjId(), member.second));
    }
    return 0;
}

SonicPICContentEntry::SonicPICContentEntry(SonicPICContentTable *mTable) : m_table(mTable) {
}

SonicPICContentEntry::~SonicPICContentEntry() {
    m_table = nullptr;
}

u_int32_t SonicIDMgr::allocateID(sonicNhgObjType type) {
    SonicIDAllocator *allocator = getAllocator(type);
    if (allocator == nullptr) {
        SWSS_LOG_ERROR("SonicIDAllocator is not exist: %d", type);
        return 0;
    }
    auto id = allocator->allocateID();
    SWSS_LOG_NOTICE("Allocate id : type %d id %d", (int) type, (int) id);
    return id;
}

void SonicIDMgr::freeID(sonicNhgObjType type, uint32_t id) {
    SonicIDAllocator *allocator = getAllocator(type);
    if (allocator == nullptr) {
        SWSS_LOG_ERROR("SonicIDAllocator is not exist: %d", type);
        return;
    }
    SWSS_LOG_NOTICE("Free id : type %d id %d", (int) type, (int) id);
    allocator->freeID(id);
}

SonicIDAllocator *SonicIDMgr::getAllocator(sonicNhgObjType type) {
    switch (type) {
        case SONIC_NHG_OBJ_TYPE_NHG_WITH_SRV6_PIC:
            return m_pic_id_allocator;
        case SONIC_NHG_OBJ_TYPE_NHG_NORMAL:
            return m_nhg_id_allocator;
        default:
            return nullptr;
    }
}

int SonicIDMgr::init(vector<sonicNhgObjType> supportedObjs) {
    for (auto type: supportedObjs) {
        switch (type) {
            case SONIC_NHG_OBJ_TYPE_NHG_WITH_SRV6_PIC: {
                if (m_pic_id_allocator == nullptr) {
                    m_pic_id_allocator = new SonicIDAllocator(APP_PIC_CONTEXT_TABLE_NAME);
                }
                break;
            }
            case SONIC_NHG_OBJ_TYPE_NHG_NORMAL: {
                if (m_nhg_id_allocator == nullptr) {
                    m_nhg_id_allocator = new SonicIDAllocator(APP_NEXTHOP_GROUP_TABLE_NAME);
                }
                break;
            }
            default: {
                SWSS_LOG_ERROR("Unsupported SonicPICContentObject type: %d", type);
                return -1;
            }
        }
    }
    return 0;
}

uint32_t SonicIDAllocator::allocateID() {
    uint32_t attempts = 0;
    while (m_id_map.find(g_id) != m_id_map.end()) {
        if (++attempts >= UINT32_MAX) {
            SWSS_LOG_ERROR("ID space exhausted");
            return 0;
        }
        if (g_id == 0) {
            g_id = 1;
        } else {
            g_id++;
        }
    }
    m_id_map[g_id] = 1;
    uint32_t allocated = g_id;
    g_id++;
    if (g_id == 0) {
        g_id = 1;
    }
    return allocated;
};

void SonicIDAllocator::freeID(uint32_t id) {
    if (m_id_map.find(id) != m_id_map.end()) {
        m_id_map.erase(id);
    }
}

bool SonicIDAllocator::isInUsed(uint32_t id) {
    if (m_id_map.find(id) != m_id_map.end()) {
        return true;
    }
    return false;
}

/*
 * create SonicNHGObjectKey from SonicPICContentObject
 */
SonicNHGObjectKey SonicNHGObjectKey::createSonicPICContentObjectKey(SonicPICContentObject obj) {
    SonicNHGObjectKey key;
    key.groupMember = obj.groupMember;
    key.type = obj.type;
    if (obj.type == SONIC_NHG_OBJ_TYPE_NHG_WITH_SRV6_PIC) {
        key.nexthop = obj.nexthop;
        key.segSrc = obj.segSrc;
        key.vpnSid = obj.vpnSid;
        key.ifName = obj.ifName;
    }
    return key;
}

void SonicNHGObjectKey::createSonicNormalNHGObjectKey(RIBNHGEntry *entry, SonicNHGObjectKey &key_out) {
    key_out.type = SONIC_NHG_OBJ_TYPE_NHG_NORMAL;
    key_out.groupMember.clear();
    key_out.nexthop="";
    key_out.ifName="";
    key_out.segSrc="";
    key_out.vpnSid="";
    if (entry->isSingleNexthop()){
        key_out.nexthop = entry->getNextHopStr();
        key_out.ifName = entry->getInterfaceNameStr();
        key_out.segSrc = entry->getSegSrcStr();
        key_out.vpnSid = entry->getVPNSIDStr();
    }else{
        for (auto member: entry->getResolvedGroup()){
            key_out.groupMember.push_back(std::make_pair(member.first, member.second));
        }
    }
}

/*
 * create SonicNHGObjectKey from RIBNHGEntry
 */
int SonicNHGObjectKey::createSonicPICContentObjectKey(RIBNHGEntry *entry, SonicNHGObjectKey &key_out) {
    key_out.groupMember.clear();
    SonicPICContentObject obj;
    int ret = entry->createSonicPICContentObjectFromRIBEntry(obj);
    if (ret != 0) {
        return ret;
    }
    key_out = createSonicPICContentObjectKey(obj);
    return 0;
}

/*
 * ============================================================
 * PIC Backwalk Infrastructure
 * ============================================================
 */

/*
 * Helper: check if all m_resolved_enable_group entries are false
 */
static bool isAllDisabled(RIBNHGEntry* entry) {
    auto& enable_group = entry->getResolvedEnableGroup();
    for (const auto& kv : enable_group) {
        if (kv.second) {
            return false;
        }
    }
    return true;
}

/*
 * Add dependents relationship: for each entry in depends, add id to their m_dependents
 */
int RIBNHGTable::addNHGDependents(set<uint32_t> depends, uint32_t id) {
    for (auto it = depends.begin(); it != depends.end(); it++) {
        RIBNHGEntry *e = getEntry(*it);
        if (e == nullptr) {
            return -1;
        }
        e->addDependentsMember(id);
    }
    return 0;
}

/*
 * Remove dependents relationship: for each entry in depends, remove id from their m_dependents
 */
void RIBNHGTable::removeNHGDependents(set<uint32_t> depends, uint32_t id) {
    for (auto it = depends.begin(); it != depends.end(); it++) {
        RIBNHGEntry *e = getEntry(*it);
        if (e == nullptr) {
            continue;
        }
        e->removeDependentsMember(id);
    }
}

/*
 * Entry point when NHT event indicates a nexthop became unreachable.
 * Performs two-phase backwalk: Part 1 (global table) and Part 2 (VRF/VPN).
 */
void NHGMgr::fib_nhg_trigger_node_quick_fixup(const std::string& nexthop_addr, uint32_t resolved_nhg_id) {

    SWSS_LOG_NOTICE("PIC: trigger_node_quick_fixup nexthop=%s resolved_nhg_id=%u",
                    nexthop_addr.c_str(), resolved_nhg_id);

    /* ---- Part 1: Global Table Context Backwalk ---- */
    std::set<RIBNHGEntry*> global_entries;

    /* Try resolved_nhg_id first */
    RIBNHGEntry* entry_by_id = m_rib_nhg_table->getEntry(resolved_nhg_id);
    if (entry_by_id != nullptr) {
        global_entries.insert(entry_by_id);
    } else {
        /* Fallback: use nexthop address to find leaf RIBNHGEntries */
        global_entries = m_rib_nhg_table->getGlobalEntries(nexthop_addr);
    }

    for (auto* start_entry : global_entries) {
        fib_nhg_walking_ctx ctx;
        ctx.nexthop_address = nexthop_addr;
        ctx.rib_nhg_table = m_rib_nhg_table;
        ctx.fib_nhg_walk_spec_func = fib_nhg_walk_spec_for_node_quick_fixup;
        ctx.fib_nhg_prune_spec_func = fib_nhg_prune_spec_for_node_quick_fixup;

        fib_nhg_back_walk(start_entry->getRIBID(), ctx);

        SWSS_LOG_NOTICE("PIC: Part 1 done, visited %zu nodes, modified %zu nodes",
                        ctx.visited_node_set.size(), ctx.modified_node_set.size());
    }

    /* ---- Part 2: VPN Context Backwalk ---- */
    const std::set<RIBNHGEntry*>& vrf_entries = m_rib_nhg_table->getVrfEntries(nexthop_addr);
    for (auto* start_entry : vrf_entries) {
        fib_nhg_walking_ctx ctx;
        ctx.nexthop_address = nexthop_addr;
        ctx.rib_nhg_table = m_rib_nhg_table;
        ctx.fib_nhg_walk_spec_func = fib_nhg_walk_spec_for_node_quick_fixup_sonic_nhg;
        ctx.fib_nhg_prune_spec_func = fib_nhg_prune_spec_for_node_quick_fixup_sonic_nhg;

        fib_nhg_back_walk(start_entry->getRIBID(), ctx);

        SWSS_LOG_NOTICE("PIC: Part 2 VPN entry %d done, visited %zu, modified %zu",
                        start_entry->getRIBID(), ctx.visited_node_set.size(), ctx.modified_node_set.size());
    }
}

/*
 * Generic backwalk traversal from a given NHG ID.
 * Invokes walk_spec, then prune_spec, then recursively visits dependents.
 */
void NHGMgr::fib_nhg_back_walk(uint32_t id, fib_nhg_walking_ctx& ctx) {

    RIBNHGEntry* entry = m_rib_nhg_table->getEntry(id);
    if (entry == nullptr) {
        SWSS_LOG_WARN("PIC: back_walk entry %u not found", id);
        return;
    }

    /* Track visited node */
    bool walk_result = ctx.fib_nhg_walk_spec_func(entry, ctx);
    ctx.visited_node_set.insert(id);

    /* Prune evaluation */
    bool prune = ctx.fib_nhg_prune_spec_func(entry, walk_result, ctx);
    if (prune) {
        return;
    }

    /* Recursive traversal to dependents */
    std::set<uint32_t> dependents = entry->getDependentsID();
    for (uint32_t dep_id : dependents) {
        fib_nhg_back_walk(dep_id, ctx);
    }
}

/*
 * Part 1 walk spec: applies fixup to global-table NHGs.
 * Uses Visit-for-State pattern: propagate disabled state from child entries.
 */
bool NHGMgr::fib_nhg_walk_spec_for_node_quick_fixup(RIBNHGEntry* entry, fib_nhg_walking_ctx& ctx) {

    const std::string& nexthop = ctx.nexthop_address;
    uint32_t entry_id = entry->getRIBID();
    std::set<uint32_t> depends = entry->getDependsID();
    auto& enable_group = entry->getResolvedEnableGroup();

    SWSS_LOG_NOTICE("Walk to node %d", entry->getRIBID());
    /* Visit-for-State: re-derive each dep's enable flag from the dep's
     * current state. The gateway-match branch below writes false durably
     * into enable_group; if an unrelated later trigger visits this entry,
     * that stale false would suppress legitimate APPDB updates until the
     * next NHGFULL reset. Healing here lets false->true transitions happen
     * between triggers, not only at NHGFULL boundaries. */
    for (uint32_t dep_id : depends) {
        RIBNHGEntry* dep_entry = ctx.rib_nhg_table->getEntry(dep_id);
        if (dep_entry) {
            enable_group[dep_id] = !isAllDisabled(dep_entry);
        }
        SWSS_LOG_NOTICE("Check depend %d, enable flag %d", dep_id, (int) enable_group[dep_id]);
    }

    bool is_relevant = false;

    /* Check if gateway matches impacted nexthop */
    if (entry->getGatewayAddress() == nexthop) {
        if (depends.empty()) {
            /* Leaf NHG: mark self-reference disabled */
            enable_group[entry_id] = false;
            entry->setUpdatedViaBackwalk();
            ctx.modified_node_set.insert(entry_id);
            SWSS_LOG_DEBUG("PIC: walk_spec leaf node %u gateway match, disabled self", entry_id);
            return true;
        } else {
            /* Non-leaf with gateway match: mark ALL depends disabled */
            for (auto& kv : enable_group) {
                kv.second = false;
            }
            is_relevant = true;
        }
    }

    /* Check if any depends entry appears in modified_node_set */
    if (!is_relevant) {
        for (uint32_t dep_id : depends) {
            if (ctx.modified_node_set.count(dep_id)) {
                is_relevant = true;
                break;
            }
        }
    }

    SWSS_LOG_NOTICE("Node is %d", (int) is_relevant);
    if (!is_relevant) {
        return false;
    }

    /* Check if all paths are disabled */
    bool all_disabled = true;
    for (const auto& kv : enable_group) {
        if (kv.second) {
            all_disabled = false;
            break;
        }
    }

    ctx.modified_node_set.insert(entry_id);

    /* This entry's effective APPDB output was changed by the PIC backwalk and
     * now diverges from zebra's last full NHG state. Tag it so the next NHGFULL
     * for this id forces a HW re-push (the flag survives the enable_group reset
     * that a dependency's NHGFULL cascade performs). Cleared in setEntry. */
    entry->setUpdatedViaBackwalk();

    if (all_disabled) {
        SWSS_LOG_NOTICE("PIC: walk_spec node %u all paths disabled, skip APPDB write", entry_id);
        return true;
    }

    /* Regenerate NHG fields with disabled paths excluded */
    if (entry->regenerateFields() != 0) {
        SWSS_LOG_ERROR("PIC: walk_spec node %u regenerateFields failed", entry_id);
        return true;
    }

    /* Write to APPDB (writeToDB has compare-and-skip dedup) */
    if (entry->needCreateSonicObject() && entry->getSonicObjID() != 0) {
        ctx.rib_nhg_table->writeToDB(entry);
        SWSS_LOG_NOTICE("PIC: walk_spec modified entry %u, written to APPDB", entry_id);
    }

    return true;
}

/*
 * Part 1 prune spec: determines whether to halt backwalk at this node.
 */
bool NHGMgr::fib_nhg_prune_spec_for_node_quick_fixup(RIBNHGEntry* entry, bool walk_result, fib_nhg_walking_ctx& ctx) {

    /* Rule 1: Multi-path nodes where nexthop wasn't matched always continue --
     * dependents above may have gateways matching the nexthop */
    std::set<uint32_t> depends = entry->getDependsID();
    if (depends.size() >= 2 && !walk_result) {
        return false;  /* don't prune */
    }

    /* Rule 2: No update means no propagation needed */
    if (!walk_result) {
        return true;  /* prune */
    }

    return false;  /* node was modified, continue */
}

/*
 * Part 2 walk spec: updates SONiC NHG representations for VPN context.
 * Tracks already-updated SONiC NHG keys to avoid redundant writes.
 */
bool NHGMgr::fib_nhg_walk_spec_for_node_quick_fixup_sonic_nhg(RIBNHGEntry* entry, fib_nhg_walking_ctx& ctx) {

    const std::string& nexthop = ctx.nexthop_address;
    uint32_t entry_id = entry->getRIBID();
    std::set<uint32_t> depends = entry->getDependsID();
    auto& enable_group = entry->getResolvedEnableGroup();

    /* Visit-for-State: re-derive from dep's current state so a stale false
     * from an earlier gateway-match branch on this entry doesn't suppress a
     * legitimate later update. Same heal pattern as the Part 1 walk above. */
    for (uint32_t dep_id : depends) {
        RIBNHGEntry* dep_entry = ctx.rib_nhg_table->getEntry(dep_id);
        if (dep_entry) {
            enable_group[dep_id] = !isAllDisabled(dep_entry);
        }
    }

    bool is_relevant = false;

    /* Check if gateway matches impacted nexthop */
    if (entry->getGatewayAddress() == nexthop) {
        if (depends.empty()) {
            enable_group[entry_id] = false;
            entry->setUpdatedViaBackwalk();
            ctx.modified_node_set.insert(entry_id);
            return true;
        } else {
            for (auto& kv : enable_group) {
                kv.second = false;
            }
            is_relevant = true;
        }
    }

    /* Check if any depends entry appears in modified_node_set */
    if (!is_relevant) {
        for (uint32_t dep_id : depends) {
            if (ctx.modified_node_set.count(dep_id)) {
                is_relevant = true;
                break;
            }
        }
    }

    if (!is_relevant) {
        return false;
    }

    /* Deduplication: check if this SONiC NHG key was already updated */
    if (entry->hasSonicGatewayObj()) {
        SonicNHGObjectKey key = entry->getSonicNHGObjectKey();
        std::string key_str = key.nexthop + "|" + key.vpnSid + "|" + key.segSrc;
        if (ctx.updated_sonic_nhg_keys.count(key_str) != 0) {
            SWSS_LOG_DEBUG("PIC: sonic NHG key already updated, skip %u", entry_id);
            return true;
        }
        ctx.updated_sonic_nhg_keys.insert(key_str);
    }

    /* Check if all paths disabled */
    bool all_disabled = true;
    for (const auto& kv : enable_group) {
        if (kv.second) {
            all_disabled = false;
            break;
        }
    }

    ctx.modified_node_set.insert(entry_id);

    /* APPDB output changed by the backwalk; force a re-push on the next NHGFULL
     * for this id. Survives the enable_group reset; cleared in setEntry. */
    entry->setUpdatedViaBackwalk();

    if (all_disabled) {
        SWSS_LOG_NOTICE("PIC: walk_spec_sonic_nhg node %u all paths disabled, skip APPDB write", entry_id);
        return true;
    }

    /* Regenerate and write to APPDB */
    if (entry->regenerateFields() != 0) {
        SWSS_LOG_ERROR("PIC: walk_spec_sonic_nhg node %u regenerateFields failed", entry_id);
        return true;
    }

    if (entry->needCreateSonicObject() && entry->getSonicObjID() != 0) {
        ctx.rib_nhg_table->writeToDB(entry);
    }

    SWSS_LOG_NOTICE("PIC: walk_spec_sonic_nhg modified entry %u", entry_id);
    return true;
}

/*
 * Part 2 prune spec: mirrors Part 1 behavior.
 */
bool NHGMgr::fib_nhg_prune_spec_for_node_quick_fixup_sonic_nhg(RIBNHGEntry* entry, bool walk_result, fib_nhg_walking_ctx& ctx) {

    std::set<uint32_t> depends = entry->getDependsID();
    if (depends.size() >= 2 && !walk_result) {
        return false;
    }

    if (!walk_result) {
        return true;
    }

    return false;
}
