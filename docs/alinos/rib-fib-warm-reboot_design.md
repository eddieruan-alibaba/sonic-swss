# 项目设计

RIB/FIB NHG Manager Warm Reboot 项目，为 fpmsyncd 的 NHG Manager 添加 warm reboot 支持，使 NEXTHOP_GROUP_TABLE 和 PIC_CONTEXT_TABLE 在 warm reboot 前后正确 reconcile，避免不必要的表项删除重建，保障流量无中断。

## NHG Manager Warm Reboot

### 核心架构决策

1. **FpmLink 层统一消息拦截**：warm restart 期间，在 `FpmLink::processFpmMessage()` 中统一拦截 NHG 和路由消息（经 `NhgWarmRestartAssist::shouldIntercept()` 判断），缓存原始 netlink 字节到 Assist 的 raw buffer。避免了双层拦截和 libnl 对象管理的复杂性。

2. **NhgWarmRestartAssist 集中管理 reconcile**：全部 warm restart 逻辑（save/load/拦截缓存/reconcile/路由回放/FSM）集中在独立类 `NhgWarmRestartAssist`（`fpmsyncd/nhgWarmRestartAssist.h/.cpp`），继承 `AppRestartAssist` 但仅复用外壳（构造/timer/WarmStart 注册），数据路径方法全部新增、不改基类。NHGMgr 回归纯数据路径。NHG 的 FV vector 匹配、依赖拓扑排序、sonic ID 复用是独有需求，由子类自实现。FSM 维持 4-state（INITIALIZED → RESTORED → RECONCILING → RECONCILED）。

3. **APP_DB FV vector 做 NHG 匹配**：warm reboot 后 APP_DB 保留旧 NHG 表项。新 NHG 干跑计算 FV vector 后，与旧表项精确比较。匹配成功→复用 sonic ID（不写 APP_DB），不匹配→新分配 ID。避免了 ribID 变化和 SonicNHGObjectKey 中 groupMember ID 失效的问题。

4. **临时 RIBNHGEntry 干跑模式**：reconcile 时创建临时 RIBNHGEntry 只调用 `setEntry()` 计算 FV vector，不分配 ID 不写 DB。最小侵入现有代码。

5. **路由以原始 netlink 字节缓存**：warm restart 期间路由不走 `onRouteMsg()` 解析（NHG Manager 不可用会导致路由丢弃），改为缓存原始字节。NHG reconcile 完成后重新 dispatch。

6. **复用 NHG_FULL_STATE_TABLE**：不新建 DB 表。在现有 APPL_STATE_DB 调试表上新增 af 字段和 NHG_ID_ALLOCATOR 键。warm reboot 后 Redis 整体恢复，数据保留。

### 分阶段 Reconcile 流程

| Phase | 内容 | 依赖 |
|-------|------|------|
| Phase 1 | 普通单跳 NHG：直接 addNHGFull，不产生 Sonic 对象 | 无 |
| Phase 2 | SRv6 单跳 + 多跳 NHG：干跑→FV 匹配→复用/新建→拓扑排序写入 | Phase 1 完成 |
| Phase 3 | 回放缓存路由，通过正常 dispatch 路径处理 | Phase 2 完成 |
| Phase 4 | WarmStartHelper.reconcile() 路由 reconcile | Phase 3 完成 |
| Phase 5 | 恢复 pipeline flush | Phase 4 完成 |

### 关键分类规则

| 类型 | 条件 | Phase | APP_DB 操作 |
|------|------|-------|------------|
| 普通单跳 NHG | type=NHG_NORMAL 且 isSingleHop | 1 | 无 |
| SRv6 单跳 NHG | type=NHG_WITH_SRV6_PIC_CONTEXT 且 isSingleHop | 2 | 匹配→跳过; 不匹配→写入 |
| 多跳 NHG | !isSingleHop | 2 | 匹配→跳过; 不匹配→写入 |

### 结构组织（2026-07-21 重构）

- **实例归属**：RouteSync 持有 `NhgWarmRestartAssist` 实例（`getNhgWarmAssist()`），Assist 反持 NHGMgr 指针（不拥有）。时序驱动（RouteSync）与数据路径（NHGMgr）各自归位。
- **STATE_DB 条目**：基类构造注册独立 appName `fpmsyncd-nhg`（dockerName `bgp`）。warm 检测仍走路由通道（`warmStartHelper.checkAndStart()`）；不依赖基类 in-progress 标志，统一用子类 FSM。finalizer 兼容性为实现期待验证点，兜底是 RECONCILED 时上报状态，回退方案是复用 "bgp" 条目。
- **行为不变**：reconcile 时序（EOIU/holdTimer 驱动 `onWarmStartEnd`）、匹配算法、DB 读写全部原样搬运，只换归属。现有 UT/E2E 用例不变，只换被测对象。

### 关键接口

- `NhgWarmRestartAssist::saveState()` — shutdown 持久化（原 `NHGMgr::saveWarmRestartState`）
- `NhgWarmRestartAssist::loadState()` — 启动加载 NHG_FULL_STATE_TABLE + APP_DB NHG_TABLE
- `NhgWarmRestartAssist::shouldIntercept()` / `bufferNHG()` / `bufferRoute()` — FpmLink 层消息拦截与缓存
- `NhgWarmRestartAssist::reconcileNHGs()` — Phase 1 + Phase 2（干跑匹配 + 拓扑排序 + 写入）
- `NhgWarmRestartAssist::replayBufferedRoutes()` — Phase 3 路由回放
- `NHGMgr::addNHGFullWithSonicId()` — 复用 sonicID 的添加路径（保留在 NHGMgr，属数据路径）
- NHGMgr 暴露最小接口：`getRIBNHGTable()`、`getSonicIDMgr()`、`addNHGFull()`、`delNHGFull()`

### 数据模型

- **扩展表**：APPL_STATE_DB NHG_FULL_STATE_TABLE 新增 af 字段 + NHG_ID_ALLOCATOR 键
- **匹配数据源**：APP_DB NEXTHOP_GROUP_TABLE 的 FV vector（精确字符串比较）
- **最小持久化**：普通单跳 NHG（无 Sonic 对象）不存储

### 约束条件

- Phase 2 依赖 Phase 1 完成（多跳 resolve 需要单跳已在 RIBNHGTable 中）
- 路由回放依赖 NHG reconcile 完成（`getRIBNHGEntryByRIBID()` 必须可用）
- NHG reconcile 必须在 pipeline flush 恢复之前完成
- 拓扑排序需检测环形依赖，有环跳过并告警
- PIC context 作为 NHG entry 附属，不需要独立 reconcile

### 修改文件清单

| 文件 | 修改 |
|------|------|
| fpmsyncd/nhgWarmRestartAssist.h/cpp | **新增**，承载全部 NHG warm restart 逻辑 |
| fpmsyncd/fpmlink.cpp | processFpmMessage() 拦截改为调用 Assist |
| fpmsyncd/nhgmgr.h/cpp | 删除 warm restart 逻辑；保留 addNHGFullWithSonicId；暴露最小数据接口 |
| fpmsyncd/routesync.h/cpp | 持有 Assist 实例；onWarmStartEnd 改为调用 Assist；删除 raw buffer |
| fpmsyncd/fpmsyncd.cpp | save/load/init 三处调用改走 Assist |
| fpmsyncd/Makefile.am | 加入新源文件 |

## 变更记录

| 日期 | 功能 | 变更说明 |
|------|------|---------|
| 2026-07-15 | NHG Manager Warm Reboot | 新增：初始设计 |
| 2026-07-15 | NHG Manager Warm Reboot | 更新：探索后修正 — SRv6 单跳参与 reconcile、路由原始消息缓存、复用 NHG_FULL_STATE_TABLE、SonicNHGObjectKey 匹配 |
| 2026-07-15 | NHG Manager Warm Reboot | 更新：Brainstorming 细化 — FpmLink 层统一拦截、APP_DB FV vector 匹配替代 SonicNHGObjectKey、临时 RIBNHGEntry 干跑模式、addNHGFullWithSonicId 复用路径 |
| 2026-07-21 | NHG Manager Warm Reboot | 更新：结构重构 — 新建 NhgWarmRestartAssist（继承 AppRestartAssist 外壳）集中全部 warm restart 逻辑；RouteSync 持有实例；NHGMgr 回归纯数据路径；独立 STATE_DB 条目 fpmsyncd-nhg；行为不变 |
