# NHG Warm Restart Assist 重构 Summary

日期：2026-07-23
分支：ribfib_2_wb_ai（base: master）
提交：6cf0141a `fpmsyncd: refactor NHG warm restart into NhgWarmRestartAssist`

## 设计概要

### 动机与目标

原 NHG warm reboot 逻辑散布在 `NHGMgr`（save/load/reconcile）与 `RouteSync`（消息拦截 buffer、路由回放）中，与 SONiC 现有 warm restart 框架（`AppRestartAssist`）不一致。本次为**纯结构重构（行为不变）**：将 NHG warm restart 全部逻辑集中到新类 `NhgWarmRestartAssist`。

### 关键设计决策

1. **继承但仅复用外壳**：`NhgWarmRestartAssist` 继承 `AppRestartAssist`，只复用其构造（WarmStart 注册、reconcile timer），不依赖基类的数据路径方法与 `m_warmStartInProgress`；子类自维护 FSM。基类无 virtual 方法，因此不改基类、不加 virtual。
2. **职责全部搬入**：save/load/reconcile/FSM/raw buffer/拦截判断全部进新类；`FpmLink`/`RouteSync`/`fpmsyncd.cpp` 只保留一行调用。
3. **实例归属方案 A**：`RouteSync` 持有 `NhgWarmRestartAssist` 成员（声明在 `m_rib_fib_nhg_mgr` 之后，构造时传入其指针）；Assist 反持 `NHGMgr*` 但不拥有。
4. **独立 WarmStart 条目**：appName `fpmsyncd-nhg`、dockerName `bgp`。已验证 swss-common 行为：`initialize` 只建连接不写 enable 表；`checkWarmStart` 无配置返回 false（首次对 fpmsyncd-nhg 会 fall back cold 并一过性置 `m_enabled=false`，但随后 bgp 的 checkWarmStart 会恢复，无影响）；reconcile 完成时上报 `WarmStart::setWarmStartState("fpmsyncd-nhg", RECONCILED)` 兜底 finalizer 等待。

### 架构选择

- 选：独立类 + 独立文件（fpmsyncd/nhgWarmRestartAssist.h/.cpp）
- 否决：逻辑留在 NHGMgr（与框架不一致）；修改 AppRestartAssist 加 virtual（侵入共享基类）

## 实现概要

### 文件清单

| 文件 | 变更 |
|---|---|
| fpmsyncd/nhgWarmRestartAssist.h/.cpp | 新增。类骨架 + save/load/init/reconcile Phase1/2 + helpers + 拦截/buffer + 回放 + RECONCILED 上报 |
| fpmsyncd/nhgmgr.h/.cpp | 删除全部 warm restart 代码（FSM enum、3 struct、reconcile、helpers、4 成员），保留 `addNHGFullWithSonicId`，新增 `getRIBNHGTable()`/`getSonicIDMgr()` 数据路径访问器 |
| fpmsyncd/routesync.h/.cpp | 持有 `m_nhgWarmAssist` + `getNhgWarmAssist()`；新增 public `dispatchBufferedRouteMsg()`；删除旧 buffer/replay 设施；`onWarmStartEnd` 切换两行调用 |
| fpmsyncd/fpmlink.cpp | 拦截块切换为 `shouldIntercept` + `bufferNHG`/`bufferRoute` |
| fpmsyncd/fpmsyncd.cpp | 三处调用点改走 `getNhgWarmAssist()`（init/load/save） |
| fpmsyncd/Makefile.am、tests/mock_tests/Makefile.am | 加入新源文件与 warmRestartAssist.cpp |
| tests/mock_tests/fpmsyncd/nhgWarmRestartAssist_ut.cpp | 新增 15 个测试（Init×3、Save×3、Load×3、Phase1×2、AddWithSonicId、EndToEnd、Intercept×2） |
| tests/mock_tests/fpmsyncd/nhgmgr_ut.cpp | 删除已迁移测试与死宏/fixture 残留（60 个测试保留） |

### 核心逻辑（新类方法）

- `initWarmStart()`：FSM NONE→INITIALIZED
- `saveState(Table&)`：遍历 RIB NHG table 写 APPL_STATE_DB `NHG_FULL_STATE_TABLE`（sonic_nhg_id/af/json/pic_context_id + NHG_ID_ALLOCATOR）
- `loadState(Table&, Table&)`：读回 saved infos + APP_DB 现存 NHG fvs，恢复 SonicIDMgr 分配器
- `shouldIntercept(nlh)`：`inProgress()` 且类型 ∈ {RTM_NEW/DELNHGFIB, RTM_NEW/DELROUTE, RTM_NEW/DELSRV6VPNROUTE}
- `bufferNHG/bufferRoute(nlh)`：原始字节拷贝进对应 buffer
- `reconcileNHGs()`：Phase1（单跳干跑匹配复用 sonic ID）+ Phase2（拓扑排序 + FV hash 精确匹配复用 + 清理陈旧 APP_DB 条目）+ 清 buffer；Phase2 尾部上报 RECONCILED
- `replayBufferedRoutes(RouteSync&)`：逐条调 `RouteSync::dispatchBufferedRouteMsg` 后清 buffer

### 集成方式

- SIGTERM/SIGINT → fpmsyncd 主循环检测 `gShutdownRequested` → `saveState()` → flush → 退出（依赖 warm reboot 先停 bgp 后停 database 的顺序）
- warm start 时 fpmsyncd.cpp 调 `initWarmStart()` + `loadState()`；FPM/netlink 消息经 FpmLink 拦截缓存；EOIU/warm 定时器到期 → `onWarmStartEnd` → `reconcileNHGs()` + `replayBufferedRoutes()`

## 接口说明

### 对外

- APPL_STATE_DB `NHG_FULL_STATE_TABLE`：字段 `sonic_nhg_id`、`af`、`nhg`（JSON）、`pic_context_id`；特殊键 `NHG_ID_ALLOCATOR`（`next_nhg_id`/`next_pic_id`）——schema 不变
- STATE_DB `WARM_RESTART_TABLE|fpmsyncd-nhg`：新增条目（restore_count/state），供 warm-reboot finalizer 等待

### 对内（新类公开接口）

```cpp
NhgWarmRestartAssist(RedisPipeline *pipeline, NHGMgr *nhgMgr);
bool inProgress() const;
void initWarmStart();
void saveState(swss::Table &stateTable);
void loadState(swss::Table &stateTable, swss::Table &appDbNhgTable);
bool shouldIntercept(const struct nlmsghdr *nlh) const;
void bufferNHG(const struct nlmsghdr *nlh);
void bufferRoute(const struct nlmsghdr *nlh);
void reconcileNHGs();
void replayBufferedRoutes(RouteSync &routeSync);
```

- `RouteSync` 新增：`NhgWarmRestartAssist& getNhgWarmAssist()`、`void dispatchBufferedRouteMsg(struct nlmsghdr *nlh)`
- `NHGMgr` 新增：`RIBNHGTable *getRIBNHGTable()`、`SonicIDMgr &getSonicIDMgr()`
- 向后兼容：无对外行为变更；`NHGMgr::addNHGFullWithSonicId` 保留

## 测试覆盖

- gtest mock 单测：`tests_fpmsyncd --gtest_filter='*WarmRestart*'` 共 15 个用例（新文件），覆盖 init/save/load/Phase1 单跳与多跳跳过/ID 复用/端到端重启模拟/拦截分类/非 in-progress 不累积
- 端到端用例含 ID 复用强断言：`EXPECT_EQ(reconciledEntry->getSonicObjIDNum(), originalSonicId)`
- 现有 nhgmgr_ut 60 个用例不受影响

## 已知限制与后续事项

1. **本地未构建验证**（macOS 无构建环境）：编译与 gtest 需 CI/Jenkins 验证
2. **优雅停机依赖容器停止顺序**：save 需在 database 容器停止前完成；需现网验证 bgp 容器 supervisord `stopwaitsecs` 与 warm-reboot 脚本停止顺序（默认 10s 足够，saveState 正常 <1s）
3. **首次 warm reboot** 对 `fpmsyncd-nhg` 条目 restore_count 不存在时基类 fall back cold（符合预期，第二轮起正常）
4. 可选项：参考 orchagent 增加 select 前检查与 EINTR 显式处理（健壮性增强，非必需）
