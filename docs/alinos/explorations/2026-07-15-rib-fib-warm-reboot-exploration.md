# Design Exploration: RIB/FIB NHG Manager Warm Reboot

## Decisions Examined

- **Total**: 14 decisions (8 explicit, 6 implicit)
- **High impact**: 6
- **Medium impact**: 5
- **Low impact**: 3

---

## Phase 1: Decision Inventory

| # | Decision | Type | Category | Impact |
|---|----------|------|----------|--------|
| D1 | NHGMgr 自行管理 reconcile，不复用 WarmStartHelper/AppRestartAssist | Explicit | Architecture | High |
| D2 | 单跳 NHG 不做 reconcile，直接重新插入 | Explicit | Behavior | High |
| D3 | 多跳 NHG 通过 tempRIBEntryTable + 拓扑排序 reconcile | Explicit | Architecture | High |
| D4 | 路由 reconcile 在 NHG reconcile 之后 | Explicit | Architecture | High |
| D5 | 新建 STATE_DB WARM_RESTART_NHG_TABLE 存储映射 | Explicit | Data Model | High |
| D6 | 通过 zebra NHG hash 比较匹配新旧 NHG | Explicit | Behavior | High |
| D7 | 存储 NextHopGroupFull 完整 JSON 作为持久化内容 | Explicit | Data Model | Medium |
| D8 | NHGMgr FSM: INITIALIZED → RESTORED → NHG_RECONCILING → RECONCILED | Explicit | Architecture | Medium |
| D9 | 假设 NHG 消息在路由之前到达（依赖 zebra 顺序） | Implicit | Assumptions | High (hidden) |
| D10 | 假设 BGP EOIU 意味着 NHG 消息也已全部到达 | Implicit | Assumptions | High (hidden) |
| D11 | 假设 warm reboot 前后 zebra 的 ribID 会变化 | Implicit | Assumptions | Medium |
| D12 | 未定义 reconcile 期间 APP_DB NHG 写入时序（是否需要 pipeline 暂停） | Implicit | Behavior | Medium |
| D13 | 未定义 shared NHG ref count 在 reconcile 期间的重建策略 | Implicit | Data Model | Medium |
| D14 | SRv6 PIC context 和 NHG 的 reconcile 顺序未定义 | Implicit | Behavior | Low |

---

## Phase 2: Socratic Examination

### D1: NHGMgr 自行管理 reconcile [HIGH]

**Q: Clarification** — "自行管理"的边界是什么？NHGMgr 是否需要参与 WarmStart FSM 的状态通知？
**A:** NHGMgr 维护自己的 warm restart state machine（INITIALIZED → RESTORED → NHG_RECONCILING → RECONCILED），但需要通过 `WarmStart::setWarmStartState()` 向全局 STATE_DB 报告状态。fpmsyncd main() 协调 NHG FSM 和路由 FSM 的先后关系。

**Q: Evidence** — 为什么不用 AppRestartAssist？它支持多 table。
**A:** AppRestartAssist 的 STALE/SAME/NEW 模型基于简单的 key-value 比较（`appRestartAssist.cpp` 中的 `setCacheEntryState`），但 NHG reconcile 有三个独特需求：(1) ribID 会变化，不能用 key 直接比较，需要 semantic 匹配；(2) NHG 之间有依赖关系，需要拓扑排序；(3) 共享 NHG 有 ref count，需要特殊处理。AppRestartAssist 无法满足这些。

**Q: Alternatives** — 有没有混合方案？
**A:**
- 方案 A：完全自管（当前选择）— 灵活但代码量大，需自行实现 FSM 和 DB 交互
- 方案 B：扩展 AppRestartAssist — 改动公共库影响其他使用者（fdbsyncd, neighsyncd, natsyncd）
- 方案 C：继承 AppRestartAssist 但 override 比较和排序逻辑 — 中间路线，但 AppRestartAssist 不是虚函数设计，不适合继承

**Resolution:** 确认自管方案。NHG 的匹配逻辑（semantic 比较而非 key 比较）和依赖排序是独有需求，硬塞到现有框架会制造更多问题。

---

### D2: 单跳 NHG 不做 reconcile [HIGH]

**Q: Clarification** — "不做 reconcile"是指不和 APP_DB 对比？单跳 NHG 在 APP_DB 中有没有对应的表项？
**A:** 代码确认：单跳 normal NHG `m_create_sonic_nhg_obj = false`（nhgmgr.cpp `checkNeedCreateSonicNHGObj`），不会向 `NEXTHOP_GROUP_TABLE` 写入任何内容。所以 APP_DB 中不存在单跳 NHG 的表项，自然无需 reconcile。

**Q: Assumptions** — 单跳 NHG 重启后直接 addNHGFull()，是否会触发不必要的 APP_DB 写入？
**A:** 不会。`addNHGFull` 对单跳 NHG 只更新内存中的 `m_nhg_map`，不写 APP_DB。路由引用单跳 NHG 时是 inline nexthop（直接在 ROUTE_TABLE 中写入 nexthop/ifname 字段），不通过 nexthop_group 引用。

**Q: Implications** — 但是单跳 NHG 在 RIBNHGTable 的 m_nhg_map 中需要存在，多跳 NHG 的 nh_grp_full_list 中的成员需要引用它们。如果多跳 reconcile 在单跳 reconcile 之前执行，会找不到成员。
**A:** 这正是设计要求单跳先于多跳 reconcile 的原因。`setEntry()` 中检查 `m_table->isNHGExist(it->id)`（nhgmgr.cpp line 853），如果成员不存在会导致 resolved group 不正确。

**Q: Evidence** — 单跳 SRv6 NHG 呢？SRv6 场景下即使单跳也会创建 Sonic 对象。
**A:** 对。`checkNeedCreateSonicNHGObj()` 中，`SONIC_NHG_OBJ_TYPE_NHG_WITH_SRV6_PIC_CONTEXT` 类型的 NHG 即使单跳也会创建共享 NHG 对象。**当前设计遗漏了这个场景。需要修正：SRv6 类型的单跳 NHG 必须参与 reconcile。**

**Resolution:** 修正为：**普通单跳 NHG 不做 reconcile，SRv6 单跳 NHG 必须参与 reconcile**。单跳/多跳的区分不能仅用 `isSingleHop()`，还需检查 `sonicNhgObjType`。

---

### D3: 多跳 NHG 通过 tempRIBEntryTable + 拓扑排序 [HIGH]

**Q: Clarification** — tempRIBEntryTable 和正式 RIBNHGTable 的关系是什么？reconcile 期间两者共存？
**A:** tempRIBEntryTable 是一个临时的 `map<ribID, TempNHGEntry>`，仅在 reconcile 函数内使用。reconcile 结束后，合法的 entry 按拓扑顺序调用 `addNHGFull()` 写入正式 RIBNHGTable，tempTable 随即销毁。两者不会长期共存。

**Q: Assumptions** — 拓扑排序假设依赖关系是 DAG（无环）。FRR/zebra 能否产生环形 NHG 依赖？
**A:** 在 FRR zebra 中，NHG 依赖关系理论上是 DAG（递归 nexthop 解析不应产生环）。但防御性编程仍需检测环并跳过。检测方式：拓扑排序时如果无法完成（剩余节点都有入度），即为有环。

**Q: Implications** — tempTable 中的 entry 如何获取 sonic ID？新建的需要分配，复用的用旧 ID。但旧 ID 的分配器状态如何恢复？
**A:** ID allocator 状态需要在 shutdown 时持久化 `next_nhg_id` 和 `next_pic_id`。启动后恢复这两个值，新分配的 ID 从恢复后的值开始递增，避免和复用的旧 ID 冲突。

**Resolution:** 确认方案。补充：需检测环形依赖并告警跳过。ID allocator 的 next_id 必须在 shutdown 时持久化。

---

### D4: 路由 reconcile 在 NHG reconcile 之后 [HIGH]

**Q: Evidence** — 路由 reconcile 真的依赖 NHG Manager 吗？代码怎么说？
**A:** 确认。`onRouteMsg()` 中，当路由有 nhg_id 时：
- 单跳 NHG → inline nexthop: `entry->getNextHopStr()`（需要 RIBNHGEntry 存在于 m_nhg_map）
- 多跳 NHG → nexthop_group 引用: `to_string(entry->getSonicNHGObjID())`（需要 sonic ID 已分配）

两种路径都需要 NHG Manager 已完成 reconcile。

**Q: Implications** — 路由的 warm restart refresh map 中存储的是什么？是已经解析好的 FV vector，还是未解析的 zebra 数据？
**A:** 当前代码中，`setRouteWithWarmRestart()` 接收的是**已经解析好的 FV vector**（包含 nexthop/ifname 或 nexthop_group 字段）。这意味着在 warm restart 期间调用 `onRouteMsg()` 时，NHG Manager 必须已经可用。

**这暴露了一个关键时序问题**：如果 warm restart 期间路由消息和 NHG 消息混合到达，而 NHG 被缓存到 refresh map 中没有写入 RIBNHGTable，那么 `onRouteMsg()` 调用 `getRIBNHGEntryByRIBID(nhg_id)` 会返回 nullptr，路由被丢弃！

**需要修改路由处理逻辑**：warm restart 期间的路由不能调用 `onRouteMsg()` 解析 FV vector，必须存储原始的 zebra 路由消息（包含 nhg_id），等 NHG reconcile 完成后再解析。

**Resolution:** 确认顺序。但需要 **重大修改**：warm restart 期间路由消息不能走现有的 `onRouteMsg()` → `setRouteWithWarmRestart()` 路径。需要改为存储原始路由消息（含 nhg_id），NHG reconcile 完成后再重新解析所有缓存路由。

---

### D5: 新建 STATE_DB WARM_RESTART_NHG_TABLE [HIGH]

**Q: Alternatives** — 能否复用 NHG_FULL_STATE_TABLE？
**A:** NHG_FULL_STATE_TABLE 已存储 sonic_nhg_id、pic_context_id 和 nhg_json。但它：
- 在 APPL_STATE_DB 而非 STATE_DB
- 没有 af (address family) 字段
- 没有 ID allocator 状态
- 是调试用途，warm reboot 时可能已被清空

**Q: Evidence** — warm reboot 后各 DB 的状态是什么？哪些 DB 内容会保留？
**A:** warm reboot 流程中，Redis 数据库整体保存到 `/host/warmboot/dump.rdb` 并在启动后恢复。所以 STATE_DB 和 APPL_STATE_DB 的内容都会保留。理论上 NHG_FULL_STATE_TABLE 的数据在重启后仍然存在。

**Q: Implications** — 如果 NHG_FULL_STATE_TABLE 数据保留，能否直接用它而不新建 table？
**A:** 可以考虑，但需要：(1) 补充 af 字段；(2) 补充 ID allocator 状态；(3) 将其从"调试可选"改为"warm reboot 必需"。改动现有 table 语义有一定风险。

**Resolution:** **修改为复用 NHG_FULL_STATE_TABLE**。在其中补充 af 字段和 ID allocator 状态键。理由：避免新建 table，减少 DB 膨胀，数据已经在写了只需补充缺失字段。将 NHG_FULL_STATE_TABLE 的写入从 APPL_STATE_DB 改到 STATE_DB，或直接在 APPL_STATE_DB 中使用（因为它在 warm reboot 后也会被保留）。

---

### D6: 通过 zebra NHG hash 比较匹配新旧 NHG [HIGH]

**Q: Clarification** — "zebra NHG hash" 具体指什么？是 NextHopGroupFull 结构体中 `_hash_begin` 到 `_hash_end` 之间的 jhash，还是 `key` 字段？
**A:** NextHopGroupFull 有 `key` 字段（"Hash key from Zebra"），是 zebra 计算的 hash。但 sonic-fib 库内部没有定义 hash 函数，只有 `operator==` 和 `_hash_begin/_hash_end` 标记。

**Q: Assumptions** — warm reboot 前后同一个 NHG 的 `key` (hash) 是否保持一致？
**A:** zebra 的 hash 基于 `_hash_begin` 到 `_hash_end` 之间的字段（type, vrf_id, ifindex, gate, src, rmap_src）。如果这些字段不变，hash 值应该一致。但 `id` 字段（zebra 分配的 NHG ID）在 hash 区域之外，重启后**一定会变化**（zebra 重新分配 ID）。

**Q: Evidence** — 那匹配逻辑应该用什么？
**A:** 三个选项：
1. 用 `key` (zebra hash) 匹配 — 如果 zebra hash 稳定则最高效。但 hash 冲突可能导致误匹配。
2. 用 `checkNeedUpdate()` 中的 11 个字段做 semantic 比较 — 最准确但需要遍历 saved map 查找匹配项（O(n²)）
3. 用 SonicNHGObjectKey 做比较 — 这是已有的去重机制，包含 nexthop/ifname/vpnSid/segSrc/groupMember

**Q: Meta** — 等一下，为什么需要匹配？匹配的目的是复用 sonic ID。但复用 sonic ID 的目的是什么？
**A:** 复用 sonic ID 意味着 APP_DB 中的 `NEXTHOP_GROUP_TABLE` key（`NHG_<sonicID>`）保持不变 → orchagent/syncd 不需要删除+重建 SAI 对象 → 硬件表项不变 → 流量不中断。这是 warm reboot 的核心价值。

**Resolution:** **使用 SonicNHGObjectKey 做匹配**。理由：(1) 它已经是 NHG 语义等价性的判定标准（共享 NHG 去重就用它）；(2) 匹配后可直接复用 sonic ID；(3) 不依赖 zebra hash 稳定性。具体实现：shutdown 时将每个 entry 的 SonicNHGObjectKey 序列化存储，启动后用新 entry 的 SonicNHGObjectKey 在 saved map 中查找匹配。

---

### D9: 假设 NHG 消息在路由之前到达 [HIDDEN HIGH]

**Q: Evidence** — 代码是否有这个假设？
**A:** 是。`onRouteMsg()` line 2685: 如果 `getRIBNHGEntryByRIBID(nhg_id)` 返回 nullptr，路由直接被丢弃（`return`）。代码假设 NHG 一定先于引用它的路由到达。

**Q: Implications** — warm restart 期间，NHG 消息被缓存到 refresh map 不写入 RIBNHGTable，那路由消息怎么办？
**A:** 这是 D4 中发现的关键问题。路由消息也必须被缓存为原始消息（包含 nhg_id），不能在 warm restart 期间调用 `onRouteMsg()` 解析。

**Resolution:** **warm restart 期间，路由消息必须以原始格式（包含 nhg_id）缓存到独立的 route refresh map**，不走 `onRouteMsg()` 的 FV 解析路径。NHG reconcile 完成后，逐条回放缓存的路由消息通过 `onRouteMsg()` 解析并写入 WarmStartHelper 的 refresh map，然后执行路由 reconcile。

---

### D10: 假设 BGP EOIU 意味着 NHG 也全部到达 [HIDDEN HIGH]

**Q: Evidence** — EOIU 只检查 BGP 路由收敛，不检查 NHG。
**A:** 确认。`eoiuFlagsSet()` 只检查 `STATE_BGP_TABLE_NAME` 的 IPv4/IPv6 eoiu flag。NHG 消息通过同一 FPM socket 发送，在路由之前发送（zebra 先发 NHG 再发引用它的路由）。如果 BGP EOIU 表示所有路由已发送完毕，那么引用的 NHG 也应该已经发送完毕（因为 NHG 先于路由）。

**Q: Assumptions** — 但 NHG 不是一定比路由先发送。如果 NHG 更新比路由慢怎么办？
**A:** 在 FRR/zebra 中，nexthop resolution 是路由处理的前置步骤。zebra 在发送 route 到 FPM 之前，一定先发送 route 依赖的 NHG。这是 FPM protocol 的语义保证。所以如果 BGP EOIU 表示所有路由已发完，那么这些路由依赖的 NHG 也已经发完。

**Q: Implications** — 但如果 warm reboot 期间有新增的 NHG（不被任何路由引用），EOIU 后它可能还没到达。
**A:** 不被任何路由引用的 NHG 不影响流量。即使遗漏也不会造成问题——reconcile 时如果 saved 中有但 refresh 中没有，会被标记删除，但由于没有路由引用它，删除不影响转发。

**Resolution:** **BGP EOIU 可以作为 NHG warm end 触发条件**，因为 zebra 保证 NHG 先于引用它的路由发送。但应保留 warmStartTimer 作为兜底，防止 EOIU 永不到达的场景。

---

### D11: 假设 ribID 在 warm reboot 后会变化 [MEDIUM]

**Q: Evidence** — ribID 是 zebra 分配的 nhe->id，重启后 zebra 重新分配。
**A:** 确认。`id` 在 NextHopGroupFull 中是 position 1（hash 区域之外），由 zebra 动态分配。warm reboot 后 zebra 重新创建 NHE（nexthop hash entry），ID 会重新从 1 开始分配。同一个 nexthop 可能得到不同的 ID。

**Resolution:** 确认。匹配新旧 NHG 不能依赖 ribID，必须用 semantic 比较（SonicNHGObjectKey）。

---

### D12: reconcile 期间 APP_DB 写入时序 [MEDIUM]

**Q: Clarification** — reconcile 期间是否需要暂停 pipeline flush？
**A:** 当前 fpmsyncd 代码中，warm restart 期间已经有 pipeline 暂停逻辑（fpmsyncd.cpp line 333: `!warmStartEnabled || sync.getWarmStartHelper().isReconciled()`）。NHG 的 reconcile 也应该在 pipeline flush 暂停期间完成，确保不会有中间状态被 flush 到 orchagent。

**Resolution:** NHG reconcile 必须在 pipeline flush 恢复之前完成。因为 NHG reconcile 调用 `addNHGFull()` 会通过 ProducerStateTable 写入 APP_DB，这些写入在 pipeline flush 恢复后才会被 orchagent 消费。时序：NHG reconcile → 路由 reconcile → pipeline flush 恢复。

---

### D13: shared NHG ref count 重建策略 [MEDIUM]

**Q: Clarification** — reconcile 后 m_created_shared_nhg_map 如何重建？
**A:** 复用的 NHG entry 通过 `addNHGFull()` → `checkNeedCreateSonicNHGObj()` 正常走共享逻辑。但如果多个 entry 共享同一个 sonic NHG 对象，它们在 reconcile 中的添加顺序会影响 ref count。

**Q: Implications** — 如果先添加第一个共享 entry 时分配了新 sonic ID（因为还没有匹配到旧 ID），然后添加第二个共享 entry 时匹配到了旧 ID，会产生两个不同的 sonic 对象。
**A:** 这是一个实际风险。解决方案：在 reconcile 开始前，先根据 saved state 预建 `m_created_shared_nhg_map`（将复用的 sonic ID 和 key 注册进去），确保后续 `addNHGFull()` 的共享检查能正确找到已有对象。

**Resolution:** reconcile 前需要 **预注册** 要复用的共享 NHG 对象到 `m_created_shared_nhg_map`，避免重复创建。具体：遍历匹配成功的 entries，对有 sonic object 的先注册 key → sonicID 映射（ref count 初始为 0），后续 `addNHGFull()` 调用时自然递增 ref count。

---

### D14: SRv6 PIC context 和 NHG 的 reconcile 顺序 [LOW]

**Q: Clarification** — PIC context 和 NHG 谁先 reconcile？
**A:** PIC context 是 NHG entry 的附属（`m_has_sonic_pic_obj`）。在 `addNHGFull()` 中，PIC context 的创建/更新和 NHG object 的创建/更新是同步完成的。所以 reconcile 时不需要单独处理 PIC context — 它随 NHG entry 一起处理。

**Resolution:** PIC context 不需要独立的 reconcile 步骤，随 NHG entry 的 `addNHGFull()` 自然处理。但 saved state 中需包含 `sonic_pic_obj_id` 以便复用。

---

## Phase 3: Cross-Cutting Concerns

### 1. Consistency Check

- D2 和 D3 的边界需要细化：SRv6 单跳 NHG 应属于 D3（需 reconcile），不属于 D2
- D4 和 D9 的路由缓存方案一致，需统一实现

### 2. Dependency Map

```
D5 (存储表) → D6 (匹配逻辑) → D3 (多跳 reconcile)
D2 (单跳) → D3 (多跳，因为多跳依赖单跳)
D3 (NHG reconcile) → D4 (路由 reconcile)
D10 (EOIU 时序) → D3, D4 (触发 reconcile 的条件)
D13 (ref count) → D3 (添加顺序)
```

无循环依赖。

### 3. Risk Concentration

高风险集中在 **reconcile 主流程**（D3 + D4 + D9）：NHG 消息缓存、路由消息缓存格式改动、reconcile 顺序。这是设计中最复杂的部分，需要最多的测试覆盖。

### 4. Gap Analysis

发现以下未回答的问题（已在 Phase 2 中解决）：
- ~~SRv6 单跳 NHG 的处理~~ → 已解决（D2 修正）
- ~~路由消息在 warm restart 期间的缓存格式~~ → 已解决（D4/D9 修正）
- ~~shared NHG ref count 重建~~ → 已解决（D13）
- ~~NHG_FULL_STATE_TABLE 复用可能性~~ → 已解决（D5 修正）

---

## Phase 4: Resolution Synthesis

### Confirmed Decisions

| Decision | Justification |
|----------|---------------|
| D1: NHGMgr 自管 reconcile | NHG 的 semantic 匹配、依赖排序、ref count 管理是独有需求 |
| D3: tempTable + 拓扑排序 | NHG 依赖关系要求按序添加，tempTable 隔离了 reconcile 中间状态 |
| D8: 4-state FSM | 清晰的状态边界，便于调试和状态报告 |
| D10: EOIU 作为 NHG warm end 触发 | zebra 保证 NHG 先于路由发送，保留 timer 兜底 |
| D14: PIC context 随 NHG 处理 | PIC 是 NHG 的附属，不需要独立 reconcile |

### Revised Decisions

| Decision | 原方案 | 修正为 | 原因 |
|----------|--------|--------|------|
| D2 | 所有单跳 NHG 不做 reconcile | **普通单跳不 reconcile，SRv6 单跳参与 reconcile** | SRv6 单跳会创建 Sonic NHG 对象 |
| D4/D9 | 路由走 onRouteMsg() 正常解析后缓存 FV | **路由以原始消息格式缓存，NHG reconcile 后再解析** | warm restart 期间 NHG Manager 不可用，无法解析 |
| D5 | 新建 WARM_RESTART_NHG_TABLE | **复用并扩展 NHG_FULL_STATE_TABLE** | 减少 DB 膨胀，数据已有大部分 |
| D6 | 用 zebra hash 匹配 | **用 SonicNHGObjectKey 做 semantic 匹配** | ribID 会变，hash 可能冲突，SonicNHGObjectKey 是已有的等价性判定 |

### New Decisions

| Decision | 描述 |
|----------|------|
| D13-R | reconcile 前预注册共享 NHG 的 key → sonicID 映射到 m_created_shared_nhg_map |
| D12-R | NHG reconcile 必须在 pipeline flush 恢复之前完成 |
| D11-R | 匹配新旧 NHG 必须用 SonicNHGObjectKey semantic 比较，不依赖 ribID |

### Open Questions

无。所有结构性问题已在探索中解决。

---

## Risk Register

| Risk | Likelihood | Impact | Mitigation |
|------|-----------|--------|------------|
| NHG 依赖关系成环 | Low | High — reconcile 死锁 | 拓扑排序检测环，有环跳过并告警 |
| SonicNHGObjectKey 序列化/反序列化不准确 | Medium | High — 匹配失败导致 NHG 全量重建 | 单元测试覆盖序列化往返 |
| shared NHG ref count 不一致 | Medium | Medium — 对象泄漏或误删 | reconcile 完成后全量校验 ref count |
| warm restart 路由缓存格式改动影响现有功能 | Medium | High — 非 warm restart 场景回归 | 条件编译，仅 warm restart 期间走新路径 |
| NHG_FULL_STATE_TABLE 扩展影响现有调试工具 | Low | Low | 新增字段不影响现有字段，向后兼容 |
| EOIU 信号比 NHG 消息更早到达 | Low | Medium — reconcile 时缺少部分 NHG | warmStartTimer 兜底，拉长 eoiuHoldTimer |
