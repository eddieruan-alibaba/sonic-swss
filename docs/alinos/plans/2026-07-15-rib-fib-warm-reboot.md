# RIB/FIB NHG Manager Warm Reboot Implementation Plan

> **For agentic workers:** Use /alinos.subagent-dev (recommended) or /alinos.executing-plans to implement this plan task-by-task.

**Goal:** 为 fpmsyncd NHG Manager 添加 warm reboot 支持，实现 NHG 表项在 warm reboot 前后的 reconcile，复用 sonic ID 避免不必要的硬件表项变更。

**Architecture:** FpmLink 层统一拦截 NHG 和路由消息到 raw buffer。NHGMgr 自管 warm restart FSM，通过临时 RIBNHGEntry 干跑计算 FV vector 并与 APP_DB 旧表项精确匹配来复用 sonic ID。NHG reconcile 完成后回放缓存路由，最后走现有 WarmStartHelper 路由 reconcile。

**Tech Stack:** C++ (sonic-swss)，Redis (APPL_DB / APPL_STATE_DB)，swss::ProducerStateTable / swss::Table，netlink (nlmsghdr)

---

### Task 1: NHGMgr Warm Restart FSM 和数据结构

**Files:**
- Modify: `fpmsyncd/nhgmgr.h`
- Modify: `fpmsyncd/nhgmgr.cpp`

- [ ] **Step 1: 在 nhgmgr.h 中添加 warm restart 枚举和数据结构**

在 `nhgmgr.h` 的 NHGMgr 类中添加以下内容（private section）：

```cpp
// Warm restart FSM states
enum NhgWarmRestartState {
    NHG_WR_NONE,
    NHG_WR_INITIALIZED,
    NHG_WR_RESTORED,
    NHG_WR_RECONCILING,
    NHG_WR_RECONCILED,
};

// Saved NHG info loaded from NHG_FULL_STATE_TABLE
struct SavedNHGInfo {
    sonicObjectID sonicId;
    sonicObjectID picObjId;
    uint8_t af;
};

// APP_DB NHG entry for matching
struct AppDbNHGEntry {
    sonicObjectID sonicId;
    std::vector<swss::FieldValueTuple> fvVector;
    bool matched = false;
};

// Temp entry used during reconcile
struct TempReconcileEntry {
    fib::NextHopGroupFull nhg;
    uint8_t af;
    std::vector<swss::FieldValueTuple> fvVector;
    sonicObjectID reuseSonicId;
    sonicObjectID reusePicObjId;
    bool needsSonicObj = false;
};

NhgWarmRestartState m_nhgWrState = NHG_WR_NONE;
std::map<sonicObjectID, SavedNHGInfo> m_saved_nhg_infos;
std::map<std::string, AppDbNHGEntry> m_appdb_nhg_fvs;
std::set<ribID> m_reconciled_ids;
```

在 public section 添加方法声明：

```cpp
bool isNhgWarmRestartInProgress() const;
void initWarmRestart();
void saveWarmRestartState(swss::Table &stateTable);
void loadWarmRestartState(swss::Table &stateTable, swss::Table &appDbNhgTable);
void reconcileNormalSingleHopNHGs(std::vector<std::vector<uint8_t>> &nhgBuffer);
void reconcileNHGsWithSonicObj(std::vector<std::vector<uint8_t>> &nhgBuffer);
int addNHGFullWithSonicId(const fib::NextHopGroupFull &nhg, uint8_t af,
                           sonicObjectID reuseNhgId, sonicObjectID reusePicId);
```

- [ ] **Step 2: 在 nhgmgr.cpp 中实现 FSM 查询方法**

```cpp
bool NHGMgr::isNhgWarmRestartInProgress() const {
    return m_nhgWrState > NHG_WR_NONE && m_nhgWrState < NHG_WR_RECONCILED;
}

void NHGMgr::initWarmRestart() {
    m_nhgWrState = NHG_WR_INITIALIZED;
    m_saved_nhg_infos.clear();
    m_appdb_nhg_fvs.clear();
    m_reconciled_ids.clear();
}
```

- [ ] **Step 3: 编译验证**

```bash
cd /home/admin/workspace/rib-fib/sonic-swss && make -C fpmsyncd -j4 2>&1 | tail -20
```

- [ ] **Step 4: Commit**

```
NHG warm reboot: add FSM enum, data structures, and method declarations
```

---

### Task 2: Graceful Shutdown — saveWarmRestartState

**Files:**
- Modify: `fpmsyncd/nhgmgr.cpp`
- Modify: `fpmsyncd/routesync.cpp` (添加 shutdown 调用点)

- [ ] **Step 1: 实现 saveWarmRestartState**

在 nhgmgr.cpp 中：

```cpp
void NHGMgr::saveWarmRestartState(swss::Table &stateTable) {
    SWSS_LOG_NOTICE("NHG warm restart: saving state, %zu entries in nhg_map",
                     m_rib_nhg_table->getAllEntries().size());

    for (auto &[ribId, entry] : m_rib_nhg_table->getAllEntries()) {
        if (!entry->needCreateSonicObject() && !entry->hasSonicPICObj())
            continue;

        std::vector<swss::FieldValueTuple> fvs;
        fvs.emplace_back("sonic_nhg_id", std::to_string(entry->getSonicObjIDNum()));
        fvs.emplace_back("af", std::to_string(entry->getAddressFamily()));

        // getNHG().toJsonStr() 获取完整 JSON
        fvs.emplace_back("json", entry->getNHG().toJsonStr());

        if (entry->hasSonicPICObj()) {
            fvs.emplace_back("pic_context_id", std::to_string(entry->getSonicPICObjIDNum()));
        }

        stateTable.set(std::to_string(ribId.id), fvs);
    }

    // 存储 ID allocator 状态
    std::vector<swss::FieldValueTuple> allocFvs;
    allocFvs.emplace_back("next_nhg_id",
        std::to_string(m_sonic_id_manager->getNextID(SONIC_NHG_OBJ_TYPE_NHG_NORMAL)));
    allocFvs.emplace_back("next_pic_id",
        std::to_string(m_sonic_id_manager->getNextID(SONIC_NHG_OBJ_TYPE_NHG_WITH_SRV6_PIC_CONTEXT)));
    stateTable.set("NHG_ID_ALLOCATOR", allocFvs);

    SWSS_LOG_NOTICE("NHG warm restart: state saved");
}
```

> 注意：需要确认 `m_rib_nhg_table` 是否有 `getAllEntries()` 方法返回 `m_nhg_map` 的引用。如果没有，需要在 `RIBNHGTable` 中添加 `const map<ribID, RIBNHGEntry*>& getNhgMap() const { return m_nhg_map; }` 访问器。同样需要确认 `SonicIDMgr` 是否有 `getNextID()` 方法，如果没有需要添加。

- [ ] **Step 2: 在 RIBNHGTable 中添加 getNhgMap 访问器（如需要）**

在 nhgmgr.h 的 RIBNHGTable public section：

```cpp
const std::map<ribID, RIBNHGEntry*>& getNhgMap() const { return m_nhg_map; }
```

在 SonicIDAllocator 中添加 getNextID（如需要）：

```cpp
uint32_t getNextID() const { return g_id; }
```

- [ ] **Step 3: 编译验证**

- [ ] **Step 4: Commit**

```
NHG warm reboot: implement saveWarmRestartState for graceful shutdown
```

---

### Task 3: Warm Start — loadWarmRestartState

**Files:**
- Modify: `fpmsyncd/nhgmgr.cpp`

- [ ] **Step 1: 实现 loadWarmRestartState**

```cpp
void NHGMgr::loadWarmRestartState(swss::Table &stateTable, swss::Table &appDbNhgTable) {
    SWSS_LOG_NOTICE("NHG warm restart: loading state");

    // 1. 从 NHG_FULL_STATE_TABLE 加载 saved NHG info
    std::vector<std::string> keys;
    stateTable.getKeys(keys);

    for (auto &key : keys) {
        if (key == "NHG_ID_ALLOCATOR") {
            // 恢复 ID allocator
            std::string nextNhgId, nextPicId;
            stateTable.hget(key, "next_nhg_id", nextNhgId);
            stateTable.hget(key, "next_pic_id", nextPicId);
            if (!nextNhgId.empty()) {
                m_sonic_id_manager->setNextID(SONIC_NHG_OBJ_TYPE_NHG_NORMAL,
                                               (uint32_t)stoul(nextNhgId));
            }
            if (!nextPicId.empty()) {
                m_sonic_id_manager->setNextID(SONIC_NHG_OBJ_TYPE_NHG_WITH_SRV6_PIC_CONTEXT,
                                               (uint32_t)stoul(nextPicId));
            }
            continue;
        }

        std::string sonicIdStr, picIdStr, afStr;
        stateTable.hget(key, "sonic_nhg_id", sonicIdStr);
        stateTable.hget(key, "af", afStr);
        stateTable.hget(key, "pic_context_id", picIdStr);

        if (sonicIdStr.empty()) continue;

        SavedNHGInfo info;
        info.sonicId = sonicObjectID((uint32_t)stoul(sonicIdStr));
        info.picObjId = picIdStr.empty() ? sonicObjectID(0)
                                          : sonicObjectID((uint32_t)stoul(picIdStr));
        info.af = afStr.empty() ? 0 : (uint8_t)stoul(afStr);

        m_saved_nhg_infos[info.sonicId] = info;

        // 标记 sonicID 为已使用
        m_sonic_id_manager->markIDUsed(SONIC_NHG_OBJ_TYPE_NHG_NORMAL, info.sonicId);
        if (info.picObjId.id != 0) {
            m_sonic_id_manager->markIDUsed(SONIC_NHG_OBJ_TYPE_NHG_WITH_SRV6_PIC_CONTEXT,
                                            info.picObjId);
        }
    }

    // 2. 从 APP_DB 加载 NEXTHOP_GROUP_TABLE
    std::vector<std::string> nhgKeys;
    appDbNhgTable.getKeys(nhgKeys);

    for (auto &nhgKey : nhgKeys) {
        std::vector<swss::FieldValueTuple> fvs;
        appDbNhgTable.get(nhgKey, fvs);

        // 解析 sonicID from key (格式: "NHG_<id>" 或纯数字)
        AppDbNHGEntry entry;
        entry.fvVector = fvs;
        entry.matched = false;

        // 从 saved_nhg_infos 查找对应的 sonicId
        // nhgKey 格式取决于 RIBNHGTable::writeToDB 的 key 构造
        entry.sonicId = sonicObjectID(0);  // 需要从 key 解析

        m_appdb_nhg_fvs[nhgKey] = entry;
    }

    m_nhgWrState = NHG_WR_RESTORED;
    SWSS_LOG_NOTICE("NHG warm restart: loaded %zu saved NHG infos, %zu APP_DB entries",
                     m_saved_nhg_infos.size(), m_appdb_nhg_fvs.size());
}
```

> 注意：需要确认 RIBNHGTable::writeToDB 使用的 key 格式，以正确解析 sonicID。也需要在 SonicIDAllocator 中添加 `setNextID()` 和 `markIDUsed()` 方法。

- [ ] **Step 2: 在 SonicIDAllocator 中添加辅助方法（如需要）**

```cpp
void setNextID(uint32_t nextId) { g_id = nextId; }
void markIDUsed(sonicObjectID id) { m_id_map[id] = true; }
```

- [ ] **Step 3: 编译验证**

- [ ] **Step 4: Commit**

```
NHG warm reboot: implement loadWarmRestartState for warm start loading
```

---

### Task 4: FpmLink 层消息拦截

**Files:**
- Modify: `fpmsyncd/fpmlink.cpp`
- Modify: `fpmsyncd/routesync.h`
- Modify: `fpmsyncd/routesync.cpp`

- [ ] **Step 1: 在 routesync.h 中添加 raw buffer 和缓存方法**

在 RouteSync 类的 private section 添加：

```cpp
std::vector<std::vector<uint8_t>> m_nhg_raw_buffer;
std::vector<std::vector<uint8_t>> m_route_raw_buffer;
```

在 public section 添加：

```cpp
void bufferNHGRaw(struct nlmsghdr *nlh);
void bufferRouteRaw(struct nlmsghdr *nlh);
bool isNhgWarmRestartInProgress() const;
void replayBufferedRoutes();
```

- [ ] **Step 2: 在 routesync.cpp 中实现缓存方法**

```cpp
void RouteSync::bufferNHGRaw(struct nlmsghdr *nlh) {
    m_nhg_raw_buffer.emplace_back(
        reinterpret_cast<uint8_t*>(nlh),
        reinterpret_cast<uint8_t*>(nlh) + nlh->nlmsg_len);
}

void RouteSync::bufferRouteRaw(struct nlmsghdr *nlh) {
    m_route_raw_buffer.emplace_back(
        reinterpret_cast<uint8_t*>(nlh),
        reinterpret_cast<uint8_t*>(nlh) + nlh->nlmsg_len);
}

bool RouteSync::isNhgWarmRestartInProgress() const {
    return m_rib_fib_nhg_mgr.isNhgWarmRestartInProgress();
}
```

- [ ] **Step 3: 在 fpmlink.cpp 的 processFpmMessage() 中添加拦截逻辑**

在 `FpmLink::processFpmMessage()` 的 for 循环内，在 `isRawProcessing(nl_hdr)` 调用之前，添加：

```cpp
// Warm restart: intercept NHG and route messages
if (m_routesync->isNhgWarmRestartInProgress()) {
    uint16_t nlmsg_type = nl_hdr->nlmsg_type;

    if (nlmsg_type == RTM_NEWNHGFIB || nlmsg_type == RTM_DELNHGFIB) {
        m_routesync->bufferNHGRaw(nl_hdr);
        continue;
    }

    if (nlmsg_type == RTM_NEWROUTE || nlmsg_type == RTM_DELROUTE ||
        nlmsg_type == RTM_NEWSRV6VPNROUTE || nlmsg_type == RTM_DELSRV6VPNROUTE) {
        m_routesync->bufferRouteRaw(nl_hdr);
        continue;
    }
}
```

注意：这段代码必须在 `nlmsg_convert(nl_hdr)` 调用之前，因为我们要跳过后续的 convert + dispatch 逻辑。查看现有代码结构，`nlmsg_convert` 在 isRawProcessing 检查之后调用。所以拦截逻辑应放在 for 循环体的最开头。

但实际看代码，`nlmsg_convert` 在所有分支之前就被调用了（line 278）。所以需要调整：将拦截逻辑放在 `nlmsg_convert` 之前，并在被拦截时 `continue` 跳过 `nlmsg_convert` 和后续逻辑。

```cpp
for (; NLMSG_OK(nl_hdr, msg_len); nl_hdr = NLMSG_NEXT(nl_hdr, msg_len))
{
    // === 新增：warm restart 拦截 ===
    if (m_routesync->isNhgWarmRestartInProgress()) {
        uint16_t nlmsg_type = nl_hdr->nlmsg_type;
        if (nlmsg_type == RTM_NEWNHGFIB || nlmsg_type == RTM_DELNHGFIB) {
            m_routesync->bufferNHGRaw(nl_hdr);
            continue;
        }
        if (nlmsg_type == RTM_NEWROUTE || nlmsg_type == RTM_DELROUTE ||
            nlmsg_type == RTM_NEWSRV6VPNROUTE || nlmsg_type == RTM_DELSRV6VPNROUTE) {
            m_routesync->bufferRouteRaw(nl_hdr);
            continue;
        }
    }
    // === 新增结束 ===

    bool isRaw = isRawProcessing(nl_hdr);
    nl_msg *msg = nlmsg_convert(nl_hdr);
    // ... 现有逻辑 ...
}
```

- [ ] **Step 4: 编译验证**

- [ ] **Step 5: Commit**

```
NHG warm reboot: add FpmLink message interception and raw buffers
```

---

### Task 5: NHG Reconcile — Phase 1 (普通单跳)

**Files:**
- Modify: `fpmsyncd/nhgmgr.cpp`

- [ ] **Step 1: 实现 NHG raw 消息解析辅助函数**

在 nhgmgr.cpp 中添加（或在 routesync.cpp 中，取决于 NHG 解析逻辑的位置）：

需要复用 `onNextHopGroupFullMsg` 中的解析逻辑。最干净的方式是将 NHG 消息解析提取为独立函数：

```cpp
// 在 routesync.cpp 或 nhgmgr.cpp 中
static int parseNHGFromRawMsg(const std::vector<uint8_t> &rawMsg,
                               fib::NextHopGroupFull &nhg,
                               uint8_t &af, uint16_t &msgType) {
    struct nlmsghdr *nlh = (struct nlmsghdr *)rawMsg.data();
    msgType = nlh->nlmsg_type;

    // 复用 onNextHopGroupFullMsg 中的解析逻辑
    // 提取 nhmsg header, 解析 rtattr, 获取 NHA_JSON_STR
    // 解析 JSON 为 NextHopGroupFull
    // ...
    return 0;
}
```

> 具体实现需要从 `RouteSync::onNextHopGroupFullMsg()` 中提取解析代码。考虑到 onNextHopGroupFullMsg 同时做解析和调用 addNHGFull，最好将解析部分提取为独立方法。

- [ ] **Step 2: 实现 reconcileNormalSingleHopNHGs**

```cpp
void NHGMgr::reconcileNormalSingleHopNHGs(std::vector<std::vector<uint8_t>> &nhgBuffer) {
    SWSS_LOG_NOTICE("NHG warm restart Phase 1: reconciling normal single-hop NHGs");

    for (auto &rawMsg : nhgBuffer) {
        fib::NextHopGroupFull nhg;
        uint8_t af;
        uint16_t msgType;

        if (parseNHGFromRawMsg(rawMsg, nhg, af, msgType) != 0) {
            SWSS_LOG_ERROR("Failed to parse NHG raw message");
            continue;
        }

        if (msgType == RTM_DELNHGFIB) continue;

        // 普通单跳：type=NORMAL 且 nh_grp_full_list 为空（或只有 1 个成员且 num_direct==0）
        bool isReceived = nhg.nhg_flags & NEXTHOP_GROUP_RECEIVED;
        bool hasGroup = !nhg.nh_grp_full_list.empty();
        bool hasSrv6 = (nhg.nh_srv6 != nullptr);

        if (!hasGroup && !hasSrv6 && !isReceived) {
            // 这是一个普通单跳 NHG，不会创建 Sonic 对象
            addNHGFull(nhg, af);
            m_reconciled_ids.insert(ribID(nhg.id));
        }
    }

    SWSS_LOG_NOTICE("NHG warm restart Phase 1: reconciled %zu normal single-hop NHGs",
                     m_reconciled_ids.size());
}
```

> 注意：判断"普通单跳"的精确条件需要和 `checkNeedCreateSonicNHGObj()` 中的逻辑保持一致。上面的判断是简化版，实际需要在 setEntry 后检查 `needCreateSonicObject()` 返回 false 来确认。但因为 Phase 1 不做干跑，可以直接调用 addNHGFull 让内部逻辑判断。关键是 addNHGFull 对普通单跳不会写 APP_DB。

- [ ] **Step 3: 编译验证**

- [ ] **Step 4: Commit**

```
NHG warm reboot: implement Phase 1 reconcile for normal single-hop NHGs
```

---

### Task 6: NHG Reconcile — Phase 2 (SRv6 单跳 + 多跳)

**Files:**
- Modify: `fpmsyncd/nhgmgr.cpp`

- [ ] **Step 1: 实现 FV hash 计算和匹配函数**

```cpp
static std::string computeFvHash(const std::vector<swss::FieldValueTuple> &fvs) {
    std::vector<swss::FieldValueTuple> sorted = fvs;
    std::sort(sorted.begin(), sorted.end());

    std::string result;
    for (auto &fv : sorted) {
        result += fvField(fv) + "=" + fvValue(fv) + ";";
    }
    return result;
}

NHGMgr::AppDbNHGEntry* NHGMgr::findMatchingAppDbEntry(const std::string &fvHash) {
    for (auto &[key, entry] : m_appdb_nhg_fvs) {
        if (!entry.matched && computeFvHash(entry.fvVector) == fvHash) {
            return &entry;
        }
    }
    return nullptr;
}
```

- [ ] **Step 2: 实现拓扑排序**

```cpp
static std::vector<ribID> topologicalSort(
        const std::map<ribID, NHGMgr::TempReconcileEntry> &entries) {
    // 构建入度 map
    std::map<ribID, int> inDegree;
    std::map<ribID, std::set<ribID>> adj;

    for (auto &[id, entry] : entries) {
        inDegree[id] = 0;
    }

    for (auto &[id, entry] : entries) {
        for (auto &grp : entry.nhg.nh_grp_full_list) {
            ribID depId(grp.id);
            if (entries.count(depId)) {
                adj[depId].insert(id);
                inDegree[id]++;
            }
        }
    }

    // Kahn's algorithm
    std::queue<ribID> q;
    for (auto &[id, deg] : inDegree) {
        if (deg == 0) q.push(id);
    }

    std::vector<ribID> result;
    while (!q.empty()) {
        ribID cur = q.front(); q.pop();
        result.push_back(cur);
        for (auto &next : adj[cur]) {
            if (--inDegree[next] == 0) {
                q.push(next);
            }
        }
    }

    if (result.size() < entries.size()) {
        SWSS_LOG_WARN("NHG warm restart: detected cyclic dependencies, %zu of %zu skipped",
                       entries.size() - result.size(), entries.size());
    }

    return result;
}
```

- [ ] **Step 3: 实现 reconcileNHGsWithSonicObj**

```cpp
void NHGMgr::reconcileNHGsWithSonicObj(std::vector<std::vector<uint8_t>> &nhgBuffer) {
    SWSS_LOG_NOTICE("NHG warm restart Phase 2: reconciling NHGs with Sonic objects");
    m_nhgWrState = NHG_WR_RECONCILING;

    std::map<ribID, TempReconcileEntry> tempMap;

    // Step A: 对未 reconcile 的 NHG，创建临时 entry 干跑计算 FV
    for (auto &rawMsg : nhgBuffer) {
        fib::NextHopGroupFull nhg;
        uint8_t af;
        uint16_t msgType;

        if (parseNHGFromRawMsg(rawMsg, nhg, af, msgType) != 0) continue;
        if (msgType == RTM_DELNHGFIB) continue;
        if (m_reconciled_ids.count(ribID(nhg.id))) continue;

        RIBNHGEntry tmpEntry(m_rib_nhg_table);
        int ret = tmpEntry.setEntry(nhg, af);
        if (ret != 0) {
            SWSS_LOG_WARN("NHG warm restart: failed to setEntry for rib_id %u", nhg.id);
            continue;
        }

        TempReconcileEntry entry;
        entry.nhg = nhg;
        entry.af = af;
        entry.fvVector = tmpEntry.getFvVector();
        entry.needsSonicObj = tmpEntry.needCreateSonicObject() || tmpEntry.hasSonicPICObj();
        entry.reuseSonicId = sonicObjectID(0);
        entry.reusePicObjId = sonicObjectID(0);

        tempMap[ribID(nhg.id)] = entry;
    }

    // Step B: FV vector 匹配
    for (auto &[ribId, entry] : tempMap) {
        if (!entry.needsSonicObj) continue;

        std::string fvHash = computeFvHash(entry.fvVector);
        auto match = findMatchingAppDbEntry(fvHash);

        if (match) {
            entry.reuseSonicId = match->sonicId;
            match->matched = true;
            SWSS_LOG_INFO("NHG warm restart: matched rib_id %u to sonic_id %u",
                           ribId.id, match->sonicId.id);
        }
    }

    // Step C: 拓扑排序 + 写入
    auto sorted = topologicalSort(tempMap);

    for (auto ribId : sorted) {
        auto &entry = tempMap[ribId];
        if (entry.reuseSonicId.id != 0) {
            addNHGFullWithSonicId(entry.nhg, entry.af,
                                   entry.reuseSonicId, entry.reusePicObjId);
        } else {
            addNHGFull(entry.nhg, entry.af);
        }
        m_reconciled_ids.insert(ribId);
    }

    // Step D: 清理未匹配的旧 APP_DB 表项
    for (auto &[key, entry] : m_appdb_nhg_fvs) {
        if (!entry.matched) {
            m_rib_nhg_table->removeFromDB(entry.sonicId);
            SWSS_LOG_NOTICE("NHG warm restart: removed stale APP_DB entry %s", key.c_str());
        }
    }

    m_appdb_nhg_fvs.clear();
    m_saved_nhg_infos.clear();
    m_reconciled_ids.clear();
    m_nhgWrState = NHG_WR_RECONCILED;

    SWSS_LOG_NOTICE("NHG warm restart Phase 2: reconcile complete");
}
```

- [ ] **Step 4: 编译验证**

- [ ] **Step 5: Commit**

```
NHG warm reboot: implement Phase 2 reconcile with FV matching and topo sort
```

---

### Task 7: addNHGFullWithSonicId — 复用 sonicID 路径

**Files:**
- Modify: `fpmsyncd/nhgmgr.cpp`

- [ ] **Step 1: 实现 addNHGFullWithSonicId**

基于现有 `addNewNHGFull` 的逻辑，跳过 ID 分配和 DB 写入：

```cpp
int NHGMgr::addNHGFullWithSonicId(const fib::NextHopGroupFull &nhg, uint8_t af,
                                    sonicObjectID reuseNhgId, sonicObjectID reusePicId) {
    ribID id = ribID(nhg.id);

    // Step 1: 添加 entry 到 RIBNHGTable（和 addNewNHGFull 相同）
    int ret = m_rib_nhg_table->addEntry(nhg, af);
    if (ret != 0) return ret;

    RIBNHGEntry *entry = m_rib_nhg_table->getEntry(id);
    if (entry == nullptr) return -1;

    // Step 2: 设置复用的 sonic ID（跳过 allocateID）
    if (reuseNhgId.id != 0) {
        entry->setSonicNHGObjId(reuseNhgId);
        // 不调用 writeToDB — APP_DB 表项已存在且内容相同

        // 维护 shared NHG map
        if (entry->isSharedSonicNHG()) {
            SonicNHGObjectKey key = entry->getSonicNHGObjectKey();
            int existingId = m_rib_nhg_table->getCreatedSharedNHGObjectID(key);
            if (existingId >= 0) {
                m_rib_nhg_table->addSonicNHGObjectRef(key);
            } else {
                m_rib_nhg_table->insertCreatedSharedNHGObject(key, reuseNhgId);
            }
        }
    }

    // Step 3: 处理 PIC context（如果有）
    if (entry->hasSonicPICObj() && reusePicId.id != 0) {
        entry->setSonicPICObjId(reusePicId);
        // 不写 PIC_CONTEXT_TABLE — 已存在
    } else if (entry->hasSonicPICObj()) {
        ret = createSonicPICObject(entry);
        if (ret != 0) {
            m_rib_nhg_table->delEntry(nhg.id);
            return ret;
        }
    }

    return 0;
}
```

- [ ] **Step 2: 编译验证**

- [ ] **Step 3: Commit**

```
NHG warm reboot: implement addNHGFullWithSonicId for sonic ID reuse
```

---

### Task 8: 路由回放 + onWarmStartEnd 集成

**Files:**
- Modify: `fpmsyncd/routesync.cpp`
- Modify: `fpmsyncd/fpmsyncd.cpp`

- [ ] **Step 1: 实现 replayBufferedRoutes**

```cpp
void RouteSync::replayBufferedRoutes() {
    SWSS_LOG_NOTICE("NHG warm restart: replaying %zu buffered route messages",
                     m_route_raw_buffer.size());

    for (auto &rawMsg : m_route_raw_buffer) {
        struct nlmsghdr *nlh = (struct nlmsghdr *)rawMsg.data();

        // 判断是 raw 路径还是 libnl 路径
        if (nlh->nlmsg_type == RTM_NEWSRV6VPNROUTE ||
            nlh->nlmsg_type == RTM_DELSRV6VPNROUTE) {
            onMsgRaw(nlh);
        } else if (FpmLink::isRawProcessing(nlh)) {
            onMsgRaw(nlh);
        } else {
            // 普通路由：通过 NetDispatcher
            nl_msg *msg = nlmsg_convert(nlh);
            if (msg) {
                nlmsg_set_proto(msg, NETLINK_ROUTE);
                NetDispatcher::getInstance().onNetlinkMessage(msg);
                nlmsg_free(msg);
            } else {
                SWSS_LOG_ERROR("NHG warm restart: failed to convert route nlmsg");
            }
        }
    }

    m_route_raw_buffer.clear();
    SWSS_LOG_NOTICE("NHG warm restart: route replay complete");
}
```

> 注意：需要确认 `FpmLink::isRawProcessing` 是否为 static 方法。如果不是，需要将其改为 static 或者在 RouteSync 中直接复制判断逻辑。

- [ ] **Step 2: 修改 onWarmStartEnd() 集成 NHG reconcile**

在 `RouteSync::onWarmStartEnd()` 中，在现有 reconcile 逻辑之前添加 NHG reconcile：

```cpp
void RouteSync::onWarmStartEnd(DBConnector &applStateDb) {
    // === 新增：NHG reconcile ===
    if (m_rib_fib_nhg_mgr.isNhgWarmRestartInProgress()) {
        m_rib_fib_nhg_mgr.reconcileNormalSingleHopNHGs(m_nhg_raw_buffer);
        m_rib_fib_nhg_mgr.reconcileNHGsWithSonicObj(m_nhg_raw_buffer);
        m_nhg_raw_buffer.clear();

        // 回放缓存路由
        replayBufferedRoutes();
    }
    // === 新增结束 ===

    // 现有逻辑：suppress-fib-pending 处理
    if (m_isSuppressionEnabled) {
        // ... existing offload logic ...
    }

    // 现有逻辑：路由 reconcile
    if (m_warmStartHelper.inProgress()) {
        m_warmStartHelper.reconcile();
    }
}
```

- [ ] **Step 3: 修改 fpmsyncd.cpp 中的 warm start 初始化**

在 fpmsyncd.cpp 的 warm start 初始化代码中（`checkAndStart()` 之后），添加 NHG warm restart 初始化：

```cpp
// 在现有 warm start check 之后
if (warmStartEnabled) {
    // ... 现有 timer 设置 ...

    // 新增：NHG warm restart 初始化
    if (sync.getNhgFibEnabled()) {
        sync.getNHGMgr().initWarmRestart();
        // 需要传入 stateTable 和 appDbNhgTable
        sync.getNHGMgr().loadWarmRestartState(
            sync.getNhgFullStateTable(),
            appDbNhgTable);  // 需要创建这个 Table 对象
    }
}
```

> 注意：需要在 RouteSync 中添加 `getNHGMgr()` 和 `getNhgFullStateTable()` 访问器。还需要创建 APP_DB NEXTHOP_GROUP_TABLE 的 Table 对象用于加载。

- [ ] **Step 4: 在 RouteSync 中添加访问器**

在 routesync.h public section：

```cpp
NHGMgr& getNHGMgr() { return m_rib_fib_nhg_mgr; }
swss::Table& getNhgFullStateTable() { return m_nhgFullStateTable; }
bool getNhgFibEnabled() const { return m_nhgFibEnabled; }
```

- [ ] **Step 5: 编译验证**

- [ ] **Step 6: Commit**

```
NHG warm reboot: integrate reconcile into onWarmStartEnd and add route replay
```

---

### Task 9: Shutdown 信号集成

**Files:**
- Modify: `fpmsyncd/fpmsyncd.cpp`

- [ ] **Step 1: 在 fpmsyncd 退出前调用 saveWarmRestartState**

找到 fpmsyncd 的 warm shutdown 处理点。通常在收到 SIGTERM 信号或检测到 warm restart flag 后的 cleanup 逻辑中。

需要确认 fpmsyncd 的 shutdown 流程。如果 fpmsyncd 通过 supervisor 管理，shutdown 时会收到 SIGTERM。需要注册 signal handler 或在退出路径上调用 save。

```cpp
// 在 fpmsyncd 退出路径上（需确认具体位置）
if (warmStartEnabled && sync.getNhgFibEnabled()) {
    sync.getNHGMgr().saveWarmRestartState(sync.getNhgFullStateTable());
}
```

> 这个步骤的具体位置取决于 fpmsyncd 的 shutdown 机制。需要在实现时检查 fpmsyncd 是如何处理 graceful shutdown 的。

- [ ] **Step 2: 编译验证**

- [ ] **Step 3: Commit**

```
NHG warm reboot: integrate saveWarmRestartState into shutdown path
```

---

### Task 10: 端到端验证

**Files:**
- 无新文件，使用现有测试框架

- [ ] **Step 1: 手动验证编译通过**

```bash
cd /home/admin/workspace/rib-fib/sonic-swss && make -C fpmsyncd -j4
```

- [ ] **Step 2: 代码审查 checklist**

检查以下关键点：
- [ ] NHG 消息在 warm restart 期间被正确拦截到 buffer
- [ ] 路由消息在 warm restart 期间被正确拦截到 buffer
- [ ] Phase 1 对普通单跳 NHG 正确调用 addNHGFull
- [ ] Phase 2 的临时 RIBNHGEntry 使用正式 RIBNHGTable 指针
- [ ] FV vector 匹配使用精确字符串比较
- [ ] 拓扑排序检测环形依赖
- [ ] addNHGFullWithSonicId 不写 APP_DB（对匹配的 NHG）
- [ ] 路由回放正确区分 raw 和 libnl 路径
- [ ] onWarmStartEnd 中 NHG reconcile 在路由 reconcile 之前
- [ ] saveWarmRestartState 在 shutdown 时被调用
- [ ] ID allocator 状态在 save/load 时正确处理

- [ ] **Step 3: Commit 最终清理（如有）**
