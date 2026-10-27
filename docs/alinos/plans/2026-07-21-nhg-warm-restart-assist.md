# NHG Warm Restart 重构（NhgWarmRestartAssist）Implementation Plan

> **For agentic workers:** Use /alinos.subagent-dev (recommended) or /alinos.executing-plans to implement this plan task-by-task.

**Goal:** 将 fpmsyncd NHG warm restart 全部逻辑从 NHGMgr/RouteSync/FpmLink 迁移到新的 `NhgWarmRestartAssist` 类（继承 `AppRestartAssist` 外壳），纯结构重构、行为不变。

**Architecture:** RouteSync 持有 `NhgWarmRestartAssist` 实例（`fpmsyncd/nhgWarmRestartAssist.h/.cpp`），Assist 反持 NHGMgr 指针做数据路径操作；FpmLink 拦截、fpmsyncd.cpp 三处调用、`onWarmStartEnd` 驱动全部改走 Assist。基类构造注册 STATE_DB 条目 `fpmsyncd-nhg`（dockerName `bgp`）。

**Tech Stack:** C++17, SONiC swss (fpmsyncd), swss-common (WarmStart/AppRestartAssist), autotools, gtest（tests/mock_tests/tests_fpmsyncd）。

**Spec:** `docs/alinos/specs/2026-07-21-nhg-warm-restart-assist-design.md`

**关键约束：**
- 行为不变：算法代码（FV 匹配、拓扑排序、解析）逐行搬运，只改归属与访问路径
- 所有 commit 通过 `/alinos.commit`（需 Aone ID）
- 构建与测试必须在 swss 构建环境（Linux 容器）执行，macOS 宿主机不能直接构建：
  - 构建 UT：`make -C tests/mock_tests tests_fpmsyncd -j$(nproc)`
  - 运行 UT：`./tests/mock_tests/tests_fpmsyncd --gtest_filter='*WarmRestart*'`
  - 构建 fpmsyncd：`make -C fpmsyncd fpmsyncd -j$(nproc)`

**现状代码锚点（搬运源）：**

| 内容 | 位置 |
|------|------|
| FSM/init/save/load/reconcile/addWithSonicId | `fpmsyncd/nhgmgr.cpp:1784-2312`，声明在 `nhgmgr.h:976-1013,1038-1048` |
| `computeFvHash` / `parseNHGFromRawMsg`（file-static） | `fpmsyncd/nhgmgr.cpp:195-204` / 约 `100-188` |
| `topologicalSort` | `fpmsyncd/nhgmgr.cpp:214-280`（含 Kahn BFS 与环告警尾部） |
| raw buffer / bufferNHGRaw / bufferRouteRaw / isNhgWarmRestartInProgress | `fpmsyncd/routesync.h:266-268,316-318` 及 routesync.cpp 对应实现 |
| `replayBufferedRoutes` | `fpmsyncd/routesync.cpp:3949-4010` 左右（else 分支尾部一并搬） |
| FpmLink 拦截块 | `fpmsyncd/fpmlink.cpp:279-294` |
| fpmsyncd.cpp 调用点 | `fpmsyncd/fpmsyncd.cpp:227,230-232,263` |
| `RouteSync::onWarmStartEnd` | `fpmsyncd/routesync.cpp:3765-3792` |
| 现有 WarmRestart_* 测试（13 个） | `tests/mock_tests/fpmsyncd/nhgmgr_ut.cpp:2585-2929` |

---

### Task 1: NhgWarmRestartAssist 骨架 + 构建接入 + RouteSync 持有实例

**Files:**
- Create: `fpmsyncd/nhgWarmRestartAssist.h`
- Create: `fpmsyncd/nhgWarmRestartAssist.cpp`
- Create: `tests/mock_tests/fpmsyncd/nhgWarmRestartAssist_ut.cpp`
- Modify: `fpmsyncd/Makefile.am:11`
- Modify: `tests/mock_tests/Makefile.am:312-330`
- Modify: `fpmsyncd/routesync.h`（成员 + getter，约 270 行处）
- Modify: `fpmsyncd/routesync.cpp:177-199`（构造函数初始化列表）

- [ ] **Step 1: 写失败的 FSM 测试**

新建 `tests/mock_tests/fpmsyncd/nhgWarmRestartAssist_ut.cpp`：

```cpp
#include "ut_helpers_fpmsyncd.h"
#include "gtest/gtest.h"
#include "mock_table.h"
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/nexthop.h>

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
}
```

- [ ] **Step 2: 构建验证失败**

`make -C tests/mock_tests tests_fpmsyncd -j$(nproc)` 应报 `nhgWarmRestartAssist.h: No such file or directory`。

- [ ] **Step 3: 创建头文件 `fpmsyncd/nhgWarmRestartAssist.h`**

```cpp
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
```

- [ ] **Step 4: 创建实现骨架 `fpmsyncd/nhgWarmRestartAssist.cpp`**

只实现构造 / `inProgress` / `initWarmStart`；其余方法先给空实现（后续 Task 填充，空实现加 `SWSS_LOG_ERROR("not implemented")` 防误调）：

```cpp
#include "nhgWarmRestartAssist.h"
#include "logger.h"

using namespace swss;

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
```

- [ ] **Step 5: 接入构建**

`fpmsyncd/Makefile.am:11` 的 `fpmsyncd_SOURCES` 增加 `nhgWarmRestartAssist.cpp`（加在 `nhgmgr.cpp` 之后）。

`tests/mock_tests/Makefile.am` 的 `tests_fpmsyncd_SOURCES` 增加两行：
```
                         fpmsyncd/nhgWarmRestartAssist_ut.cpp \
                         $(top_srcdir)/fpmsyncd/nhgWarmRestartAssist.cpp \
                         $(top_srcdir)/warmrestart/warmRestartAssist.cpp \
```
（`warmRestartAssist.cpp` 提供 `AppRestartAssist` 实现；`WarmStart` 静态方法来自 `-lswsscommon`，已链接。）

- [ ] **Step 6: RouteSync 持有实例**

`fpmsyncd/routesync.h`：
- 顶部 `#include "nhgWarmRestartAssist.h"`（放在 `warmRestartHelper.h` include 旁）
- `getNHGMgr()` 旁（约 270 行）新增：
```cpp
    NhgWarmRestartAssist& getNhgWarmAssist() { return m_nhgWarmAssist; }
```
- private 成员区，`NHGMgr m_rib_fib_nhg_mgr;`（302 行）**之后**新增（初始化顺序依赖 NHGMgr）：
```cpp
    /* NHG warm restart assist（依赖 m_rib_fib_nhg_mgr，必须在其后声明） */
    NhgWarmRestartAssist m_nhgWarmAssist;
```

`fpmsyncd/routesync.cpp` 构造函数初始化列表，在 `m_nhgFullStateTable(...)` 之后追加：
```cpp
    , m_nhgWarmAssist(pipeline, &m_rib_fib_nhg_mgr)
```
（注意保持初始化列表与头文件声明顺序一致，避免 -Werror=reorder。）

- [ ] **Step 7: 构建并跑新测试**

`make -C tests/mock_tests tests_fpmsyncd -j$(nproc) && ./tests/mock_tests/tests_fpmsyncd --gtest_filter='*WarmAssist*:*WarmRestart_Init*:*WarmRestart_None*'`
预期 3 个新测试全过；`nhgmgr_ut.cpp` 中旧的 `WarmRestart_Init*`/`WarmRestart_None*` 此时仍走 NHGMgr 旧实现，也仍应通过（旧代码未删）。

- [ ] **Step 8: Commit**

`/alinos.commit` — 建议消息：`fpmsyncd: add NhgWarmRestartAssist skeleton with FSM and build wiring`

---

### Task 2: NHGMgr 最小接口 + saveState 搬入

**Files:**
- Modify: `fpmsyncd/nhgmgr.h`（约 965-975 行 public 区加 getter；1013 行附近删 `saveWarmRestartState` 声明）
- Modify: `fpmsyncd/nhgmgr.cpp`（删 1799-1852）
- Modify: `fpmsyncd/nhgWarmRestartAssist.cpp`
- Modify: `fpmsyncd/fpmsyncd.cpp:263`
- Modify: `tests/mock_tests/fpmsyncd/nhgWarmRestartAssist_ut.cpp`（迁入 3 个 Save 测试）
- Modify: `tests/mock_tests/fpmsyncd/nhgmgr_ut.cpp:2616-2691`（删 3 个 Save 测试）

- [ ] **Step 1: 迁移测试（先红）**

把 `nhgmgr_ut.cpp` 的 `WarmRestart_SaveSingleHopSkipped` / `WarmRestart_SaveMultiHopPersisted` / `WarmRestart_SaveIDAllocator`（2616-2691 行）剪切到 `nhgWarmRestartAssist_ut.cpp`，做如下替换：
- `m_nhgmgr->saveWarmRestartState(*m_stateTable)` → `m_assist->saveState(*m_stateTable)`
- 测试体内的 NHG 构造/断言（`addNHGFull`、`m_stateTable->hget(...)` 等）保持不变
- 从 `nhgmgr_ut.cpp` 删除这 3 个测试

构建：报 `NhgWarmRestartAssist::saveState is not implemented` 链接或运行失败（红）。

- [ ] **Step 2: NHGMgr 暴露最小 getter**

`nhgmgr.h` public 区（`addNHGFullWithSonicId` 声明旁）新增：

```cpp
    /* Accessors for NhgWarmRestartAssist (data path only) */
    RIBNHGTable *getRIBNHGTable() { return m_rib_nhg_table; }
    SonicIDMgr &getSonicIDMgr() { return m_sonic_id_manager; }
```

- [ ] **Step 3: 搬 saveState 实现**

将 `nhgmgr.cpp:1799-1852`（`saveWarmRestartState` 函数体）逐行移入 `NhgWarmRestartAssist::saveState`，替换访问路径：
- `m_rib_nhg_table->getNhgMap()` → `m_nhgMgr->getRIBNHGTable()->getNhgMap()`
- `m_sonic_id_manager.getNextNhgID()` → `m_nhgMgr->getSonicIDMgr().getNextNhgID()`
- `m_sonic_id_manager.getNextPicID()` → `m_nhgMgr->getSonicIDMgr().getNextPicID()`

从 `nhgmgr.cpp` 删除原函数，从 `nhgmgr.h` 删除 `saveWarmRestartState` 声明。

- [ ] **Step 4: 切换 fpmsyncd.cpp 调用点**

`fpmsyncd/fpmsyncd.cpp:263`：
```cpp
sync.getNHGMgr().saveWarmRestartState(sync.getNhgFullStateTable());
```
改为：
```cpp
sync.getNhgWarmAssist().saveState(sync.getNhgFullStateTable());
```

- [ ] **Step 5: 构建 + 测试**

`make -C tests/mock_tests tests_fpmsyncd -j$(nproc) && ./tests/mock_tests/tests_fpmsyncd --gtest_filter='*WarmRestart_Save*'`
3 个 Save 测试通过；另跑 `make -C fpmsyncd fpmsyncd -j$(nproc)` 确认主程序编译。

- [ ] **Step 6: Commit**

`/alinos.commit` — 建议消息：`fpmsyncd: move NHG warm restart saveState into NhgWarmRestartAssist`

---

### Task 3: loadState 搬入 + initWarmStart 切换

**Files:**
- Modify: `fpmsyncd/nhgmgr.h`（删 `initWarmRestart`/`loadWarmRestartState` 声明）
- Modify: `fpmsyncd/nhgmgr.cpp`（删 1788-1797、1854-2024）
- Modify: `fpmsyncd/nhgWarmRestartAssist.cpp`
- Modify: `fpmsyncd/fpmsyncd.cpp:227,230-232`
- Modify: `tests/mock_tests/fpmsyncd/nhgWarmRestartAssist_ut.cpp`（迁入 3 个 Load 测试）
- Modify: `tests/mock_tests/fpmsyncd/nhgmgr_ut.cpp:2693-2767`（删 3 个 Load 测试及旧 Init 测试 2585-2614）

- [ ] **Step 1: 迁移测试（先红）**

剪切 `nhgmgr_ut.cpp` 的 `WarmRestart_LoadRestoresState` / `WarmRestart_LoadRestoresAllocator` / `WarmRestart_LoadCorruptedDataGraceful`（2693-2767 行）到新测试文件，替换：
- `m_nhgmgr->initWarmRestart()` → `m_assist->initWarmStart()`
- `m_nhgmgr->loadWarmRestartState(*m_stateTable, *m_nextHopTable)` → `m_assist->loadState(*m_stateTable, *m_nextHopTable)`
- fixture 访问器 `getSavedNhgInfos()/getAppDbNhgFvs()` 已在新 fixture 中存在，直接可用

同时删除 `nhgmgr_ut.cpp` 中已被新文件覆盖的 `WarmRestart_InitSetsState` / `WarmRestart_NoneAndReconciledNotInProgress` / `WarmRestart_InitClearsMaps`（2585-2614 行）及 fixture 中 `getWrState/setWrState/getSavedNhgInfos/getAppDbNhgFvs/getReconciledIds` 五个访问器（119-138 行）。

- [ ] **Step 2: 搬 loadState 实现**

将 `nhgmgr.cpp:1854-2024`（`loadWarmRestartState` 全文，含 Phase 1 allocator 恢复 / Phase 2 state 表加载 / Phase 3 APP_DB FV 加载 / Phase 4 FSM→RESTORED）逐行移入 `NhgWarmRestartAssist::loadState`，替换：
- `m_sonic_id_manager.setNextNhgID(...)` → `m_nhgMgr->getSonicIDMgr().setNextNhgID(...)`
- `m_sonic_id_manager.setNextPicID(...)` → `m_nhgMgr->getSonicIDMgr().setNextPicID(...)`
- `m_sonic_id_manager.markNhgIDUsed(...)` → `m_nhgMgr->getSonicIDMgr().markNhgIDUsed(...)`
- `m_sonic_id_manager.markPicIDUsed(...)` → `m_nhgMgr->getSonicIDMgr().markPicIDUsed(...)`
- `m_nhgWrState = NHG_WR_RESTORED` → `m_state = NHG_WR_RESTORED`
- `m_saved_nhg_infos` / `m_appdb_nhg_fvs` 直接使用（已为本类成员）

从 nhgmgr.cpp/h 删除 `loadWarmRestartState` 与 `initWarmRestart`。

- [ ] **Step 3: 切换 fpmsyncd.cpp 调用点**

`fpmsyncd/fpmsyncd.cpp:227,230-232`：
```cpp
sync.getNHGMgr().initWarmRestart();

swss::Table appDbNhgTable(&db, APP_NEXTHOP_GROUP_TABLE_NAME);
sync.getNHGMgr().loadWarmRestartState(
    sync.getNhgFullStateTable(),
    appDbNhgTable);
```
改为：
```cpp
sync.getNhgWarmAssist().initWarmStart();

swss::Table appDbNhgTable(&db, APP_NEXTHOP_GROUP_TABLE_NAME);
sync.getNhgWarmAssist().loadState(
    sync.getNhgFullStateTable(),
    appDbNhgTable);
```

- [ ] **Step 4: 构建 + 测试**

`make -C tests/mock_tests tests_fpmsyncd -j$(nproc) && ./tests/mock_tests/tests_fpmsyncd --gtest_filter='*WarmRestart_Load*:*WarmRestart_Init*:*WarmRestart_None*'`（应全过）+ `make -C fpmsyncd fpmsyncd -j$(nproc)`。

- [ ] **Step 5: Commit**

`/alinos.commit` — 建议消息：`fpmsyncd: move NHG warm restart loadState/init into NhgWarmRestartAssist`

---

### Task 4: reconcile 逻辑搬入（Phase1/Phase2 + helpers）并切换 onWarmStartEnd

**Files:**
- Modify: `fpmsyncd/nhgmgr.h`（删 FSM enum、3 个 struct、`isNhgWarmRestartInProgress`、两个 reconcile 声明、`findMatchingAppDbEntry`、`topologicalSort`、成员 `m_nhgWrState/m_saved_nhg_infos/m_appdb_nhg_fvs/m_reconciled_ids`，即 976-1013、1038-1048 行段；**保留** `addNHGFullWithSonicId`）
- Modify: `fpmsyncd/nhgmgr.cpp`（删 1784-1786 FSM 方法、2034-2234 reconcile 实现、195-204 `computeFvHash`、约 100-188 `parseNHGFromRawMsg`、214-280 `topologicalSort`；**保留** `addNHGFullWithSonicId` 2253-2312）
- Modify: `fpmsyncd/nhgWarmRestartAssist.cpp`
- Modify: `fpmsyncd/routesync.cpp:3765-3780`（onWarmStartEnd NHG 段）
- Modify: `tests/mock_tests/fpmsyncd/nhgWarmRestartAssist_ut.cpp`（迁入 4 个测试）
- Modify: `tests/mock_tests/fpmsyncd/nhgmgr_ut.cpp:2769-2929`（删 Phase1/AddWithSonicId/EndToEnd 测试）

- [ ] **Step 1: 迁移测试（先红）**

剪切 `nhgmgr_ut.cpp` 的 `WarmRestart_Phase1AddsSingleHop` / `WarmRestart_Phase1SkipsMultiHop` / `WarmRestart_AddWithSonicIdReusesId` / `WarmRestart_EndToEnd`（2769-2929 行）到新测试文件，替换：
- `m_nhgmgr->reconcileNormalSingleHopNHGs(nhgBuffer)` → 直接调用 `m_assist->reconcileNHGs()`（公开入口），若测试需要单独跑 Phase 1，则经 fixture 访问器调私有方法：`m_assist->reconcileNormalSingleHopNHGs()`（`#define private public` 已生效），且 NHG buffer 改为先 `m_assist->m_nhg_raw_buffer = nhgBuffer;` 再调用（签名已无参数）
- `m_nhgmgr->reconcileNHGsWithSonicObj(nhgBuffer)` → `m_assist->reconcileNHGsWithSonicObj()`（同上，先灌 buffer）
- `m_nhgmgr->addNHGFullWithSonicId(...)` 调用保持不变（该方法留在 NHGMgr）
- `WarmRestart_EndToEnd` 中 save/load/reconcile 全部改走 `m_assist`，NHG 数据路径（`addNHGFull`/`getRIBNHGEntryByRIBID`）保持 `m_nhgmgr`

- [ ] **Step 2: 搬 file-static helpers**

将 `nhgmgr.cpp` 的 `computeFvHash`（195-204 行）与 `parseNHGFromRawMsg`（约 100-188 行，含 NHA_RTA/NHA_JSON_STR 相关解析与 eth0/docker0 过滤）原样（保持 `static`）移入 `nhgWarmRestartAssist.cpp` 顶部。检查并搬走它们依赖的 include（如 `<net/if.h>`、`<linux/nexthop.h>`、`netlink/utils.h` 等，以 nhgmgr.cpp 头部实际为准）。

- [ ] **Step 3: 搬 topologicalSort 与 findMatchingAppDbEntry**

- `nhgmgr.cpp:214-280` 的 `NHGMgr::topologicalSort` → `NhgWarmRestartAssist::topologicalSort`，签名中 `swss::NHGMgr::TempReconcileEntry` 改为 `TempReconcileEntry`（本类 struct）
- `nhgmgr.cpp:2034-2042` 的 `findMatchingAppDbEntry` 原样搬入（成员名不变）

- [ ] **Step 4: 搬 Phase1/Phase2 + 公开入口 reconcileNHGs**

`reconcileNormalSingleHopNHGs`（nhgmgr.cpp:2054-2086）与 `reconcileNHGsWithSonicObj`（2107-2234）搬入，签名改为无参（buffer 用成员 `m_nhg_raw_buffer`），替换：
- 参数 `nhgBuffer` → `m_nhg_raw_buffer`
- `m_nhgWrState = NHG_WR_RECONCILING/RECONCILED` → `m_state = ...`
- `RIBNHGEntry tmpEntry(m_rib_nhg_table)` → `RIBNHGEntry tmpEntry(m_nhgMgr->getRIBNHGTable())`
- `addNHGFull(...)` / `addNHGFullWithSonicId(...)` → `m_nhgMgr->addNHGFull(...)` / `m_nhgMgr->addNHGFullWithSonicId(...)`
- `m_rib_nhg_table->removeFromDB(...)` → `m_nhgMgr->getRIBNHGTable()->removeFromDB(...)`

新增公开入口：

```cpp
void NhgWarmRestartAssist::reconcileNHGs()
{
    reconcileNormalSingleHopNHGs();
    reconcileNHGsWithSonicObj();
    m_nhg_raw_buffer.clear();
}
```

- [ ] **Step 5: 清理 nhgmgr.h/cpp 中已搬走的内容**

删除 `nhgmgr.h` 的：`NhgWarmRestartState` enum、`SavedNHGInfo`/`AppDbNHGEntry`/`TempReconcileEntry` struct、`isNhgWarmRestartInProgress` 声明、两个 reconcile 声明、`findMatchingAppDbEntry`/`topologicalSort` 声明、4 个 warm restart 成员变量。删除 `nhgmgr.cpp` 的 `isNhgWarmRestartInProgress`（1784-1786）。**保留** `addNHGFullWithSonicId` 与其声明。

- [ ] **Step 6: 切换 onWarmStartEnd**

`fpmsyncd/routesync.cpp:3769-3780`：
```cpp
    if (m_rib_fib_nhg_mgr.isNhgWarmRestartInProgress())
    {
        SWSS_LOG_NOTICE("NHG warm restart: starting reconcile in onWarmStartEnd");
        m_rib_fib_nhg_mgr.reconcileNormalSingleHopNHGs(m_nhg_raw_buffer);
        m_rib_fib_nhg_mgr.reconcileNHGsWithSonicObj(m_nhg_raw_buffer);
        m_nhg_raw_buffer.clear();

        replayBufferedRoutes();
        SWSS_LOG_NOTICE("NHG warm restart: reconcile complete");
    }
```
改为（`replayBufferedRoutes` 暂时还是 RouteSync 旧实现，Task 6 再切）：
```cpp
    if (m_nhgWarmAssist.inProgress())
    {
        SWSS_LOG_NOTICE("NHG warm restart: starting reconcile in onWarmStartEnd");
        m_nhgWarmAssist.reconcileNHGs();

        replayBufferedRoutes();
        SWSS_LOG_NOTICE("NHG warm restart: reconcile complete");
    }
```
同时把 routesync.cpp 中 `RouteSync::isNhgWarmRestartInProgress()` 的定义改为委托（保持 FpmLink 调用点编译通过，Task 5 再删）：
```cpp
bool RouteSync::isNhgWarmRestartInProgress() const
{
    return m_nhgWarmAssist.inProgress();
}
```

- [ ] **Step 7: 构建 + 测试**

`make -C tests/mock_tests tests_fpmsyncd -j$(nproc) && ./tests/mock_tests/tests_fpmsyncd --gtest_filter='*WarmRestart*'`（全部 13+3 个测试应通过）+ `make -C fpmsyncd fpmsyncd -j$(nproc)`。

- [ ] **Step 8: Commit**

`/alinos.commit` — 建议消息：`fpmsyncd: move NHG warm restart reconcile phases into NhgWarmRestartAssist`

---

### Task 5: 消息拦截与 buffer 搬入 + FpmLink 切换

**Files:**
- Modify: `fpmsyncd/nhgWarmRestartAssist.cpp`（实现 shouldIntercept/bufferNHG/bufferRoute）
- Modify: `fpmsyncd/fpmlink.cpp:279-294`
- Modify: `fpmsyncd/routesync.h:266-268,316-318` 与 `fpmsyncd/routesync.cpp`（删 bufferNHGRaw/bufferRouteRaw/isNhgWarmRestartInProgress/两个 buffer 成员）
- Modify: `tests/mock_tests/fpmsyncd/nhgWarmRestartAssist_ut.cpp`（新增 2 个测试）

- [ ] **Step 1: 写新测试（先红）**

`nhgWarmRestartAssist_ut.cpp` 新增：

```cpp
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
```
（RTM_NEWNHGFIB=5000 / RTM_NEWROUTE=46 / RTM_NEWLINK=16 宏按 nhgmgr_ut.cpp 顶部的 ifndef 方式定义。）

- [ ] **Step 2: 实现 shouldIntercept/bufferNHG/bufferRoute**

`nhgWarmRestartAssist.cpp`：

```cpp
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
```
（RTM_NEWNHGFIB/RTM_DELNHGFIB/RTM_NEWSRV6VPNROUTE/RTM_DELSRV6VPNROUTE 的宏定义从 fpmlink.h 取值方式复制到本文件顶部 ifndef 块，与 nhgmgr_ut.cpp:21-27 同一做法。）

- [ ] **Step 3: 切换 FpmLink 拦截块**

`fpmsyncd/fpmlink.cpp:279-294` 替换为：

```cpp
        /* Warm restart: intercept NHG and route messages into raw buffers */
        auto &warmAssist = m_routesync->getNhgWarmAssist();
        if (warmAssist.shouldIntercept(nl_hdr))
        {
            uint16_t type = nl_hdr->nlmsg_type;
            if (type == RTM_NEWNHGFIB || type == RTM_DELNHGFIB)
            {
                warmAssist.bufferNHG(nl_hdr);
            }
            else
            {
                warmAssist.bufferRoute(nl_hdr);
            }
            continue;
        }
```

- [ ] **Step 4: 删除 RouteSync 旧 buffer 设施**

- `routesync.h`：删 `bufferNHGRaw`/`bufferRouteRaw`/`isNhgWarmRestartInProgress` 声明（266-268 行）与 `m_nhg_raw_buffer`/`m_route_raw_buffer` 成员（316-318 行）
- `routesync.cpp`：删 `bufferNHGRaw`/`bufferRouteRaw`/`isNhgWarmRestartInProgress` 三个实现

- [ ] **Step 5: 构建 + 测试**

`make -C tests/mock_tests tests_fpmsyncd -j$(nproc) && ./tests/mock_tests/tests_fpmsyncd --gtest_filter='*WarmRestart*'` + `make -C fpmsyncd fpmsyncd -j$(nproc)`。

- [ ] **Step 6: Commit**

`/alinos.commit` — 建议消息：`fpmsyncd: move warm restart message interception into NhgWarmRestartAssist`

---

### Task 6: 路由回放搬入

**Files:**
- Modify: `fpmsyncd/routesync.h`（删 `replayBufferedRoutes` 声明 269 行；public 区加 `dispatchBufferedRouteMsg`）
- Modify: `fpmsyncd/routesync.cpp`（删 3949-4010 的 `replayBufferedRoutes`，新增 `dispatchBufferedRouteMsg`；onWarmStartEnd 切换）
- Modify: `fpmsyncd/nhgWarmRestartAssist.cpp`（实现 replayBufferedRoutes）

- [ ] **Step 1: RouteSync 新增 dispatchBufferedRouteMsg**

将 `replayBufferedRoutes`（routesync.cpp:3949-4010）循环体中对**单条消息**的 dispatch 逻辑原样提取为 public 方法（含 SRv6VPN 直走 onMsgRaw、encap_type>0 走 onMsgRaw、否则 nlmsg_convert + NetDispatcher、以及尾部 else 的 WARN 分支）：

```cpp
/* routesync.h public 区 */
void dispatchBufferedRouteMsg(struct nlmsghdr *nlh);
```

```cpp
/* routesync.cpp */
void RouteSync::dispatchBufferedRouteMsg(struct nlmsghdr *nlh)
{
    if (nlh->nlmsg_type == RTM_NEWSRV6VPNROUTE ||
        nlh->nlmsg_type == RTM_DELSRV6VPNROUTE)
    {
        /* SRv6 VPN routes always go through raw processing */
        onMsgRaw(nlh);
    }
    else if (nlh->nlmsg_type == RTM_NEWROUTE ||
             nlh->nlmsg_type == RTM_DELROUTE)
    {
        uint16_t encap_type = getEncapType(nlh);
        if (encap_type > 0)
        {
            onMsgRaw(nlh);
        }
        else
        {
            nl_msg *msg = nlmsg_convert(nlh);
            if (msg)
            {
                nlmsg_set_proto(msg, NETLINK_ROUTE);
                NetDispatcher::getInstance().onNetlinkMessage(msg);
                nlmsg_free(msg);
            }
            else
            {
                SWSS_LOG_ERROR("NHG warm restart: failed to convert route nlmsg, type=%d",
                               nlh->nlmsg_type);
            }
        }
    }
    else
    {
        /* 保留原 replayBufferedRoutes 尾部 else 分支的日志语义 */
        SWSS_LOG_WARN("NHG warm restart: unexpected buffered message type %d, skipped",
                      nlh->nlmsg_type);
    }
}
```

- [ ] **Step 2: assist 实现 replayBufferedRoutes 并删除 RouteSync 旧实现**

`nhgWarmRestartAssist.cpp`（顶部 `#include "routesync.h"`，头文件中已前向声明 `class RouteSync`，无循环包含）：

```cpp
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
}
```

从 routesync.h/cpp 删除 `replayBufferedRoutes`（声明 269 行与实现 3949-4010 行）。

- [ ] **Step 3: 切换 onWarmStartEnd 回放调用**

`routesync.cpp` onWarmStartEnd 中（Task 4 已改过的块）：
```cpp
        replayBufferedRoutes();
```
改为：
```cpp
        m_nhgWarmAssist.replayBufferedRoutes(*this);
```

- [ ] **Step 4: 构建 + 测试**

`make -C tests/mock_tests tests_fpmsyncd -j$(nproc) && ./tests/mock_tests/tests_fpmsyncd --gtest_filter='*WarmRestart*'` + `make -C fpmsyncd fpmsyncd -j$(nproc)`。

- [ ] **Step 5: Commit**

`/alinos.commit` — 建议消息：`fpmsyncd: move buffered route replay into NhgWarmRestartAssist`

---

### Task 7: RECONCILED 状态上报 + swss-common 行为验证 + 全量收尾

**Files:**
- Modify: `fpmsyncd/nhgWarmRestartAssist.cpp`（reconcile 完成时上报）
- Modify: `fpmsyncd/nhgmgr.h/.cpp`（清理残留 include/friend 注释）

- [ ] **Step 1: 验证 swss-common WarmStart 行为（spec 待验证点）**

在构建环境中阅读 swss-common `common/warm_restart.cpp`：
- 确认 `WarmStart::initialize("fpmsyncd-nhg", "bgp")` 对 `WARM_RESTART_ENABLE_TABLE` 的写入行为（是否创建默认 disabled 条目）
- 确认 `checkWarmStart` 在无配置时返回 false，且不影响子类自有 FSM（本设计不依赖基类 `m_warmStartInProgress`，已隔离）
- 确认 host warm-restart finalizer 是否遍历 `WARM_RESTART_TABLE` 等待新条目；若等待，Step 2 的上报必须落地

- [ ] **Step 2: RECONCILED 状态上报**

`reconcileNHGsWithSonicObj()` 尾部 `m_state = NHG_WR_RECONCILED;` 之后追加：

```cpp
    /* 向 STATE_DB 上报完成，避免 warm-restart finalizer 等待 fpmsyncd-nhg 条目 */
    WarmStart::setWarmStartState("fpmsyncd-nhg", WarmStart::RECONCILED);
```
（`warm_restart.h` 已被 warmRestartAssist.cpp include；本文件需 `#include "warm_restart.h"`。）

- [ ] **Step 3: 残留清理**

- `nhgmgr.h`：确认 warm restart 相关注释/声明无残留；`ut_fpmsyncd::FpmSyncdNhgMgr` friend 声明保留（其他测试仍用）
- `nhgmgr.cpp`：确认已无 `NHG_WR_` 引用；检查 `<linux/nexthop.h>` 等仅被搬走的 helper 使用的 include，若无其他使用者则删除
- `routesync.h`：确认 `isNhgWarmRestartInProgress` 声明已删、无 `m_nhg_raw_buffer` 残留

- [ ] **Step 4: 全量验证**

```bash
make -C fpmsyncd fpmsyncd -j$(nproc)
make -C tests/mock_tests tests_fpmsyncd -j$(nproc)
./tests/mock_tests/tests_fpmsyncd          # 全量，不只 WarmRestart
```
预期：编译零告警（-Werror 下零错误）、tests_fpmsyncd 全部通过。

- [ ] **Step 5: Commit**

`/alinos.commit` — 建议消息：`fpmsyncd: report fpmsyncd-nhg warm restart state and clean up legacy code`

---

## Self-Review

**Spec coverage：**
- 新类 + 文件 → Task 1；继承外壳（构造/timer/FSM 自有）→ Task 1 Step 3/4
- saveState → Task 2；loadState/init → Task 3；reconcile Phase1/2/helpers → Task 4
- shouldIntercept/buffer → Task 5；replayBufferedRoutes → Task 6
- RouteSync 持有/接口暴露 → Task 1 Step 6 / Task 2 Step 2
- fpmsyncd.cpp 三处调用点 → Task 2/3；FpmLink → Task 5；onWarmStartEnd → Task 4/6
- fpmsyncd-nhg 待验证点 + RECONCILED 上报 → Task 7
- 测试策略（旧用例换被测对象 + 新增 shouldIntercept 用例）→ 各 Task Step 1、Task 5 Step 1
- Makefile（fpmsyncd + mock_tests）→ Task 1 Step 5

**Placeholder scan：** 无 TBD/TODO；Task 7 Step 1 是 spec 明确标注的验证项，有具体查证路径与后续动作。

**Type consistency：** `NhgWarmRestartAssist`（类名）/ `getNhgWarmAssist()` / `saveState/loadState/initWarmStart/reconcileNHGs/replayBufferedRoutes/shouldIntercept/bufferNHG/bufferRoute` / `getRIBNHGTable()/getSonicIDMgr()` / `dispatchBufferedRouteMsg` 在 Task 间引用一致；struct 从 `NHGMgr::Xxx` 变为 `NhgWarmRestartAssist::Xxx` 已在 Task 4 明确。
