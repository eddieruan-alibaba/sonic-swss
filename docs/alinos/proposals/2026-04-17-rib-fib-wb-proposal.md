## Why

在 SONiC warm reboot 场景下，fpmsyncd 通过 FPM 接收 Zebra 下发的路由信息（RIB），并将其转化为 FIB 写入 APPL_DB。当前实现中，`NHGMgr`（RIB/FIB NHG 管理器）以及其管理的内存状态（RIB NHG 表、Sonic Gateway NHG 表、ID 分配器）在 warm reboot 过程中**不具备状态持久化与恢复能力**，导致重启后 NHG 对象 ID 可能错配，进而引起路由转发中断或 ASIC 编程错误。本变更旨在为 RIB/FIB NHG 模块补充 warm reboot 支持，确保重启期间流量不中断。

## What Changes

- **新增** `NHGMgr` warm reboot 状态保存逻辑：在重启前将 RIB NHG 表、Sonic Gateway NHG 表及 ID 分配状态持久化到 Redis（`APPL_STATE_DB` 或专用 State Table）。
- **新增** `NHGMgr` warm reboot 状态恢复逻辑：进程重启后，从 Redis 恢复上述内存状态，重建 `RIBNHGTable`、`SonicGateWayNHGTable` 以及 `SonicIDMgr` 中的 ID 映射。
- **新增** `SonicIDAllocator::recoverSonicIDMapFromDB()` 实现（当前为空桩）：支持从 DB 恢复已分配 ID 集合，避免 ID 重用。
- **修改** `RouteSync::onWarmStartEnd()`：在 reconcile 阶段同步触发 RIB/FIB NHG 模块的 reconcile，清除过期条目。
- **修改** `RouteSync::setRouteWithWarmRestart()`：在 warm restart 进行中时，RIB/FIB NHG 相关操作也进入缓冲模式，不立即写入 DB，等待 reconcile 完成后批量应用。
- **新增** RIB/FIB warm restart 的集成测试用例，验证重启前后 NHG 条目一致性。

## Capabilities

### New Capabilities

- `rib-fib-nhg-warm-reboot`: RIB/FIB NHG 管理器的 warm reboot 状态持久化与恢复能力，包括 RIB NHG 表恢复、Sonic Gateway NHG 表恢复、ID 分配器状态恢复，以及 reconcile 阶段过期条目清理。

### Modified Capabilities

（无现有 spec 需要修改）

## Impact

- **fpmsyncd/nhgmgr.h / nhgmgr.cpp**：`NHGMgr`、`RIBNHGTable`、`SonicGateWayNHGTable`、`SonicIDAllocator` 增加持久化/恢复接口。
- **fpmsyncd/routesync.h / routesync.cpp**：`RouteSync::onWarmStartEnd()` 及 `setRouteWithWarmRestart()` 修改。
- **warmrestart/warmRestartHelper**：可能复用现有 `WarmStartHelper` 的 `runRestoration()` / `reconcile()` 机制，或扩展新的辅助方法。
- **Redis Schema**：新增用于持久化 NHG 状态的 State Table（`APPL_STATE_DB`）。
- **测试**：`tests/` 目录下新增 warm reboot 场景集成测试。
- **依赖**：无外部新依赖；依赖现有 `sonic-swss-common` 的 `WarmStart` 框架。
