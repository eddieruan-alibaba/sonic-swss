#ifndef NHG_WARM_RESTART_ASSIST_H
#define NHG_WARM_RESTART_ASSIST_H

#include "dbconnector.h"
#include "table.h"
#include "select.h"
#include "selectabletimer.h"
#include "warmRestartAssist.h"
#include "nhgmgr.h"

#include <map>
#include <string>
#include <vector>

namespace swss {

class NHGWarmRestartAssist : public AppRestartAssist {
public:
    NHGWarmRestartAssist(RedisPipeline *pipeline, const std::string &appName,
                         const std::string &dockerName, NHGMgr *nhgMgr,
                         DBConnector *stateDb);
    ~NHGWarmRestartAssist();

    enum NHGCacheState {
        NHG_STALE  = 0,
        NHG_SAME   = 1,
        NHG_NEW    = 2,
        NHG_DELETE = 3
    };

    struct NHGCacheEntry {
        uint32_t zebra_id;
        uint32_t sonic_obj_id;
        uint32_t sonic_pic_id;
        sonicNhgObjType sonic_obj_type;
        bool is_single;
        bool is_shared;
        uint8_t af;
        std::string sonic_nhg_key_hash;
        NHGCacheState state;
    };

    void readNHGStateFromDB();

    void insertNHGToMap(const NextHopGroupFull &nhg, uint8_t af, bool is_delete);

    void reconcileNHG();

    bool isNHGWarmStartInProgress() const { return m_nhgWarmStartInProgress; }

    void setNHGWarmStartInProgress(bool inProgress) { m_nhgWarmStartInProgress = inProgress; }

private:
    NHGMgr *m_nhgMgr;
    bool m_nhgWarmStartInProgress{false};

    Table m_nhgWarmStateTable;

    std::map<uint32_t, NHGCacheEntry> m_nhgCacheMap;

    struct PendingNHGEntry {
        NextHopGroupFull nhg;
        uint8_t af;
        bool is_delete;
    };
    std::map<uint32_t, PendingNHGEntry> m_pendingEntries;

    void reconcileSingleHop();
    void reconcileMultiHop();
    bool compareWithIncoming(const NHGCacheEntry &cached, const SonicNHGObjectKey &incomingKey);

    static SonicNHGObjectKey buildKeyFromNHGFull(const NextHopGroupFull &nhg, uint8_t af);
};

}

#endif
