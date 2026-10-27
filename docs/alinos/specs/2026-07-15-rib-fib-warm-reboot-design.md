# Design: RIB/FIB NHG Manager Warm Reboot Support

## Overview

为 fpmsyncd 的 NHG Manager 添加 warm reboot 支持，确保 `NEXTHOP_GROUP_TABLE` 和 `PIC_CONTEXT_TABLE` 在 warm reboot 前后正确 reconcile。核心目标：不变的 NHG 不删不重建，复用 sonic ID，避免 orchagent/SAI 层不必要的硬件表项变更。

## Architecture

### 整体流程

```
Graceful Shutdown:
  fpmsyncd 收到 shutdown 信号
    → NHGMgr::saveWarmRestartState()
      - 对有 Sonic 对象的 entry 写入 NHG_FULL_STATE_TABLE（sonicID, af, json, picObjID）
      - 存储 ID allocator 的 next_id
    → RouteSync 正常关闭（已有逻辑）

Warm Start:
  fpmsyncd 启动，检测 warm reboot 模式
    → NHGMgr::loadWarmRestartState()
      - 从 NHG_FULL_STATE_TABLE 加载 sonicID 映射
      - 从 APP_DB 加载 NEXTHOP_GROUP_TABLE 所有表项（sonicKey → FV vector）
      - 恢复 ID allocator next_id
    → FpmLink::processFpmMessage() 中统一拦截：
      - NHG 消息 → m_nhg_raw_buffer
      - 路由消息 → m_route_raw_buffer
      - 其他消息 → 正常 dispatch

Warm End（EOIU + holdTimer 或 warmStartTimer 到期）:
  RouteSync::onWarmStartEnd()
    → Phase 1: reconcileNormalSingleHopNHGs()
      - 回放 NHG buffer 中的普通单跳消息，直接 addNHGFull()
    → Phase 2: reconcileNHGsWithSonicObj()
      - 回放 NHG buffer 中的 SRv6 单跳 + 多跳消息
      - 临时 RIBNHGEntry + setEntry() 干跑计算 FV vector
      - FV vector 匹配 APP_DB 旧表项 → 复用 sonicID
      - 无匹配 → 新分配 sonicID，写 APP_DB
      - 旧表项未匹配 → 删除
    → Phase 3: replayBufferedRoutes()
      - 回放路由 buffer，通过正常 dispatch 路径处理
      - 此时 NHG Manager 已就绪
    → Phase 4: m_warmStartHelper.reconcile()
      - 路由 reconcile（现有逻辑）
    → Phase 5: pipeline flush 恢复
```

### 组件关系

```
                    ┌─────────────────────┐
                    │   fpmsyncd main()   │
                    │   (warm start FSM)  │
                    └──────────┬──────────┘
                               │
        ┌──────────────────────┼────────────────────┐
        ▼                      ▼                    ▼
 ┌─────────────┐    ┌────────────────┐    ┌──────────────────┐
 │  FpmLink     │    │   RouteSync    │    │ WarmStartHelper  │
 │ (拦截+缓存) │───>│ (协调 reconcile│    │ (路由 reconcile) │
 │              │    │  raw buffers)  │    │                  │
 └──────────────┘    └───────┬────────┘    └──────────────────┘
                             │
                    ┌────────┴────────┐
                    │    NHGMgr       │
                    │ (NHG reconcile) │
                    │  save/load/FSM  │
                    └────────┬────────┘
                             │
                    ┌────────┴────────┐
                    │  RIBNHGTable    │
                    │  PICContentTable│
                    └────────┬────────┘
                             │
                             ▼
                    ┌─────────────────┐
                    │     APP_DB      │
                    │ NHG_TABLE       │
                    │ PIC_TABLE       │
                    │ ROUTE_TABLE     │
                    └─────────────────┘
```

## Detailed Design

### 1. 消息拦截与缓存（FpmLink 层）

在 `FpmLink::processFpmMessage()` 中，现有 dispatch 逻辑前增加 warm restart 拦截：

```cpp
// FpmLink::processFpmMessage() — 在 isRawProcessing() 检查前
if (m_routesync->isNhgWarmRestartInProgress()) {
    uint16_t type = nl_hdr->nlmsg_type;
    
    if (type == RTM_NEWNHGFIB || type == RTM_DELNHGFIB) {
        m_routesync->bufferNHGRaw(nl_hdr);
        continue;
    }
    
    if (type == RTM_NEWROUTE || type == RTM_DELROUTE ||
        type == RTM_NEWSRV6VPNROUTE || type == RTM_DELSRV6VPNROUTE) {
        m_routesync->bufferRouteRaw(nl_hdr);
        continue;
    }
    
    // RTM_NEWNEXTHOP, RTM_NEWLINK 等其他消息正常 dispatch
}
```

RouteSync 新增缓存接口：

```cpp
// routesync.h 新增
vector<vector<uint8_t>> m_nhg_raw_buffer;
vector<vector<uint8_t>> m_route_raw_buffer;

void bufferNHGRaw(struct nlmsghdr* nlh) {
    m_nhg_raw_buffer.emplace_back((uint8_t*)nlh, (uint8_t*)nlh + nlh->nlmsg_len);
}

void bufferRouteRaw(struct nlmsghdr* nlh) {
    m_route_raw_buffer.emplace_back((uint8_t*)nlh, (uint8_t*)nlh + nlh->nlmsg_len);
}

bool isNhgWarmRestartInProgress() const;
```

### 2. Graceful Shutdown — NHG 状态持久化

在 fpmsyncd 收到 warm shutdown 信号时调用。扩展现有 NHG_FULL_STATE_TABLE（APPL_STATE_DB）：

```cpp
void NHGMgr::saveWarmRestartState() {
    // 1. 对有 Sonic 对象的 NHG entry，存储 sonicID 映射
    for (auto& [ribId, entry] : m_rib_nhg_table->getAllEntries()) {
        if (!entry->hasSonicNHGObj() && !entry->hasSonicPICObj())
            continue;
        
        vector<FieldValueTuple> fvs;
        fvs.emplace_back("sonic_nhg_id", to_string(entry->getSonicNHGObjID()));
        fvs.emplace_back("af", to_string(entry->getAF()));
        fvs.emplace_back("json", entry->getNHGFullJSON());
        if (entry->hasSonicPICObj())
            fvs.emplace_back("pic_context_id", to_string(entry->getSonicPICObjID()));
        
        m_nhgFullStateTable.set(to_string(ribId.id), fvs);
    }
    
    // 2. 存储 ID allocator 状态
    vector<FieldValueTuple> allocFvs;
    allocFvs.emplace_back("next_nhg_id", to_string(m_sonic_id_manager->getNextNHGId()));
    allocFvs.emplace_back("next_pic_id", to_string(m_sonic_id_manager->getNextPICId()));
    m_nhgFullStateTable.set("NHG_ID_ALLOCATOR", allocFvs);
}
```

NHG_FULL_STATE_TABLE 新增字段（向后兼容，不影响现有字段）：

| 字段 | 状态 | 说明 |
|------|------|------|
| sonic_nhg_id | 已有 | Sonic NHG 对象 ID |
| pic_context_id | 已有 | PIC 对象 ID |
| json | 已有 | NextHopGroupFull JSON |
| af | **新增** | 地址族 |
| NHG_ID_ALLOCATOR 键 | **新增** | ID 分配器状态 |

### 3. Warm Start — NHG 加载

```cpp
void NHGMgr::loadWarmRestartState() {
    // 数据源 1: NHG_FULL_STATE_TABLE (APPL_STATE_DB)
    //   → 构建 sonicID 到 ribID 的映射
    //   → 恢复 ID allocator 的 next_id
    
    // 数据源 2: APP_DB NEXTHOP_GROUP_TABLE
    //   → 加载所有表项的 FV vector
    //   → 构建 m_appdb_nhg_fvs: map<string/*NHG_key*/, AppDbNHGEntry>
    //   AppDbNHGEntry = {sonicObjectID, vector<FieldValueTuple>, matched=false}
    
    // 设置 NHG warm restart state = RESTORED
}
```

### 4. NHG Reconcile

#### Phase 1: 普通单跳 NHG

```cpp
void NHGMgr::reconcileNormalSingleHopNHGs(vector<vector<uint8_t>>& nhgBuffer) {
    for (auto& rawMsg : nhgBuffer) {
        struct nlmsghdr* nlh = (struct nlmsghdr*)rawMsg.data();
        // 解析为 NextHopGroupFull
        NextHopGroupFull nhg = parseNHGFromNlMsg(nlh);
        uint8_t af = extractAF(nlh);
        
        if (nlh->nlmsg_type == RTM_DELNHGFIB) continue;
        
        if (isNormalSingleHop(nhg)) {
            addNHGFull(nhg, af);  // 直接添加，不产生 Sonic 对象
            markAsReconciled(nhg.id);
        }
    }
}
```

#### Phase 2: 需 Sonic 对象的 NHG（SRv6 单跳 + 多跳）

```cpp
void NHGMgr::reconcileNHGsWithSonicObj(vector<vector<uint8_t>>& nhgBuffer) {
    // Step A: 对未 reconcile 的 NHG，创建临时 entry 干跑计算 FV
    map<ribID, TempReconcileEntry> tempMap;
    
    for (auto& rawMsg : nhgBuffer) {
        NextHopGroupFull nhg = parseNHGFromNlMsg(rawMsg);
        if (isReconciled(nhg.id) || isDeletion(rawMsg)) continue;
        
        // 创建临时 RIBNHGEntry，setEntry() 计算 FV vector
        // table 指针指向正式 RIBNHGTable（用于 resolve group member 查找）
        RIBNHGEntry tmpEntry(m_rib_nhg_table);
        tmpEntry.setEntry(nhg, af);
        
        tempMap[nhg.id] = {
            .nhg = nhg,
            .af = af,
            .fvVector = tmpEntry.getFvVector(),
            .isSingle = tmpEntry.isSingleNexthop(),
            .sonicObjType = tmpEntry.getSonicObjType(),
        };
    }
    
    // Step B: FV vector 匹配
    for (auto& [ribId, entry] : tempMap) {
        string fvHash = computeFvHash(entry.fvVector);
        auto match = findMatchingAppDbEntry(fvHash);
        
        if (match) {
            entry.reuseSonicId = match->sonicId;
            match->matched = true;
        }
    }
    
    // Step C: 拓扑排序 + 写入
    auto sorted = topologicalSort(tempMap);
    
    for (auto ribId : sorted) {
        auto& entry = tempMap[ribId];
        if (entry.reuseSonicId.isValid()) {
            addNHGFullWithSonicId(entry.nhg, entry.af, entry.reuseSonicId);
            // 不写 APP_DB（对象已在，FV 相同）
        } else {
            addNHGFull(entry.nhg, entry.af);
            // 正常路径：新分配 ID，写 APP_DB
        }
    }
    
    // Step D: 处理缓存中的删除消息
    for (auto& rawMsg : nhgBuffer) {
        if (isDeletion(rawMsg)) {
            NextHopGroupFull nhg = parseNHGFromNlMsg(rawMsg);
            // 标记对应的旧表项为待删除
        }
    }
    
    // Step E: 清理未匹配的旧 APP_DB 表项
    for (auto& [key, entry] : m_appdb_nhg_fvs) {
        if (!entry.matched) {
            removeFromAppDb(key);
        }
    }
    
    // 设置 NHG warm restart state = RECONCILED
}
```

#### FV Hash 匹配逻辑

```cpp
string computeFvHash(const vector<FieldValueTuple>& fvs) {
    // 将 FV vector 排序后拼接为字符串
    // 排序确保字段顺序不影响匹配
    vector<FieldValueTuple> sorted = fvs;
    sort(sorted.begin(), sorted.end());
    
    string result;
    for (auto& [field, value] : sorted) {
        result += field + "=" + value + ";";
    }
    return result;
}

AppDbNHGEntry* findMatchingAppDbEntry(const string& fvHash) {
    for (auto& [key, entry] : m_appdb_nhg_fvs) {
        if (!entry.matched && computeFvHash(entry.fvVector) == fvHash) {
            return &entry;
        }
    }
    return nullptr;
}
```

#### addNHGFullWithSonicId — 复用 sonicID 的添加路径

```cpp
void NHGMgr::addNHGFullWithSonicId(NextHopGroupFull& nhg, uint8_t af,
                                     sonicObjectID reuseId) {
    // 和 addNHGFull 类似，但：
    // 1. 使用传入的 reuseId 而非调用 allocateID()
    // 2. 跳过 writeToDB()（APP_DB 表项已存在且内容相同）
    // 3. 正常维护 m_nhg_map、m_created_shared_nhg_map 等内存结构
}
```

### 5. 路由 Reconcile 协调

```cpp
void RouteSync::onWarmStartEnd(DBConnector& applStateDb) {
    // Phase 1+2: NHG reconcile
    m_rib_fib_nhg_mgr.reconcileNormalSingleHopNHGs(m_nhg_raw_buffer);
    m_rib_fib_nhg_mgr.reconcileNHGsWithSonicObj(m_nhg_raw_buffer);
    m_nhg_raw_buffer.clear();
    
    // Phase 3: 回放缓存路由
    replayBufferedRoutes();
    
    // Phase 4: 路由 reconcile
    if (m_warmStartHelper.inProgress()) {
        m_warmStartHelper.reconcile();
    }
}

void RouteSync::replayBufferedRoutes() {
    for (auto& rawMsg : m_route_raw_buffer) {
        struct nlmsghdr* nlh = (struct nlmsghdr*)rawMsg.data();
        
        // 根据消息类型决定 dispatch 路径
        if (FpmLink::isRawProcessing(nlh) ||
            nlh->nlmsg_type == RTM_NEWSRV6VPNROUTE ||
            nlh->nlmsg_type == RTM_DELSRV6VPNROUTE) {
            onMsgRaw(nlh);  // raw 路径
        } else {
            // libnl 路径：重新 convert 并 dispatch
            nl_msg* msg = nlmsg_convert(nlh);
            if (msg) {
                NetDispatcher::getInstance().onNetlinkMessage(msg);
                nlmsg_free(msg);
            }
        }
    }
    m_route_raw_buffer.clear();
}
```

### 6. NHGMgr Warm Restart FSM

```
INITIALIZED → RESTORED → NHG_RECONCILING → RECONCILED
```

通过 `WarmStart::setWarmStartState()` 向 STATE_DB 报告状态。

```cpp
// NHGMgr 新增 warm restart 状态
enum NhgWarmRestartState {
    NHG_WR_NONE,
    NHG_WR_INITIALIZED,
    NHG_WR_RESTORED,
    NHG_WR_RECONCILING,
    NHG_WR_RECONCILED,
};
NhgWarmRestartState m_nhgWrState = NHG_WR_NONE;

bool isNhgWarmRestartInProgress() const {
    return m_nhgWrState > NHG_WR_NONE && m_nhgWrState < NHG_WR_RECONCILED;
}
```

### 7. NHG 分类规则

| 类型 | 判断条件 | 处理方式 | APP_DB 操作 |
|------|---------|---------|------------|
| 普通单跳 NHG | `type=NHG_NORMAL && isSingleHop()` | Phase 1: 直接 addNHGFull | 无（不产生 Sonic 对象） |
| SRv6 单跳 NHG | `type=NHG_WITH_SRV6_PIC_CONTEXT && isSingleHop()` | Phase 2: 干跑匹配 | 匹配→跳过; 不匹配→写入 |
| 多跳 NHG | `!isSingleHop()` | Phase 2: 干跑匹配 + 拓扑排序 | 同上 |

### 8. SRv6 VPN 支持

- SRv6 单跳 NHG 参与 Phase 2 reconcile（因为会创建 Sonic NHG 和 PIC 对象）
- PIC context 作为 NHG entry 附属，在 `addNHGFull()` / `addNHGFullWithSonicId()` 中同步处理
- Shutdown 时存储 `pic_context_id` 以便复用
- SRv6 VPN 路由（RTM_NEWSRV6VPNROUTE）在 FpmLink 层同样被拦截缓存

## Data Model

### 扩展的 DB 表

| 表名 | DB | 变更 |
|------|-----|------|
| `NHG_FULL_STATE_TABLE` | APPL_STATE_DB | 新增 `af` 字段；新增 `NHG_ID_ALLOCATOR` 键 |

### Reconcile 期间行为

| 表名 | DB | 行为 |
|------|-----|------|
| `NEXTHOP_GROUP_TABLE` | APP_DB | NHG 消息缓存不写入；reconcile 时匹配的不动，新增的写入，过期的删除 |
| `PIC_CONTEXT_TABLE` | APP_DB | 同上 |
| `ROUTE_TABLE` | APP_DB | 路由消息缓存不写入；NHG reconcile 后回放，走现有 WarmStartHelper reconcile |

## Key Files to Modify

| 文件 | 修改内容 |
|------|---------|
| `fpmsyncd/fpmlink.cpp` | processFpmMessage() 增加 warm restart 拦截逻辑 |
| `fpmsyncd/nhgmgr.h` | 新增 warm restart 数据结构、FSM、reconcile 方法声明 |
| `fpmsyncd/nhgmgr.cpp` | 实现 save/load/reconcile 逻辑、addNHGFullWithSonicId |
| `fpmsyncd/routesync.h` | 新增 raw buffer、bufferNHGRaw/bufferRouteRaw/replayBufferedRoutes |
| `fpmsyncd/routesync.cpp` | 修改 onWarmStartEnd()、新增 replayBufferedRoutes() |
| `fpmsyncd/fpmsyncd.cpp` | 修改 warm start 初始化调用 loadWarmRestartState() |

## Risks / Trade-offs

| 风险 | 可能性 | 影响 | 缓解措施 |
|------|--------|------|---------|
| NHG 依赖关系成环 | Low | High | 拓扑排序检测环，有环跳过并告警 |
| FV vector 匹配误判 | Low | High — 复用了错误的 sonicID | FV 匹配用精确字符串比较，不用 hash |
| 临时 RIBNHGEntry 的 resolve 依赖单跳已就绪 | — | High | Phase 1 先处理普通单跳，Phase 2 按拓扑排序 |
| 路由回放时 nlmsg_convert 失败 | Low | Medium | 错误日志 + 跳过该路由（不影响其他路由） |
| NHG_FULL_STATE_TABLE 扩展影响调试工具 | Low | Low | 新增字段向后兼容 |
| warmStartTimer 到期时 NHG/路由消息未全部到达 | Low | Medium | 保持现有 timer 逻辑作为兜底 |

## GBrain 参考

无相关历史记录。
