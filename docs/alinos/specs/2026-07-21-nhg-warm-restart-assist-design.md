# Design: NHG Warm Restart 重构 — NhgWarmRestartAssist

## Overview

当前 fpmsyncd 的 NHG warm reboot 实现（见 `2026-07-15-rib-fib-warm-reboot-design.md`）功能正确，但 warm restart 逻辑分散在 NHGMgr（save/load/reconcile/FSM）、FpmLink（消息拦截）、RouteSync（raw buffer + 驱动）三处，且自造一套 FSM，与 SONiC 既有的 `AppRestartAssist` 框架（`warmrestart/warmRestartAssist.h`）不一致。

本设计进行**纯结构重构（行为不变）**：新建 `NhgWarmRestartAssist` 类，继承 `AppRestartAssist`（仅复用外壳），把 NHG warm restart 的全部逻辑（状态持久化、加载、消息拦截缓存、reconcile、路由回放）集中到独立文件中。NHGMgr 回归纯数据路径。

### 已确认的决策

| 决策点 | 结论 |
|--------|------|
| 问题性质 | 纯结构问题 + 与现有框架不一致，无功能性 bug |
| 继承方式 | 继承但**仅复用外壳**：基类构造（WarmStart 注册）、timer、`isWarmStartInProgress`；数据路径方法全部子类新增，**不改基类、不加 virtual** |
| 职责范围 | save/load/reconcile/FSM/raw buffer/拦截判断**全部搬入**新类；FpmLink/RouteSync 只留一行调用 |
| 实例归属 | 方案 A：RouteSync 持有 Assist，Assist 反持 NHGMgr 指针（不拥有） |
| STATE_DB 条目 | 独立 appName `fpmsyncd-nhg`（dockerName `bgp`），见"待验证点" |

## Architecture

### 组件关系

```
┌──────────────┐   owns   ┌─────────────────────────┐  uses   ┌─────────────┐
│  RouteSync   │─────────>│  NhgWarmRestartAssist   │────────>│   NHGMgr    │
│ (warm FSM    │          │  : AppRestartAssist     │ (数据路径)│ (addNHGFull/ │
│  驱动者)      │          │  save/load/buffer/      │         │  addNHGFull- │
└──────┬───────┘          │  reconcile/replay       │         │  WithSonicId)│
       │                  └─────────────────────────┘         └─────────────┘
       │ 回调/转发                       ▲
┌──────┴───────┐                        │ shouldIntercept/bufferXxx
│   FpmLink    │────────────────────────┘
│ (消息入口)    │
└──────────────┘
```

- `NhgWarmRestartAssist` 只依赖 NHGMgr，不 include `fpmlink.h`，避免循环依赖
- 路由回放需要 dispatch 能力（`onMsgRaw`/`NetDispatcher`），实现时由 RouteSync 提供私有 dispatch 辅助，assist 通过 RouteSync 调用（具体形式以实现时不引入循环依赖为准）

### 新增文件

`fpmsyncd/nhgWarmRestartAssist.h` / `fpmsyncd/nhgWarmRestartAssist.cpp`，加入 `Makefile.am`。

### 类定义（骨架）

```cpp
class NhgWarmRestartAssist : public AppRestartAssist {
public:
    NhgWarmRestartAssist(RedisPipeline *pipeline, NHGMgr *nhgMgr);
    // 基类构造参数：appName = "fpmsyncd-nhg", dockerName = "bgp"

    // ---- FSM（从 NHGMgr 搬入）----
    enum NhgWarmRestartState {
        NHG_WR_NONE, NHG_WR_INITIALIZED, NHG_WR_RESTORED,
        NHG_WR_RECONCILING, NHG_WR_RECONCILED
    };
    bool inProgress() const;                 // 原 isNhgWarmRestartInProgress()

    // ---- Shutdown 路径（fpmsyncd.cpp 调用）----
    void saveState(Table &stateTable);       // 原 saveWarmRestartState()

    // ---- Startup 路径（fpmsyncd.cpp 调用）----
    void initWarmStart();                    // 原 initWarmRestart()
    void loadState(Table &stateTable,
                   Table &appDbNhgTable);    // 原 loadWarmRestartState()

    // ---- 消息拦截 + 缓存（FpmLink 调用，经 RouteSync 转发）----
    bool shouldIntercept(const nlmsghdr *nlh) const;
    void bufferNHG(const nlmsghdr *nlh);
    void bufferRoute(const nlmsghdr *nlh);

    // ---- Warm end reconcile（RouteSync::onWarmStartEnd 调用）----
    void reconcileNHGs();                    // Phase1+Phase2，内部清空 NHG buffer
    void replayBufferedRoutes();             // 原 RouteSync::replayBufferedRoutes()

private:
    NHGMgr *m_nhgMgr;                        // 数据路径，不持有所有权
    NhgWarmRestartState m_state = NHG_WR_NONE;
    std::vector<std::vector<uint8_t>> m_nhg_raw_buffer;
    std::vector<std::vector<uint8_t>> m_route_raw_buffer;

    // 从 NHGMgr 搬入的 warm restart 数据结构：
    // SavedNHGInfo / AppDbNHGEntry / TempReconcileEntry 移到本头文件
    std::map<sonicObjectID, SavedNHGInfo> m_saved_nhg_infos;
    std::map<std::string, AppDbNHGEntry> m_appdb_nhg_fvs;
    std::set<ribID> m_reconciled_ids;

    // 从 NHGMgr 搬入的内部步骤：
    void reconcileNormalSingleHopNHGs();
    void reconcileNHGsWithSonicObj();
    AppDbNHGEntry *findMatchingAppDbEntry(const std::string &fvHash);
    static std::vector<ribID> topologicalSort(
        const std::map<ribID, TempReconcileEntry> &entries);
};
```

### 各现有文件的变化

| 文件 | 变化 |
|------|------|
| `fpmsyncd/nhgmgr.h/.cpp` | 删除全部 warm restart 成员/方法（FSM、`saveWarmRestartState`、`loadWarmRestartState`、`reconcileNormalSingleHopNHGs`、`reconcileNHGsWithSonicObj`、`findMatchingAppDbEntry`、`topologicalSort`、`m_saved_nhg_infos`、`m_appdb_nhg_fvs`、`m_reconciled_ids`）；`addNHGFullWithSonicId()` 保留（属数据路径）；按上表暴露最小接口 |
| `fpmsyncd/routesync.h/.cpp` | 删除 `m_nhg_raw_buffer`/`m_route_raw_buffer`/`bufferNHGRaw`/`bufferRouteRaw`/`replayBufferedRoutes`；新增成员 `NhgWarmRestartAssist m_nhgWarmAssist` 及 `getNhgWarmAssist()`；`onWarmStartEnd()` 改为调用 assist 两行 |
| `fpmsyncd/fpmlink.cpp` | 拦截块改为经 `getNhgWarmAssist()` 判断和缓存 |
| `fpmsyncd/fpmsyncd.cpp` | `initWarmRestart`/`loadWarmRestartState`/`saveWarmRestartState` 三处调用改为走 assist（fpmsyncd.cpp:227,230,263） |
| `fpmsyncd/Makefile.am` | 加入新源文件 |

### NHGMgr 暴露给 Assist 的接口（最小集）

| 接口 | 用途 |
|------|------|
| `getRIBNHGTable()`（或现有 `getNhgMap()` 通路） | `saveState` 遍历 entry |
| `getSonicIDMgr()` | save/load ID allocator 状态 |
| `addNHGFull(nhg, af)` | Phase 1/2 新增路径（已 public） |
| `addNHGFullWithSonicId(nhg, af, nhgId, picId)` | Phase 2 复用 sonicID 路径（已 public） |
| `delNHGFull(id)` / APP_DB 删除通路 | 过期表项清理 |

## 数据流

### 阶段 1：Graceful Shutdown

```
fpmsyncd.cpp 收到 shutdown 信号
  if (warmStartEnabled && nhgFibEnabled):
    sync.getNhgWarmAssist().saveState(stateTable)
      → 遍历 NHGMgr RIBNHGTable 中有 Sonic 对象的 entry
      → 写 NHG_FULL_STATE_TABLE（sonic_nhg_id/af/json/pic_context_id）
      → 写 NHG_ID_ALLOCATOR（next_nhg_id/next_pic_id）
```

### 阶段 2：Warm Start 启动 + 消息拦截

```
fpmsyncd.cpp:
  warmStartEnabled = warmStartHelper.checkAndStart()   // 路由通道，不变
  if (warmStartEnabled && nhgFibEnabled):
    assist.initWarmStart()                              // FSM → INITIALIZED
    assist.loadState(stateTable, appDbNhgTable)         // FSM → RESTORED
      → 恢复 ID allocator、构建 m_saved_nhg_infos、m_appdb_nhg_fvs

FpmLink::processFpmMessage()（每条消息）:
  if (assist.shouldIntercept(nl_hdr)):   // = FSM in-progress && 类型 ∈ {NHG, 路由}
    RTM_NEW/DELNHGFIB       → assist.bufferNHG(nl_hdr)
    RTM_NEW/DELROUTE 等路由 → assist.bufferRoute(nl_hdr)
    continue
  其他类型（或 FSM 非 in-progress）→ 走原有 dispatch，不缓存
```

`shouldIntercept(nlh)` 内部 = `inProgress() && (类型 ∈ {RTM_NEW/DELNHGFIB, RTM_NEW/DELROUTE, RTM_NEW/DELSRV6VPNROUTE})`。类型集合判断从 FpmLink 搬入 assist；FpmLink 只保留区分 NHG/路由的 switch 以调用对应 buffer。

### 阶段 3：Warm End（onWarmStartEnd）

```
RouteSync::onWarmStartEnd():
  if (assist.inProgress()):
    assist.reconcileNHGs()          // FSM → RECONCILING
      ├─ reconcileNormalSingleHopNHGs()   // Phase 1：普通单跳直接 addNHGFull
      ├─ reconcileNHGsWithSonicObj()      // Phase 2：干跑 FV 匹配 → 复用 sonicID
      │     匹配   → m_nhgMgr->addNHGFullWithSonicId(...)
      │     未匹配 → m_nhgMgr->addNHGFull(...)
      │     过期 APP_DB 表项 → 删除
      ├─ m_nhg_raw_buffer.clear()
      └─ FSM → RECONCILED
    assist.replayBufferedRoutes()   // 回放路由 → 走 RouteSync dispatch 路径
  // 之后路由 reconcile（m_warmStartHelper.reconcile()）不变
```

## 错误处理

沿用现有语义，不改变行为：

| 场景 | 处理 |
|------|------|
| `loadState` 中 allocator 值解析失败 | WARN 日志 + 用默认值继续（现状） |
| state table 缺 `NHG_ID_ALLOCATOR` 键 | WARN 日志，allocator 从 1 开始（现状） |
| Phase 2 FV 匹配失败 | 走新增路径（新分配 sonicID 写 APP_DB），不视为错误 |
| 拓扑排序检测到环 | 跳过并告警（现状） |
| `nlmsg_convert` 失败（路由回放） | 错误日志 + 跳过该条，不影响其他路由（现状） |
| `m_nhgMgr` 为 nullptr | 构造时断言/CHECK（编程错误，非运行时恢复场景） |
| 基类构造抛 `invalid_argument`（timer 值非法） | 子类不传自定义 timer，用基类默认 5s，不会触发 |

## `fpmsyncd-nhg` 独立条目 — 待验证点

实现阶段需读 swss-common `WarmStart::initialize/checkWarmStart` 源码确认：

1. `checkWarmStart("fpmsyncd-nhg", "bgp")` 在无 `WARM_RESTART_ENABLE_TABLE` 配置时的行为。预期返回 false → 基类 `m_warmStartInProgress = false`。**不依赖基类的 in-progress 标志**，统一用子类自有 FSM（`inProgress()`）判断
2. 新增的 `WARM_RESTART_TABLE|fpmsyncd-nhg` 条目是否会被 host warm-restart finalizer 等待。兜底措施：子类 FSM 到 RECONCILED 时调用 `WarmStart::setWarmStartState("fpmsyncd-nhg", WarmStart::RECONCILED)`；若 finalizer 存在兼容性问题且无法解决，回退方案是复用 "bgp" 条目（构造传 `("bgp", "bgp")`）

warm 检测仍走路由通道（`warmStartHelper.checkAndStart()`），与现状一致。

## 测试策略

| 层级 | 内容 |
|------|------|
| 单元测试（`tests/.../ut_fpmsyncd`） | 现有 NHG warm reboot UT 全部改为测 `NhgWarmRestartAssist`：save/load 往返、ID allocator 恢复、FV 匹配复用、拓扑排序、过期表项删除。**测试用例逻辑不变，只换被测对象**——作为"行为不变"的验证手段 |
| E2E（现有 NHG warm restart E2E） | 用例不改动，重构后必须通过，作为行为等价的最终验证 |
| 新增 UT（少量） | `shouldIntercept()` 消息类型分类；FSM 非 in-progress 时 buffer 不累积 |

## 风险

| 风险 | 可能性 | 缓解 |
|------|--------|------|
| 重构引入行为差异 | Low | 算法代码逐行搬运；现有 UT/E2E 不改用例只改调用方 |
| 循环依赖（assist ↔ routesync/fpmlink） | Low | assist 只依赖 NHGMgr；回放经 RouteSync dispatch，不反向 include fpmlink.h |
| `fpmsyncd-nhg` 新 STATE_DB 条目影响 finalizer | Medium | 见"待验证点"；最坏情况回退复用 "bgp" 条目 |

## Key Files

| 文件 | 变更 |
|------|------|
| `fpmsyncd/nhgWarmRestartAssist.h/.cpp` | **新增**，承载全部 NHG warm restart 逻辑 |
| `fpmsyncd/nhgmgr.h/.cpp` | 删除 warm restart 成员/方法；暴露最小数据接口 |
| `fpmsyncd/routesync.h/.cpp` | 持有 assist 实例；删除 raw buffer 与回放逻辑 |
| `fpmsyncd/fpmlink.cpp` | 拦截改为单行调用 assist |
| `fpmsyncd/fpmsyncd.cpp` | 三处调用改走 assist |
| `fpmsyncd/Makefile.am` | 加入新源文件 |
| `tests/`（ut_fpmsyncd 相关） | 被测对象从 NHGMgr 换成 NhgWarmRestartAssist |

## GBrain 参考

无相关历史记录（本会话 GBrain MCP 未连接，未能执行历史查询）。
