# 项目设计

本项目在 `sonic-swss` 的 `fpmsyncd` 模块中为 RIB/FIB NHG 管理器补充 warm reboot 支持。核心目标是确保 `NHGMgr` 在进程重启后能完整恢复内存状态，避免 NHG ID 错配导致的路由转发中断。通过通用状态持久化框架（`WarmRestartStateManager` + `WarmRestartStateAdapter`），支持多子系统的独立部署与扩展。

## RIB/FIB NHG Warm Reboot

### 背景

`NHGMgr` 维护 RIB NHG 表（`RIBNHGTable`）、Sonic Gateway NHG 表（`SonicGateWayNHGTable`）及 Sonic ID 分配器（`SonicIDMgr`）三类核心内存状态。当前 warm reboot 仅覆盖 `ROUTE_TABLE`（通过 `WarmStartHelper`），`NHGMgr` 的内存状态重启后完全丢失，`SonicIDAllocator` 从 1 重新分配 ID，导致与 `APPL_DB` / ASIC 已存在条目冲突。

### 架构决策

| 决策 | 选择 | 理由 |
|------|------|------|
| 持久化存储 | `APPL_STATE_DB` 专用 Table | 与现有 Route warm reboot 机制一致；Redis 保证原子性 |
| 写入时机 | **仅在 warm reboot 停止前**（SIGTERM 时） | 零运行时开销；系统保证超时内完成 |
| 超时设置 | **5 秒** | 基于 docker 10 秒 graceful stop，预留足够余量 |
| 功能开关 | **CONFIG_DB `nhg-warm-reboot-enabled`** | 默认 enabled，可禁用走冷启动 |
| 路径选择 | **CONFIG_DB `rib-fib-nhg-warm-reboot`** | 区分 RIB/FIB 路径和原有 WarmStartHelper 路径 |
| Reconcile 策略 | **tempTable 双写 + 多轮处理** | 支持 old/new ID 映射；处理 Zebra 推送顺序不一致 |
| 依赖关系 | **不持久化，reconcile 后重建** | Zebra 重推带新 ID，旧依赖关系失效 |
| 辅助类设计 | **NHGWarmRestartAssist 继承 AppRestartAssist + 实现 WarmRestartStateAdapter** | 复用 STALE/SAME/NEW/DELETE 状态机；通过适配器接口接入统一管理框架 |
| 管理框架 | **WarmRestartStateManager 编排多个 WarmRestartStateAdapter** | 支持多子系统独立部署；统一超时、完整性检查、错误处理 |
| Route reconcile | **NHG reconcile 完成后执行** | Route 依赖 zebra_id → nexthop 映射 |
| 暂存策略 | **warm 期间拦截正常下发，暂存到缓存** | 避免新旧状态混淆，等 reconcile 统一写入 |
| tempTable 实现 | **复用 RIBNHGTable 类** | 避免双重维护；复用已有 API |
| NHG 清理策略 | **引用计数保护** | 被路由引用的 NHG 不因 Zebra 未重推而被误删 |
| 数据加载 | **四阶段加载** | APPL_STATE_DB → APP_DB NHG → APP_DB Route → NHG_GROUP_STATE_TABLE，确保完整映射 |
| NextHopGroup map 持久化 | **NHG_GROUP_STATE_TABLE** | 保存 m_nh_groups 映射，SIGTERM 时写入 APPL_STATE_DB |
| SonicIDAllocator 恢复 | **NHGAdapter::restoreState() 中恢复** | 从已保存的 Sonic NHG 条目中恢复 ID 分配器状态，无需独立持久化 |

### 数据模型

**schema.h 宏定义**：
```cpp
#define APP_NHG_RIB_STATE_TABLE_NAME    "NHG_RIB_STATE_TABLE"
#define APP_NHG_SONIC_STATE_TABLE_NAME  "NHG_SONIC_STATE_TABLE"
#define APP_NHG_GROUP_STATE_TABLE_NAME  "NHG_GROUP_STATE_TABLE"
```

新增 `APPL_STATE_DB` State Tables：

**NHG_RIB_STATE_TABLE**：Key = `<zebra_id>`

| Field | 说明 |
|-------|------|
| `sonic_obj_id` | Sonic NHG ID（单跳为 0） |
| `sonic_obj_type` | 0=NORMAL, 1=SRv6_GATEWAY |
| `sonic_gateway_nhg_id` | SRv6 PIC Context ID |
| `af` | 地址族（2=AF_INET, 10=AF_INET6） |
| `nexthop` | compare field |
| `ifname` | compare field |
| `weight` | compare field |
| `vpn_sid` | SRv6 VPN SID |
| `seg_src` | SRv6 Segment Source |
| `group` | 成员列表：`<zebra_id>:<weight>[,...]` |

**NHG_SONIC_STATE_TABLE**：Key = `<type>:<sonic_id>`

| Field | 说明 |
|-------|------|
| `nexthop` | SonicNHGObjectKey 字段 |
| `ifname` | SonicNHGObjectKey 字段 |
| `vpn_sid` | VPN SID |
| `seg_src` | Segment Source |
| `group_members` | `<zebra_id>:<weight>[,...]` |

**NHG_GROUP_STATE_TABLE**：Key = `<nhg_id>`（uint32 字符串）

| Field | 说明 |
|-------|------|
| `nexthop` | Nexthop IP 字符串 |
| `intf` | 接口名 |
| `installed` | 0 或 1 |

### 核心类设计

**NHGWarmRestartAssist**：warm reboot 辅助类（增强版），实现 `WarmRestartStateAdapter` 接口

```cpp
class NHGWarmRestartAssist : public AppRestartAssist,
                             public WarmRestartStateAdapter {
public:
    // WarmRestartStateAdapter 接口
    std::string name() const override;      // "NHG"
    void saveState() override;              // 委托到 saveStateToDB()
    void restoreState() override;           // 委托到 readNHGTablesToMap()
    void reconcile() override;              // 委托到 reconcileAll()
    bool checkIntegrity() const override;
    void clearState() override;
    bool isEnabled() const override;

    // 状态保存与恢复
    void saveStateToDB(...);           // SIGTERM 时调用
    void readNHGTablesToMap();         // 启动时恢复 APPL_STATE_DB
    void loadAppDBNHGTable();          // 加载 APP_DB Sonic NHG TABLE
    void loadAppDBRouteTable();        // 加载 APP_DB Route TABLE（建立 ref count）

    // warm 期间暂存
    void insertNHGToMap(uint32_t zebra_id,
                        const NextHopGroupFull& nhg,
                        uint8_t af);
    void cacheRouteWithZebraId(...);

    // Reconcile
    void reconcileAll();
    virtual bool compareNHGContent(...); // 新旧 NHG 对比（可 override）

    // 配置
    void setRibFibWBAvailable(bool enabled);
    bool isRibFibWBAvailable() const;

    // Debug
    void dumpTempTable(std::string& ret);
    void dumpRouteCache(std::string& ret);
    void dumpRefCount(std::string& ret);

private:
    void reconcileNHG();
    void reconcileRoutes();
    void migrateToRIBTable();  // 实际写入 NHGMgr 表

    // 数据：复用 RIBNHGTable 作为 tempTable
    RIBNHGTable *m_temp_rib_table;

    // 路由缓存
    std::map<std::string, RouteCacheEntry> m_route_cache;

    // NHG 引用计数
    std::unordered_map<uint32_t, uint32_t> m_nhg_ref_count;

    // NHGMgr 引用（用于 reconcile 时操作 NHGMgr 表）
    NHGMgr *m_nhgmgr;
};
```

### 配置项

| 配置项 | 位置 | 类型 | 默认值 | 说明 |
|--------|------|------|--------|------|
| `nhg-warm-reboot-enabled` | `CONFIG_DB` `DEVICE_METADATA\|localhost` | string | `"enabled"` | NHG warm reboot 总开关 |
| `rib-fib-nhg-warm-reboot` | `CONFIG_DB` `DEVICE_METADATA\|localhost` | string | `"disabled"` | 启用 RIB/FIB warm reboot 路径（替代原有 WarmStartHelper NHG 路径）|
| `SAVE_STATE_TIMEOUT_SEC` | 代码常量 | int | 5 | saveStateToDB 超时（秒）|

### 关键流程

**阶段一：正常运行**
```
Zebra → FPM → onNextHopGroupFullMsg()
    → 检查 isRibFibWBAvailable && isWarmStartInProgress
    → 否：m_rib_fib_nhg_mgr.addNHGFull(nhg, af)
    → 是：暂存到 NHGWarmRestartAssist
```

**阶段二：warm reboot 停止前**
```
SIGTERM → sigterm_handler()
    → 设置 g_sigterm_received = 1
主循环检测到 g_sigterm_received
    → WarmRestartStateManager::saveAll()
        → 遍历所有已注册 adapter，依次调用 saveState()
    → pipeline.flush()
    → exit(0)
```

**阶段三：启动后四阶段加载**
```
fpmsyncd 启动
    → WarmRestartStateManager::restoreAll()
        → 对每个已注册 adapter：checkIntegrity() → restoreState()
    → readNHGTablesToMap()        // 加载 APPL_STATE_DB（旧 NHG 状态，标记 STALE）
    → 读取 NHG_GROUP_STATE_TABLE  // 恢复 NextHopGroup map
    → loadAppDBNHGTable()         // 加载 APP_DB Sonic NHG TABLE（当前 ASIC 状态）
    → loadAppDBRouteTable()       // 加载 APP_DB Route TABLE（建立 ref count）
    → 恢复 SonicIDAllocator
```

**阶段四：warm reboot 期间（暂存）**
```
Zebra 重推 NHG → onNextHopGroupFullMsg()
    → isRibFibWBAvailable && isWarmStartInProgress
    → insertNHGToMap()            // 暂存，与旧 NHG 对比，标记 SAME/NEW

Zebra 重推 Route → onRouteMsg()
    → cacheRouteWithZebraId()     // 暂存
```

**阶段五：reconcile**
```
WarmRestartStateManager::reconcileAll()
    → 对每个已注册 adapter 依次调用 reconcile() + clearState()
NHGWarmRestartAssist::reconcile()
    → reconcileNHG()
        → 将 APP DB NHG 通过 tempTable 映射关系与新 NHG 对应
        → SAME：保留 sonic_obj_id，更新 zebra_id 映射
        → NEW：分配新 sonic_obj_id
        → DELETE + ref_count==0：从 APPL_DB 删除
        → DELETE + ref_count>0：保留，记 WARN
        → migrateToRIBTable()：实际写入 NHGMgr 的 RIBNHGTable/SonicGateWayNHGTable
    → reconcileRoutes()
        → 通过 NHGMgr RIBNHGTable 查询 zebra_id → sonic_obj_id
        → 转换 Route 中的 NHG ID 为 Sonic NHG ID
        → 写入 ROUTE_TABLE
    → NextHopGroup map reconcile
        → 比较已保存的 m_nh_groups 与 Zebra 重推的 NHG 消息
        → SAME：保留 installed 状态
        → NEW：安装 NHG
        → DELETE：从 m_nh_groups 删除，deleteNextHopGroupDb()
```

### NHG 引用计数机制

- **建立时机**：`loadAppDBRouteTable()` 时解析每条路由引用的 NHG ID，递增 `m_nhg_ref_count[zebra_id]`
- **查询时机**：reconcileNHG 中对 DELETE 条目检查 ref count
- **更新时机**：reconcileRoute 完成后，根据新增/删除路由更新 ref count
- **清理规则**：仅 ref count == 0 的 DELETE 条目才实际从 APPL_DB 删除

### 降级与容错

| 场景 | 处理策略 |
|------|----------|
| State Table 不完整 | 记录 WARN，清理后走冷启动 |
| State Table 过大（>100K） | 记录 WARN，走冷启动 |
| SIGTERM 超时 | 已写入部分可能残留，启动时检测清理 |
| Zebra 推送顺序不一致 | 多轮 reconcile，MAX_ROUNDS 限制 |
| 依赖无法解析 | 标记错误，不影响其他条目 |
| warm reboot 取消 | 检测取消标志，恢复到正常状态 |
| RIB/FIB 路径未启用 | 走原有 WarmStartHelper 流程，完全兼容 |
| NHG 有路由引用但 Zebra 未重推 | 保留 NHG（ref count > 0），记录 WARN |

### 约束

- 不修改 `orchagent` 侧逻辑；NHG/Route reconcile 仅在 fpmsyncd 侧完成
- 不改变 Zebra/FRR 侧行为
- 回滚兼容：旧版本不读 State Table，自动走冷启动路径
- RIB/FIB 路径未启用时，原有 WarmStartHelper 流程不受任何影响
- Reconcile 完成前 ASIC 保持旧表项转发

### 风险登记

| 风险 | 可能性 | 影响 | 缓解措施 |
|------|--------|------|----------|
| SIGTERM 超时 | 低 | 中 | 降级到冷启动 |
| Zebra 推送顺序问题 | 中 | 高 | 多轮 reconcile |
| State Table 部分写入 | 低 | 高 | 完整性检测 |
| 内存不足 | 低 | 高 | temp 表大小限制 |
| RIBNHGTable 复用增加耦合 | 中 | 中 | 独立 pipeline 实例 |
| 对比逻辑误判 | 中 | 低 | SAME 误判为 NEW 仅浪费 ID 资源 |

## 变更记录

| 日期 | 功能 | 变更说明 |
|------|------|---------|
| 2026-04-17 | RIB/FIB NHG Warm Reboot | 新增：初始提案与设计框架 |
| 2026-04-20 | RIB/FIB NHG Warm Reboot | 更新：Socratic 探索后修订，新增多轮 reconcile、降级路径、完整性检测 |
| 2026-04-20 | RIB/FIB NHG Warm Reboot | 更新：确认 SIGTERM 超时 5 秒、添加配置开关 `nhg-warm-reboot-enabled`、对齐 schema.h 命名 |
| 2026-04-24 | RIB/FIB NHG Warm Reboot 增强 | 更新：9 项增强 — 暂存逻辑、新旧对比、条件化路径、migrateToRIBTable 实际执行、复用 RIBNHGTable、reconcileRoute 用 NHGMgr 表、NHG ref count、三阶段数据加载、debug 方法 |
| 2026-04-28 | APPL_STATE_DB 持久化增强 | 更新：引入 WarmRestartStateManager + WarmRestartStateAdapter 框架；NHGWarmRestartAssist 实现适配器接口；新增 NHG_GROUP_STATE_TABLE 持久化 NextHopGroup map；SonicIDAllocator 恢复纳入 restoreState()；SIGTERM 流程改为 saveAll() 统一编排；数据加载扩展为四阶段 |
