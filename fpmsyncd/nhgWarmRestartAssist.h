#ifndef NHG_WARM_RESTART_ASSIST_H
#define NHG_WARM_RESTART_ASSIST_H

#include <linux/netlink.h>
#include <map>
#include <set>
#include <string>
#include <vector>
#include <stdint.h>

#include "warmRestartAssist.h"
#include "nhgmgr.h"

/* Forward declaration for unit test friend access */
namespace ut_fpmsyncd { struct FpmSyncdNhgWarmAssist; }

namespace swss {

class RouteSync;

class NhgWarmRestartAssist : public AppRestartAssist {
    friend struct ut_fpmsyncd::FpmSyncdNhgWarmAssist;

public:
    /* Warm restart FSM（从 NHGMgr 搬入） */
    enum NhgWarmRestartState {
        NHG_WR_NONE,
        NHG_WR_INITIALIZED,
        NHG_WR_RESTORED,
        NHG_WR_RECONCILING,
        NHG_WR_RECONCILED,
    };

    struct SavedNHGInfo {
        sonicObjectID sonicId;
        sonicObjectID picObjId;
        uint8_t af;
    };

    struct AppDbNHGEntry {
        sonicObjectID sonicId;
        std::vector<swss::FieldValueTuple> fvVector;
        bool matched = false;
    };

    struct TempReconcileEntry {
        fib::NextHopGroupFull nhg;
        uint8_t af;
        std::vector<swss::FieldValueTuple> fvVector;
        sonicObjectID reuseSonicId;
        sonicObjectID reusePicObjId;
        bool needsSonicObj = false;
    };

    /* 基类构造参数：appName="fpmsyncd-nhg", dockerName="bgp" */
    NhgWarmRestartAssist(RedisPipeline *pipeline, NHGMgr *nhgMgr);

    /* FSM 是否处于 warm restart 进行中 */
    bool inProgress() const;

    /* Shutdown 路径 */
    void saveState(swss::Table &stateTable);

    /* Startup 路径 */
    void initWarmStart();
    void loadState(swss::Table &stateTable, swss::Table &appDbNhgTable);

    /* FpmLink 消息拦截 + 缓存 */
    bool shouldIntercept(const struct nlmsghdr *nlh) const;
    void bufferNHG(const struct nlmsghdr *nlh);
    void bufferRoute(const struct nlmsghdr *nlh);

    /* Warm end（RouteSync::onWarmStartEnd 驱动） */
    void reconcileNHGs();
    void replayBufferedRoutes(RouteSync &routeSync);

private:
    NHGMgr *m_nhgMgr;   /* 数据路径，不持有所有权 */
    NhgWarmRestartState m_state = NHG_WR_NONE;

    std::vector<std::vector<uint8_t>> m_nhg_raw_buffer;
    std::vector<std::vector<uint8_t>> m_route_raw_buffer;

    std::map<sonicObjectID, SavedNHGInfo> m_saved_nhg_infos;
    std::map<std::string, AppDbNHGEntry> m_appdb_nhg_fvs;
    std::set<ribID> m_reconciled_ids;

    void reconcileNormalSingleHopNHGs();
    void reconcileNHGsWithSonicObj();
    AppDbNHGEntry *findMatchingAppDbEntry(const std::string &fvHash);
    static std::vector<ribID> topologicalSort(
        const std::map<ribID, TempReconcileEntry> &entries);
};

} // namespace swss

#endif // NHG_WARM_RESTART_ASSIST_H
