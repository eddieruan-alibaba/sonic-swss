## Context

fpmsyncd 通过 FPM 协议接收 Zebra/FRR 下发的路由信息，经过 `RouteSync` 处理后写入 `APPL_DB`，再由 `orchagent` 编程到 ASIC。其中，`NHGMgr` 负责管理 RIB NHG（来自 Zebra 的 NextHopGroupFull 对象）到 Sonic NHG/PIC Context 对象的映射，维护以下核心内存状态：

- `RIBNHGTable`：RIB ID → `RIBNHGEntry`，记录依赖关系、Sonic Object ID、地址族等。
- `SonicGateWayNHGTable`：`SonicNHGObjectKey` → `SonicGateWayNHGEntry`，管理 NHG 对象和 SRv6 PIC Context 的去重与引用计数。
- `SonicIDMgr` / `SonicIDAllocator`：管理 Sonic NHG ID 和 PIC Context ID 的分配（`m_id_map`）。

当前 warm reboot 支持仅覆盖 `ROUTE_TABLE`（通过 `WarmStartHelper` 缓冲和 reconcile），`NHGMgr` 的内存状态在进程重启后**完全丢失**。重启后 Zebra 重新下发 NHG 时，`SonicIDAllocator` 从 1 重新分配 ID，与 `APPL_DB` / ASIC 中已存在的条目产生冲突，`RIBNHGTable` 也无法感知已安装的条目，导致错误的增量更新。

`recoverSonicIDMapFromDB()` 当前为空桩（`return 0`），`RouteSync::onWarmStartEnd()` 中也未触发 NHG 层面的 reconcile。

---

## Goals / Non-Goals

**Goals:**

- 在 warm reboot 停止前将 `NHGMgr` 的关键状态持久化到 Redis（`APPL_STATE_DB`）。
- 进程重启后，从 Redis 恢复 `RIBNHGTable`、`SonicGateWayNHGTable` 及 `SonicIDMgr` 状态。
- 实现 NHG 层面的 reconcile：将恢复的旧状态与重启后 Zebra 重新下发的新状态对比，清除过期条目，保留有效条目，重建依赖关系。
- 实现 Route 层面的 reconcile：在 NHG reconcile 完成后，处理缓存的路由信息。
- 不中断重启期间的路由转发（流量保持由 ASIC 按旧表项转发）。
- 复用现有 `AppRestartAssist` 的机制，通过继承扩展。

**Non-Goals:**

- 不修改 `orchagent` 侧逻辑；NHG/Route reconcile 仅在 fpmsyncd 侧完成。
- 不实现完整的 `NHG_FULL_STATE_TABLE`（当前仅作调试用）的生产化。
- 不改变 Zebra/FRR 侧行为。

---

## Decisions

### D1：存储时机——只在 warm reboot 停止前写入 DB

**决策**：正常运行时不写入 DB，只在 warm reboot 停止前检测到 warm reboot 标志后，一次性将 NHG 状态写入 State Table。

**理由**：
- 减少 DB 写压力，NHG 状态变更频率低但无必要实时写入。
- warm reboot 停止前由系统保证 fpmsyncd 有时间完成状态保存。
- 通过 `WarmStart::isWarmStart()` 或 `WarmStart::isSystemWarmRebootEnabled()` 判断是否 warm reboot。

**实现方式**：
```cpp
void sigterm_handler(int signum) {
    if (WarmStart::isWarmStart() || WarmStart::isSystemWarmRebootEnabled()) {
        sync.getNHGMgr().getWarmRestartAssist().saveStateToDB();
        pipeline.flush();
    }
    exit(0);
}
```

---

### D2：单跳 nexthop 存储

**决策**：单跳 nexthop 也要存储，`sonic_obj_id` = 0（reserved ID）。

**理由**：
- 单跳 nexthop 需要参与 reconcile，和 Zebra 重推的 nexthop 做 hash key 对比。
- 存储所有 nexthop 保证完整性。

---

### D3：多跳 nexthop 使用 tempRIBEntryTable

**决策**：创建 `TempRIBEntryTable`，在 reconcile 阶段统一处理单跳和多跳 nexthop。

**流程**：
1. 启动后：从 DB 恢复到 `tempRIBEntryTable`（old 信息）
2. warm reboot 期间：Zebra 重推的 NHG 缓存到 `tempRIBEntryTable`（new 信息）
3. reconcile 时：
   - 对比 old/new，标记 STALE/NEW/SAME/DELETE
   - 处理多跳 nexthop 的依赖关系（用新 ID 重建）
4. 统一将 `tempRIBEntryTable` 合法条目迁移到 `RIBEntryTable`

---

### D4：依赖关系不持久化，reconcile 后重建

**决策**：`depends` / `dependents` 不持久化到 DB，reconcile 时使用 Zebra 重推的新 ID 建立依赖关系。

**理由**：
- Zebra 重推的 NHG 带有正确的新 ID 和新依赖关系。
- 旧 ID 和新 ID 不会完全一致，只用新 ID 建立依赖关系即可。
- 简化存储内容。

---

### D5：NHGWarmRestartAssist 继承 AppRestartAssist

**决策**：新建 `NHGWarmRestartAssist` 类继承 `AppRestartAssist`，统一处理 NHG 和 Route 的 reconcile。

**理由**：
- 复用 `AppRestartAssist` 的 STALE/SAME/NEW/DELETE 机制。
- 扩展处理 tempRIBEntryTable 的 old/new ID 映射。
- 扩展处理 Route 的缓存和 reconcile。
- NHGMgr 持有一个 `NHGWarmRestartAssist` 实例，对外统一接口。

---

### D6：TempRIBEntry 新建独立 class

**决策**：新建 `TempRIBEntry` class，不继承 `RIBNHGEntry`。

**理由**：
- `TempRIBEntry` 需要同时保存 old/new 两组数据。
- `RIBNHGEntry` 只有一组数据，继承会增加复杂度。
- 独立 class 更清晰。

---

### D7：Route reconcile 在 NHG reconcile 完成后执行

**决策**：Route 的缓存和 reconcile 在 `NHGWarmRestartAssist` 中统一处理，等 NHG reconcile 完成后再执行。

**理由**：
- Route 存储的是 zebra_id，需要通过 NHGMgr 查询真正的 nexthop fields。
- NHG reconcile 完成后，`RIBEntryTable` 才有完整的 zebra_id → nexthop 映射。

---

## State Table Schema

### NHG_RIB_STATE_TABLE（APPL_STATE_DB）

Key：`<zebra_id>`（uint32 转字符串）

| Field | Type | 说明 |
|-------|------|------|
| `sonic_obj_id` | string | Sonic NHG ID（单跳为 0）|
| `sonic_obj_type` | string | 0=NORMAL, 1=SRv6_GATEWAY |
| `sonic_gateway_nhg_id` | string | SRv6 PIC Context ID |
| `af` | string | 地址族（2=AF_INET, 10=AF_INET6）|
| `nexthop` | string | compare field |
| `ifname` | string | compare field |
| `weight` | string | compare field |
| `vpn_sid` | string | SRv6 VPN SID |
| `seg_src` | string | SRv6 Segment Source |
| `group` | string | 成员列表：`<zebra_id>:<weight>[,...]` |

### NHG_SONIC_STATE_TABLE（APPL_STATE_DB）

Key：`<type>:<sonic_id>`

| Field | Type | 说明 |
|-------|------|------|
| `nexthop` | string | SonicNHGObjectKey 字段 |
| `ifname` | string | SonicNHGObjectKey 字段 |
| `vpn_sid` | string | VPN SID |
| `seg_src` | string | Segment Source |
| `group_members` | string | `<zebra_id>:<weight>[,...]` |

**ID 恢复**：启动时遍历 `NHG_SONIC_STATE_TABLE` 的所有 key，提取 `sonic_id`，写入 `SonicIDAllocator::m_id_map`。

---

## Class Design

### TempRIBEntry

```cpp
class TempRIBEntry {
public:
    // 来自 DB 恢复
    uint32_t old_zebra_id = 0;
    std::string old_group;          // 成员是 old ID

    // 来自 Zebra 重推
    uint32_t new_zebra_id = 0;
    std::string new_group;          // 成员是 new ID

    // compare fields（单独成员）
    std::string nexthop;
    std::string ifname;
    std::string weight;
    std::string vpn_sid;
    std::string seg_src;
    uint8_t af = 0;

    // 需要保留
    uint32_t sonic_obj_id = 0;
    uint32_t sonic_obj_type = 0;          // 0=NORMAL, 1=SRv6_GATEWAY
    uint32_t sonic_gateway_nhg_id = 0;

    // 状态标记
    enum State { STALE, NEW, SAME, DELETE };
    State state = STALE;

    // hash key 用于对比
    std::string getHashKey() const;
    bool compareWithNewEntry(const TempRIBEntry& other) const;
};
```

### RouteCacheEntry

```cpp
struct RouteCacheEntry {
    std::string route_key;              // prefix
    uint32_t zebra_nhg_id;              // 下一跳的 zebra id
    std::vector<FieldValueTuple> fields; // route 其他字段
    bool is_delete;                     // 是否删除操作
};
```

### NHGWarmRestartAssist

```cpp
class NHGWarmRestartAssist : public AppRestartAssist {
public:
    NHGWarmRestartAssist(RedisPipeline *pipeline,
                         const std::string &appName,
                         const std::string &dockerName);

    // warm reboot 停止前调用，保存状态到 DB
    void saveStateToDB();

    // 启动时调用，从 DB 恢复到 tempTable
    void readNHGTablesToMap();

    // warm 期间缓存 NHG
    void insertNHGToMap(uint32_t zebra_id, const TempRIBEntry& entry);

    // warm 期间缓存 Route
    void cacheRouteWithZebraId(const std::string& route_key,
                                uint32_t zebra_id,
                                const std::vector<FieldValueTuple>& fields,
                                bool is_delete);

    // NHG reconcile（处理依赖关系）
    void reconcileNHG();

    // Route reconcile（等 NHG reconcile 完成后）
    void reconcileRoutes();

    // 统一 reconcile：先 NHG，后 Route
    void reconcileAll();

    // 获取 tempRIBEntryTable（供依赖关系处理使用）
    std::map<uint32_t, TempRIBEntry>& getTempRIBTable();

private:
    // tempRIBEntryTable：存储 old/new ID 映射
    std::map<uint32_t, TempRIBEntry> m_temp_rib_table;

    // Route 缓存
    std::map<std::string, RouteCacheEntry> m_route_cache;

    // State Table
    std::unique_ptr<Table> m_rib_state_table;
    std::unique_ptr<Table> m_sonic_state_table;
};
```

### NHGMgr 变更

```cpp
class NHGMgr {
public:
    // 构造时新增 app_state_pipeline 参数
    NHGMgr(RedisPipeline *pipeline,
           RedisPipeline *app_state_pipeline,
           const std::string &nexthopTableName,
           const std::string &picTableName,
           bool isStateTable);

    // 获取 WarmRestartAssist
    NHGWarmRestartAssist& getWarmRestartAssist() {
        return m_warm_restart_assist;
    }

private:
    NHGWarmRestartAssist m_warm_restart_assist;
};
```

---

## Data Flow

### 阶段一：正常运行

```
Zebra → FPM → onNextHopGroupFullMsg()
    → m_rib_fib_nhg_mgr.addNHGFull(nhg, af)
        → 更新 RIBNHGTable 内存
        → 更新 SonicGateWayNHGTable 内存
        → 写入 NEXTHOP_GROUP_TABLE (APPL_DB)
    → 不写入 State Table
```

### 阶段二：warm reboot 停止前

```
收到 SIGTERM 信号
    → 检查 WarmStart::isWarmStart() 或 isSystemWarmRebootEnabled()
    → 如果是 warm reboot：
        → sync.getNHGMgr().getWarmRestartAssist().saveStateToDB()
            → 遍历 RIBNHGTable，写入 NHG_RIB_STATE_TABLE
            → 遍历 SonicGateWayNHGTable，写入 NHG_SONIC_STATE_TABLE
        → pipeline.flush()
    → exit(0)
```

### 阶段三：warm reboot 启动后

```
fpmsyncd 进程重启
    → fpmsyncd.cpp: warmStartEnabled = sync.getWarmStartHelper().checkAndStart()
    → sync.getNHGMgr().getWarmRestartAssist().readNHGTablesToMap()
        → 读取 NHG_RIB_STATE_TABLE → tempRIBEntryTable（old 信息，标记 STALE）
        → 读取 NHG_SONIC_STATE_TABLE → 恢复 SonicIDAllocator::m_id_map
```

### 阶段四：warm reboot 期间（接收 Zebra 重推）

```
Zebra 重推 NHG → onNextHopGroupFullMsg()
    → sync.getNHGMgr().getWarmRestartAssist().insertNHGToMap(zebra_id, entry)
        → 对比 tempRIBEntryTable 中的 old 信息
        → 更新 new 信息，标记 NEW 或 SAME

Zebra 重推 Route → onRouteMsg()
    → sync.getNHGMgr().getWarmRestartAssist().cacheRouteWithZebraId(...)
        → 缓存到 m_route_cache
```

### 阶段五：reconcile（warm reboot 定时器到期）

```
warmStartTimer/eoiuHoldTimer 到期
    → sync.onWarmStartEnd(applStateDb)
        → sync.getNHGMgr().getWarmRestartAssist().reconcileAll()
            → reconcileNHG()
                1. 遍历 tempRIBEntryTable
                2. 删除 STALE 条目（从 NEXTHOP_GROUP_TABLE）
                3. 处理多跳 nexthop 依赖关系（用 new ID）
                4. 将合法条目迁移到 RIBEntryTable
            → reconcileRoutes()
                1. 遍历 m_route_cache
                2. 通过 NHGMgr 查询 zebra_id → nexthop fields
                3. 写入 ROUTE_TABLE
        → m_warmStartHelper.reconcile()  // 现有 route reconcile（保持兼容）
```

---

## Risks / Trade-offs

- **[风险] warm reboot 停止前时间不足** → 缓解：saveStateToDB 使用 pipeline 批量写入；接受极端情况下状态丢失。

- **[风险] Zebra 重推 ID 与旧 ID 映射错误** → 缓解：tempRIBEntryTable 同时保存 old/new ID，通过 hash key 对比确保映射正确。

- **[风险] 依赖关系重建失败** → 缓解：Zebra 重推的 NHG 带有完整依赖信息，按新 ID 重建即可。

- **[风险] Route reconcile 时 NHG 尚未就绪** → 缓解：reconcileAll() 保证先 NHG 后 Route 的顺序。

---

## Migration Plan

1. 部署新版 fpmsyncd（含持久化逻辑）后，首次正常重启时 State Table 为空，NHGMgr 走冷启动路径，行为与现有版本一致。
2. 后续 warm reboot 时：
   - 停止前：保存状态到 State Table
   - 启动后：从 State Table 恢复
3. 回滚：降级到旧版本时，旧版本不读取 State Table，冷启动路径不受影响。
4. 无需数据库 schema 版本迁移。

---

## Exploration-Driven Revisions

基于 Socratic Design Exploration（2026-04-20），以下决策已修订或新增：

### 修订决策

| 决策 | 修订内容 | 原因 |
|------|----------|------|
| D4 | reconcile 支持**多轮处理** | 处理 Zebra 推送顺序不一致问题 |
| D1 | 添加**降级路径** | 保存失败时走冷启动，记录日志 |
| I1 | 启动时**完整性检测** | 处理 State Table 部分写入失败 |

### 新增决策

| 决策 | 内容 | 理由 |
|------|------|------|
| N1 | **启动时清理 State Table** | 避免重复 warm reboot 时旧数据干扰 |
| N2 | **temp 表大小限制** | 防止内存溢出（可配置阈值，默认 100K 条目） |
| N3 | **Zebra 推送超时检测** | 长时间未收到关键 NHG 时触发错误处理 |
| N4 | **reconcile 取消机制** | warm reboot 取消时能恢复到正常状态 |

### 更新的 reconcile 算法

```cpp
void NHGWarmRestartAssist::reconcileNHG() {
    // 多轮处理，直到没有新条目可以处理
    bool progress = true;
    int round = 0;
    const int MAX_ROUNDS = 10;
    
    while (progress && round < MAX_ROUNDS) {
        progress = false;
        round++;
        
        for (auto& [zebra_id, entry] : m_temp_rib_table) {
            if (entry.state != State::PENDING) continue;
            
            // 检查依赖是否已处理
            if (hasUnresolvedDependencies(entry)) continue;
            
            // 处理当前条目
            processEntry(entry);
            progress = true;
        }
    }
    
    // 处理剩余条目（依赖无法解析的标记为错误）
    handleUnresolvedEntries();
    
    // 迁移到 RIBNHGTable
    migrateToRIBTable();
}
```

### 更新的启动流程

```cpp
void NHGWarmRestartAssist::readNHGTablesToMap() {
    // 1. 检测 State Table 完整性
    if (!checkStateTableIntegrity()) {
        SWSS_LOG_WARN("State Table incomplete, falling back to cold start");
        clearStateTables();
        return;
    }
    
    // 2. 检查 temp 表大小限制
    if (getStateTableEntryCount() > MAX_TEMP_ENTRIES) {
        SWSS_LOG_WARN("State Table too large, falling back to cold start");
        clearStateTables();
        return;
    }
    
    // 3. 读取到 temp 表（标记 STALE）
    // ...
    
    // 4. 恢复 Sonic ID
    // ...
}
```

---

## 配置项

### NHG Warm Reboot 开关

从 `CONFIG_DB` `DEVICE_METADATA|localhost` 读取：

| 配置项 | 类型 | 默认值 | 说明 |
|--------|------|--------|------|
| `nhg-warm-reboot-enabled` | string | `"enabled"` | 启用/禁用 NHG warm reboot 支持 |

**实现**：
```cpp
// fpmsyncd.cpp 启动时读取
std::string nhgWarmRebootEnabled;
deviceMetadataTable.hget("localhost", "nhg-warm-reboot-enabled", nhgWarmRebootEnabled);
bool nhgWarmRebootEnabled = (nhgWarmRebootEnabled != "disabled");
sync.getNHGMgr().setWarmRebootEnabled(nhgWarmRebootEnabled);
```

**降级行为**：当设置为 `"disabled"` 时：
- SIGTERM 时不保存 NHG 状态
- 启动时不恢复 NHG 状态
- 走冷启动路径，行为与旧版本一致

### SIGTERM 超时设置

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `SAVE_STATE_TIMEOUT_SEC` | 5 | saveStateToDB() 超时时间（秒）|

**理由**：
- SONiC docker 默认 graceful stop 超时通常为 10 秒
- pipeline flush 操作通常 < 1 秒
- 5 秒提供足够余量，同时避免阻塞整体 warm reboot

## State Table 命名（已对齐 schema.h）

根据 SONiC 命名规范（`APP_*_TABLE_NAME`）：

| 表名 | 宏定义 | 实际字符串 |
|------|--------|----------|
| NHG RIB State Table | `APP_NHG_RIB_STATE_TABLE_NAME` | `"NHG_RIB_STATE_TABLE"` |
| NHG Sonic State Table | `APP_NHG_SONIC_STATE_TABLE_NAME` | `"NHG_SONIC_STATE_TABLE"` |

**schema.h 添加**（需要 sonic-swss-common 仓库更新）：
```cpp
#define APP_NHG_RIB_STATE_TABLE_NAME    "NHG_RIB_STATE_TABLE"
#define APP_NHG_SONIC_STATE_TABLE_NAME  "NHG_SONIC_STATE_TABLE"
```

**注意**：这些定义需要在 sonic-swss-common 的 `common/schema.h` 中添加。在 fpmsyncd 代码中，我们将使用字符串字面量作为临时方案，直到 sonic-swss-common 更新完成。

## Implementation Notes

### Files Added/Modified
- `fpmsyncd/nhgwarmrestartassist.h` - NHGWarmRestartAssist class declaration with TempRIBEntry and RouteCacheEntry
- `fpmsyncd/nhgwarmrestartassist.cpp` - Implementation of state save/restore and reconcile logic
- `fpmsyncd/nhgmgr.h/cpp` - Integration with NHGWarmRestartAssist, added accessor methods for RIB/Sonic tables
- `fpmsyncd/routesync.h/cpp` - onWarmStartEnd integration, NHGMgr constructor update with app_state_pipeline
- `fpmsyncd/fpmsyncd.cpp` - SIGTERM handler for state persistence, warm start config reading
- `fpmsyncd/Makefile.am` - Added nhgwarmrestartassist.cpp to build
- `tests/mock_tests/fpmsyncd/test_nhg_warm_reboot.cpp` - Unit tests for TempRIBEntry and RouteCacheEntry

### Build Requirements
Requires sonic-swss-common update to add:
- `APP_NHG_RIB_STATE_TABLE_NAME`
- `APP_NHG_SONIC_STATE_TABLE_NAME`

Currently using string literals as temporary workaround.

### Key Implementation Details
1. **State Persistence**: On SIGTERM during warm reboot, NHG state is saved to APPL_STATE_DB
2. **State Recovery**: On startup, state is read from APPL_STATE_DB into temp tables
3. **Reconcile**: Multi-round dependency resolution for NHG entries, followed by route reconcile
4. **Configuration**: `nhg-warm-reboot-enabled` in DEVICE_METADATA controls feature enablement

## 信号处理

fpmsyncd 需新增 SIGTERM 信号处理：

```cpp
// fpmsyncd.cpp
void sigterm_handler(int signum) {
    if (WarmStart::isWarmStart() || WarmStart::isSystemWarmRebootEnabled()) {
        if (sync.getNHGMgr().isWarmRebootEnabled()) {
            sync.getNHGMgr().getWarmRestartAssist().saveStateToDB();
            pipeline.flush();
        }
    }
    exit(0);
}

// main() 中注册
struct sigaction sigact;
sigact.sa_handler = sigterm_handler;
sigemptyset(&sigact.sa_mask);
sigact.sa_flags = 0;
sigaction(SIGTERM, &sigact, NULL);
```
