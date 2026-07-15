# Proposal: RIB/FIB NHG Manager Warm Reboot Support

## Summary

为 fpmsyncd 的 NHG Manager（RIBNHGTable / SonicPICContentTable）添加 warm reboot 支持，使 NHG 和 PIC Context 表项在 warm reboot 前后能够正确 reconcile，避免不必要的表项删除和重建，保障流量无中断。

## Problem Statement

当前 fpmsyncd 的 warm reboot 实现只覆盖了 `ROUTE_TABLE` — 通过 `WarmStartHelper` 完成路由表项的 restore 和 reconcile。但 NHG Manager 管理的 `NEXTHOP_GROUP_TABLE` 和 `PIC_CONTEXT_TABLE` **没有任何 warm reboot 支持**：

1. **NHG 表项直接写入**：warm reboot 期间，`onNextHopGroupFullMsg()` 直接调用 `NHGMgr::addNHGFull()` 写入 AppDB，没有 buffering 或 reconcile 逻辑
2. **Zebra ID 映射丢失**：RIB 层的 zebra ID → sonic ID 映射只存在于内存中（`RIBNHGTable::m_nhg_map`），重启后丢失，导致无法关联新旧 NHG
3. **依赖关系丢失**：NHG 之间的 depends/dependents 关系（用于 backwalk）重启后丢失
4. **路由 reconcile 依赖 NHG**：路由的 reconcile 需要通过 zebra ID 查找 NHG 获取下一跳信息，但在 NHG reconcile 完成之前，NHG Manager 状态不可用

这会导致 warm reboot 后 NHG 表项被全量删除再重建，造成短暂的流量中断。

## Proposed Solution

### 高层方案

1. **Graceful shutdown 阶段**：在关闭前将 NHG Manager 的关键状态（zebra ID → sonic ID 映射、NHG entry 信息）持久化到 DB
2. **Warm start 阶段**：
   - 加载持久化的 NHG 映射信息 + AppDB 中的 NHG_TABLE 内容
   - 在 warm reboot 期间缓存所有新收到的 NHG 消息
   - 按单跳 → 多跳的顺序完成 NHG reconcile
   - NHG reconcile 完成后再触发路由 reconcile
3. **扩展 WarmStartHelper**：修改为支持多 table 的 reconcile（或复用 `AppRestartAssist` 模式）

### 关键设计决策

- **单跳 nexthop**：不创建 sonic NHG 对象，路由直接使用，重启后直接重新插入 RIBEntry，无需 reconcile 映射
- **多跳 nexthop**：需要通过 tempRIBEntryTable 做完整 reconcile，处理依赖关系后按依赖顺序写入
- **路由处理**：等待 NHG reconcile 完成后再进行，通过 NHG Manager 获取 zebra ID 对应的 sonic 下一跳信息

## Scope

### In Scope

- fpmsyncd NHG Manager 的 warm reboot shutdown 存储逻辑
- fpmsyncd NHG Manager 的 warm reboot restore 和 reconcile 逻辑
- 单跳/多跳 nexthop 的分别处理
- NHG reconcile 与路由 reconcile 的协调顺序
- NHG 依赖关系的恢复
- SRv6 VPN 场景的支持

### Out of Scope

- orchestrator/syncd 层面的 warm reboot 改动
- BGP/zebra 层面的 graceful restart 改动
- PIC (Prefix Independent Convergence) 功能本身的修改
- warm reboot 的端到端测试框架

## Success Criteria

1. warm reboot 前后 `NEXTHOP_GROUP_TABLE` 和 `PIC_CONTEXT_TABLE` 中的表项保持一致（无不必要的删除和重建）
2. 新增/删除的 NHG 在 reconcile 后正确反映到 AppDB
3. NHG 之间的依赖关系在重启后正确恢复
4. 路由 reconcile 能够正确使用 NHG Manager 提供的下一跳信息
5. SRv6 VPN 路由的 NHG warm reboot 正常工作

## Impact

- **流量影响**：warm reboot 期间消除因 NHG 全量重建导致的短暂流量丢失
- **代码影响**：主要修改 fpmsyncd/nhgmgr.cpp、routesync.cpp、warmRestartHelper 或新增 reconcile 模块
- **依赖**：无外部组件依赖变更
