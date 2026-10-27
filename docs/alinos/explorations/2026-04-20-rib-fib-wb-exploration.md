# Design Exploration: RIB-FIB Warm Reboot Support for NHGMgr

## Decisions Examined

**Total: 13 decisions**
- High Impact: 5 (architecture, data model, core abstractions)
- Medium Impact: 5 (API design, integration, error handling)
- Low Impact: 3 (naming, implementation details)

---

## Phase 1: Extracted Design Decisions

### Explicit Decisions

| # | Decision | Type | Category | Impact |
|---|----------|------|----------|--------|
| D1 | 只在 warm reboot 停止前写入 DB | explicit | architecture | High |
| D2 | 单跳 nexthop 存储，sonic_obj_id=0 | explicit | data model | Medium |
| D3 | 使用 tempRIBEntryTable 统一处理单跳/多跳 | explicit | architecture | High |
| D4 | 依赖关系不持久化，reconcile 后重建 | explicit | architecture | High |
| D5 | NHGWarmRestartAssist 继承 AppRestartAssist | explicit | architecture | High |
| D6 | TempRIBEntry 新建独立 class | explicit | data model | Medium |
| D7 | Route reconcile 在 NHG reconcile 完成后执行 | explicit | behavior | Medium |

### Implicit Decisions (Surfaced)

| # | Decision | Type | Category | Impact |
|---|----------|------|----------|--------|
| I1 | 使用 APPL_STATE_DB 作为持久化存储 | implicit | integration | High |
| I2 | State Table Schema 采用扁平化设计（非 JSON） | implicit | data model | Medium |
| I3 | Sonic ID 恢复通过遍历 State Table 实现 | implicit | behavior | Medium |
| I4 | Route 缓存使用 zebra_nhg_id 而非展开 nexthop fields | implicit | data model | Medium |
| I5 | reconcile 时 STALE 条目直接删除（不通知 Zebra） | implicit | behavior | Medium |
| I6 | tempRIBEntryTable 使用 std::map 而非 unordered_map | implicit | implementation | Low |

---

## Phase 2: Socratic Examination

### HIGH IMPACT DECISIONS

---

#### D1: 只在 warm reboot 停止前写入 DB

**Q: Clarification** — "warm reboot 停止前"具体指什么时机？如何保证 fpmsyncd 有足够时间完成保存？

**A:** 
- 时机：收到 SIGTERM 信号时，由 sigterm_handler 处理
- 系统保证：SONiC warm reboot 流程会等待 docker 优雅停止，fpmsyncd 属于 swss 容器，有固定超时时间（默认约 1-2 秒）
- 实现：使用 pipeline.flush() 确保数据落盘

**Q: Assumptions** — 这个决策依赖哪些前提条件？

**A:**
1. SIGTERM 一定会在 warm reboot 时发送给 fpmsyncd
2. pipeline.flush() 能在超时前完成
3. State Table 写入不会失败（磁盘空间充足、Redis 可用）
4. 保存期间不会有新的 NHG 更新需要处理

**Q: Evidence** — 为什么选择"只在停止前写入"而不是"实时同步"或"定期快照"？

**A:**
- NHG 变更频率相对较低（与路由更新相比）
- 实时同步会增加 APPL_DB 和 State DB 的双写压力
- 定期快照增加复杂度（需要定时器、增量检测）
- warm reboot 是低频操作，优化正常运行的性能更合理

**Q: Alternatives** — 还有哪些持久化策略？

**A:**
| 方案 | 优点 | 缺点 |
|------|------|------|
| **停止前一次性写入** (Chosen) | 零运行时开销，实现简单 | 极端情况下可能丢失状态 |
| 实时同步（写 APPL_DB 时同步写 State Table） | 数据一致性最好 | 双写压力，影响性能 |
| 定期快照（每 N 秒/每 M 次变更） | 折中方案 | 需要增量检测逻辑，增加复杂度 |
| 写日志 + 回放（类似 WAL） | 精确恢复 | 实现复杂，需要日志管理 |

选择理由：NHG 状态可重建（Zebra 会重推），允许极端情况下冷启动。

**Q: Implications** — 这个决策会带来什么连锁反应？

**A:**
1. **故障场景**：如果保存失败或超时，重启后走冷启动路径，Zebra 重推会重建所有状态，不影响正确性只影响收敛时间
2. **回滚兼容性**：旧版本不读取 State Table，自动走冷启动，无兼容性问题
3. **调试限制**：无法通过 State Table 查看历史 NHG 状态（仅最后一次 warm reboot 前）

**Q: Meta** — 我们在解决正确性问题还是性能问题？

**A:** 主要是正确性问题（warm reboot 后状态恢复），性能是次要考量。如果只考虑正确性，实时同步最保险；但考虑到 NHG 可重建特性，停止前写入是性能和正确性的合理平衡。

**Resolution**: 保持 D1 决策，但需添加保存失败时的降级处理（记录日志，走冷启动路径）。

---

#### D3: 使用 tempRIBEntryTable 统一处理单跳/多跳

**Q: Clarification** — tempRIBEntryTable 和最终的 RIBNHGTable 是什么关系？数据如何迁移？

**A:**
- tempRIBEntryTable：仅存在于 reconcile 阶段，保存 old/new 两组信息
- RIBNHGTable：正常运行时的内存状态，reconcile 完成后从 temp 表迁移合法条目
- 迁移时机：reconcileNHG() 最后阶段，将标记为 SAME/NEW 的条目插入 RIBNHGTable

**Q: Assumptions** — 这个设计的依赖条件？

**A:**
1. 内存足够容纳两份 NHG 状态（old + new）
2. Zebra 重推的 NHG 集合与重启前基本一致（不会暴增）
3. reconcile 过程是原子的（不会被打断）

**Q: Evidence** — 为什么不用原地更新 RIBNHGTable？

**A:**
- 原地更新风险：如果 reconcile 中途失败，RIBNHGTable 处于不一致状态
- 需要 old/new 对比：单表结构无法同时保存两组信息
- 回滚需求：如果 warm reboot 被取消，需要能恢复到旧状态

**Q: Alternatives** — 其他设计方案？

**A:**
| 方案 | 优点 | 缺点 |
|------|------|------|
| **temp 表双写** (Chosen) | 清晰分离阶段，失败可回滚 | 内存占用翻倍 |
| 原地更新 + 备份 | 内存占用小 | 实现复杂，备份/恢复逻辑容易出错 |
| 直接操作 RIBNHGTable + 标记 | 无额外内存 | 逻辑混乱，STALE 条目处理困难 |

**Q: Implications** — 内存占用具体是多少？

**A:**
- 假设 100K NHG 条目，每个 TempRIBEntry 约 200 字节
- temp 表内存：~20MB
- 可接受：fpmsyncd 通常有足够内存，且 warm reboot 是临时状态

**Q: Meta** — 这个设计是否过度工程化？

**A:** 不是。单跳/多跳统一处理简化了 reconcile 逻辑，避免了重复代码。如果不统一，单跳和多跳需要两套 reconcile 逻辑。

**Resolution**: 保持 D3，但需确认内存限制和添加 temp 表大小监控。

---

#### D4: 依赖关系不持久化，reconcile 后重建

**Q: Clarification** — "依赖关系"具体指什么？为什么不持久化？

**A:**
- 依赖关系：多跳 NHG 的成员引用（depends：我依赖谁；dependents：谁依赖我）
- 不持久化原因：Zebra 重推的 NHG 带有新的 zebra_id，旧依赖关系中的 ID 已失效

**Q: Assumptions** — Zebra 重推时依赖关系会完全一致吗？

**A:**
1. Zebra 会重推所有 NHG（包括被依赖的单跳）
2. 重推顺序可能不同，但依赖图结构一致
3. 不会有循环依赖（Zebra 保证）

**Q: Evidence** — 如果依赖关系变化了怎么办？

**A:**
- 正常情况：配置未变，依赖关系不变
- 边界情况：warm reboot 期间配置变更
  - 新依赖：作为 NEW 条目处理
  - 删除依赖：旧条目标记 STALE 删除
  - 修改依赖：旧条目 STALE，新条目 NEW

**Q: Alternatives** — 持久化依赖关系有什么好处？

**A:**
- 可以检测 Zebra 行为异常（比如漏推某些 NHG）
- 可以优化 reconcile 顺序（按依赖拓扑排序）
- 但：增加了存储复杂度，且 ID 变化使旧依赖关系难以直接使用

**Q: Implications** — reconcile 时如何处理依赖？

**A:**
1. 第一轮：处理所有单跳 NHG（无依赖）
2. 第二轮：处理多跳 NHG，此时依赖的 zebra_id 已经是 new_id
3. 风险：如果 Zebra 先推多跳后推单跳，temp 表中可能找不到依赖

**Q: Meta** — 这个决策是否隐藏了时序假设？

**A:** 是。设计假设 Zebra 会推送所有 NHG，但没有规定顺序。如果 Zebra 推送顺序与依赖拓扑不一致，reconcile 可能需要多轮处理。

**Resolution**: 保持 D4，但 reconcile 需要支持多轮处理或按依赖层级排序。

---

#### D5: NHGWarmRestartAssist 继承 AppRestartAssist

**Q: Clarification** — AppRestartAssist 提供什么能力？NHGWarmRestartAssist 需要扩展什么？

**A:**
- AppRestartAssist 提供：
  - STALE/SAME/NEW/DELETE 状态机
  - 与 WarmStartHelper 的集成
  - 基本的 reconcile 流程
- NHGWarmRestartAssist 扩展：
  - tempRIBEntryTable 管理（old/new ID 映射）
  - NHG 特有的依赖关系处理
  - Route 缓存和 reconcile

**Q: Assumptions** — AppRestartAssist 的设计是否适合 NHG 场景？

**A:**
1. AppRestartAssist 是为 Route 设计的，Route 没有复杂的 ID 映射问题
2. AppRestartAssist 假设条目可以直接对比，NHG 需要 hash key 对比
3. AppRestartAssist 不处理依赖关系

**Q: Evidence** — 继承 vs 组合？

**A:**
- 继承优点：复用状态机和 reconcile 流程，保持与现有 Route reconcile 的一致性
- 继承缺点：AppRestartAssist 可能有 Route 特有的逻辑需要绕过
- 组合优点：更灵活，不受基类约束
- 组合缺点：需要重新实现状态机

**Q: Alternatives** — 完全独立实现？

**A:**
| 方案 | 优点 | 缺点 |
|------|------|------|
| **继承** (Chosen) | 复用现有机制，一致性 | 可能受基类限制 |
| 组合 | 灵活，无继承耦合 | 重复实现状态机 |
| 完全独立 | 完全控制 | 维护两份相似代码 |

**Q: Implications** — 如果 AppRestartAssist 变更怎么办？

**A:**
- 风险：基类变更可能破坏 NHGWarmRestartAssist
- 缓解：AppRestartAssist 是稳定接口，很少变更
- 需要：明确哪些方法可以重写，哪些不能

**Q: Meta** — 这是代码复用还是概念复用？

**A:** 两者都有。状态机概念复用（STALE/SAME/NEW/DELETE），代码复用（reconcile 流程）。NHG 和 Route 的 warm reboot 问题本质相似。

**Resolution**: 保持 D5，但需明确 AppRestartAssist 的扩展点，添加文档说明。

---

#### I1: 使用 APPL_STATE_DB 作为持久化存储

**Q: Clarification** — 为什么选择 APPL_STATE_DB 而不是其他 DB？

**A:**
- APPL_STATE_DB：用于应用程序状态持久化，符合场景
- 其他选择：
  - CONFIG_DB：用于配置，不用于运行时状态
  - STATE_DB：用于 orchagent 状态
  - 本地文件：脱离 Redis 生态

**Q: Assumptions** — APPL_STATE_DB 的可靠性如何？

**A:**
1. Redis AOF 持久化已启用，数据不会丢失
2. APPL_STATE_DB 在 warm reboot 期间保持可用
3. 其他组件不会误删我们的 key

**Q: Evidence** — 与现有 Route warm reboot 保持一致？

**A:**
- Route warm reboot 使用 APPL_STATE_DB 存储路由状态
- 使用相同的机制有利于维护和一致性
- schema.h 中已有相关定义模式

**Q: Alternatives** — 使用本地文件存储？

**A:**
| 方案 | 优点 | 缺点 |
|------|------|------|
| **APPL_STATE_DB** (Chosen) | 与 Route 一致，Redis 保证原子性 | 依赖 Redis 可用 |
| 本地文件 | 不依赖 Redis，简单 | 需要文件锁，与生态不一致 |
| 共享内存 | 最快 | 无法跨进程重启保留 |

**Q: Implications** — 如果 Redis 在保存时崩溃？

**A:**
- pipeline.flush() 保证原子性，要么全成功要么全失败
- 如果崩溃，State Table 可能部分写入，启动时需要检测并清理

**Q: Meta** — 这个选择是技术决定还是组织决定？

**A:** 主要是技术决定，APPL_STATE_DB 是 SONiC 中状态持久化的标准位置。但也受现有代码影响（Route 已经这样做了）。

**Resolution**: 保持 I1，但启动时需要检测 State Table 完整性，处理部分写入的情况。

---

### MEDIUM IMPACT DECISIONS

---

#### D2: 单跳 nexthop 存储，sonic_obj_id=0

**Q: Clarification** — 单跳 nexthop 为什么需要参与 reconcile？

**A:**
- 多跳 NHG 的成员引用单跳 NHG 的 zebra_id
- reconcile 时需要知道哪些单跳被引用，哪些可以删除
- 单跳也可能被路由直接引用

**Q: Assumptions** — sonic_obj_id=0 不会与真实 ID 冲突？

**A:**
- SonicIDAllocator 从 1 开始分配
- 0 作为 reserved ID 表示"无 Sonic 对象"
- 需要文档化这个约定

**Q: Evidence** — 不存储单跳会怎样？

**A:**
- 多跳 reconcile 时无法确定依赖关系
- 被路由直接引用的单跳无法恢复
- 必须存储

**Q: Alternatives** — 给单跳也分配真实 Sonic ID？

**A:**
- 可以，但单跳不需要创建 Sonic NHG 对象（直接展开到 route）
- 分配 ID 会增加 ID 消耗，无实际收益

**Resolution**: 保持 D2，文档化 0 为 reserved ID。

---

#### D6: TempRIBEntry 新建独立 class

**Q: Clarification** — TempRIBEntry 和 RIBNHGEntry 的核心区别？

**A:**
- TempRIBEntry：保存 old_zebra_id + new_zebra_id，用于 reconcile 对比
- RIBNHGEntry：只保存当前有效的 zebra_id
- TempRIBEntry 有 state 标记（STALE/NEW/SAME/DELETE）

**Q: Assumptions** — 独立 class 不会导致代码重复？

**A:**
- 两者有共同字段（nexthop, ifname, weight 等）
- 可以考虑提取公共基类，但当前复杂度不值得

**Q: Evidence** — 继承 RIBNHGEntry 的问题？

**A:**
- RIBNHGEntry 有 sonic_obj_id，TempRIBEntry 需要 old/new 两组
- 继承会导致命名混乱（sonic_obj_id vs old_sonic_obj_id）

**Resolution**: 保持 D6，如果字段重复过多后续可提取公共结构。

---

#### D7: Route reconcile 在 NHG reconcile 完成后执行

**Q: Clarification** — Route 缓存时存储什么？为什么不直接展开 nexthop？

**A:**
- 缓存：route_key + zebra_nhg_id + fields
- 不展开原因：
  1. 节省内存（不重复存储 nexthop 字段）
  2. Zebra 可能更新 NHG，展开后可能不一致
  3. reconcile 时通过 NHGMgr 查询最新映射

**Q: Assumptions** — NHG reconcile 完成后，所有 zebra_id 都能查到？

**A:**
- 是，Zebra 会重推所有被引用的 NHG
- 如果查不到，说明 Zebra 行为异常，需要错误处理

**Q: Implications** — Route reconcile 依赖 NHGMgr 状态？

**A:**
- 是，需要 NHGMgr 提供 zebra_id → nexthop fields 的查询接口
- 可能需要新增接口或暴露 RIBNHGTable

**Resolution**: 保持 D7，确保 NHGMgr 提供查询接口供 Route reconcile 使用。

---

#### I2: State Table Schema 采用扁平化设计

**Q: Clarification** — 为什么不用 JSON 存储 group 成员？

**A:**
- 扁平化：`<zebra_id>:<weight>[,...]` 格式
- 不用 JSON：
  - SONiC 传统偏好扁平化（与现有表一致）
  - 解析简单，无需 JSON 库
  - 字段查询方便（Redis HGET）

**Q: Assumptions** — group 成员数量不会太多？

**A:**
- 通常 < 16 个成员（ECMP 限制）
- 字符串格式可以容纳

**Resolution**: 保持 I2，与 SONiC 风格一致。

---

#### I4: Route 缓存使用 zebra_nhg_id 而非展开 nexthop fields

**Q: Clarification** — 这与 D7 的关系？

**A:**
- 是 D7 的具体实现方式
- 存储 zebra_nhg_id 而不是完整的 nexthop 字符串

**Q: Assumptions** — zebra_nhg_id 在 warm reboot 期间稳定？

**A:**
- Zebra 重推时会分配新的 zebra_id
- 但 Route 缓存的是"当时的 zebra_id"，reconcile 时通过 temp 表映射到 new_id

**Q: Implications** — 需要 old_zebra_id → new_zebra_id 的映射？

**A:**
- 是，tempRIBEntryTable 需要提供这个映射
- reconcileRoutes 时需要将缓存的 zebra_nhg_id 转换为 new_id 再查询

**Resolution**: 保持 I4，确保 tempRIBEntryTable 提供 ID 映射查询。

---

### LOW IMPACT DECISIONS

---

#### I3: Sonic ID 恢复通过遍历 State Table 实现

**Q: Clarification** — 遍历 vs 单独存储 ID 列表？

**A:**
- 遍历：启动时读取 NHG_SONIC_STATE_TABLE 所有 key，提取 sonic_id
- 替代：单独存储一个 ID 列表，但增加写入复杂度
- 遍历简单且可靠

**Resolution**: 保持 I3，遍历数据量可控（与 NHG 数量线性相关）。

---

#### I5: reconcile 时 STALE 条目直接删除

**Q: Clarification** — 不通知 Zebra 吗？

**A:**
- STALE 条目是 fpmsyncd 内存状态，Zebra 已经不会重推这些 NHG
- 需要删除的是 APPL_DB 中的对应条目（orchagent 会同步删除 ASIC）
- Zebra 不需要知道（它已经不关心这些 NHG）

**Resolution**: 保持 I5，STALE 条目只涉及本地状态清理。

---

#### I6: tempRIBEntryTable 使用 std::map

**Q: Clarification** — 为什么不用 unordered_map？

**A:**
- std::map：有序，遍历时按 zebra_id 排序，便于调试和日志
- unordered_map：无序，理论上更快但差距不大
- 数据量不大，map 的 O(log n) 可接受

**Resolution**: 保持 I6，有序性有利于调试。

---

## Phase 3: Cross-Cutting Concerns

### Consistency Check

| 检查项 | 结果 | 说明 |
|--------|------|------|
| D1 与 D3 | ✅ 一致 | 停止前写入，启动后读到 temp 表 |
| D3 与 D4 | ⚠️ 需注意 | temp 表不存依赖，reconcile 需多轮处理 |
| D5 与 D7 | ✅ 一致 | AppRestartAssist 支持分阶段 reconcile |
| D4 与 I4 | ✅ 一致 | 都依赖 Zebra 重推重建关系 |
| I1 与现有 Route WB | ✅ 一致 | 都使用 APPL_STATE_DB |

### Dependency Map

```
D1 (存储时机)
  → 影响 D3 (temp 表生命周期)
  → 影响 I1 (DB 选择)

D3 (temp 表)
  → 依赖 D6 (TempRIBEntry 定义)
  → 影响 D4 (依赖重建时机)
  → 影响 D7 (Route reconcile 顺序)

D5 (继承 AppRestartAssist)
  → 影响 D7 (reconcile 流程)
  → 约束 reconcile 实现方式

D4 (依赖不持久化)
  → 影响 reconcile 算法设计
  → 需要处理 Zebra 推送顺序问题
```

### Risk Concentration

| 区域 | 风险 | 缓解措施 |
|------|------|----------|
| reconcile 算法 | 高 | 多轮处理、依赖拓扑排序 |
| SIGTERM 处理 | 中 | 超时保护、降级处理 |
| ID 映射 | 中 | temp 表 old/new 双写 |
| State Table 一致性 | 低 | 启动时检测、清理 |

### Gap Analysis

| 问题 | 设计是否回答 | 补充决策 |
|------|-------------|----------|
| Zebra 推送顺序不一致 | ⚠️ 部分 | reconcile 需支持多轮 |
| 部分 State Table 写入失败 | ❌ 未回答 | 添加完整性检测 |
| warm reboot 取消场景 | ❌ 未回答 | 添加取消检测逻辑 |
| 内存不足处理 | ❌ 未回答 | 添加 temp 表大小限制 |
| 重复 warm reboot | ⚠️ 部分 | 启动时清理旧 State Table |

---

## Phase 4: Resolution Synthesis

### Confirmed Decisions

| 决策 | 确认理由 |
|------|----------|
| D1: 停止前写入 | 运行时零开销，符合 warm reboot 低频特性 |
| D3: temp 表统一处理 | 简化 reconcile 逻辑，支持失败回滚 |
| D5: 继承 AppRestartAssist | 复用成熟机制，保持一致性 |
| I1: APPL_STATE_DB | SONiC 标准做法，与 Route WB 一致 |
| D2: 单跳存储 | 必须参与 reconcile |
| D6: 独立 TempRIBEntry | 清晰分离概念 |
| D7: Route 后 reconcile | 依赖 NHG 映射完成 |

### Revised Decisions

| 决策 | 原设计 | 修订后 | 修订原因 |
|------|--------|--------|----------|
| D4 补充 | 依赖不持久化 | **reconcile 支持多轮处理** | 处理 Zebra 推送顺序不一致 |
| D1 补充 | SIGTERM 处理 | **添加降级路径** | 保存失败时走冷启动 |
| I1 补充 | 直接读取 | **启动时检测完整性** | 处理部分写入失败 |

### New Decisions

| 决策 | 内容 | 理由 |
|------|------|------|
| N1 | **启动时清理 State Table** | 避免重复 warm reboot 时旧数据干扰 |
| N2 | **temp 表大小限制** | 防止内存溢出（可配置阈值） |
| N3 | **Zebra 推送超时检测** | 如果长时间未收到关键 NHG，触发错误处理 |
| N4 | **reconcile 取消机制** | warm reboot 取消时能恢复到正常状态 |

### Open Questions

| 问题 | 需要输入 | 影响 |
|------|----------|------|
| Q1 | SIGTERM 超时具体数值？ | 决定 saveStateToDB 性能要求 |
| Q2 | 是否需要配置开关禁用 NHG WB？ | 故障时回退手段 |
| Q3 | State Table 命名最终确认？ | 需要同步 schema.h |

---

## Refined Design

### 关键变更摘要

1. **reconcile 算法增强**：支持多轮处理，处理 Zebra 推送顺序不一致问题
2. **降级路径**：保存失败时记录日志并走冷启动，不中断流程
3. **完整性检测**：启动时检测 State Table，部分写入时清理后冷启动
4. **资源限制**：temp 表添加大小限制，防止内存溢出

### 更新的 reconcile 流程

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
    
    // 2. 读取到 temp 表（标记 STALE）
    // ...
    
    // 3. 恢复 Sonic ID
    // ...
}
```

---

## Risk Register

| 风险 | 可能性 | 影响 | 缓解措施 |
|------|--------|------|----------|
| SIGTERM 超时导致保存失败 | 低 | 中 | 降级到冷启动，记录日志 |
| Zebra 推送顺序导致依赖无法解析 | 中 | 高 | 多轮 reconcile，MAX_ROUNDS 限制 |
| State Table 部分写入 | 低 | 高 | 启动时完整性检测 |
| 内存不足 | 低 | 高 | temp 表大小限制 |
| Zebra 行为异常（漏推 NHG） | 低 | 高 | 超时检测，错误日志 |
| warm reboot 取消 | 中 | 中 | 取消检测，状态恢复 |

---

## Quality Gates Check

- [x] 所有显式设计决策已审查
- [x] 至少 3 个隐式决策已发现和审查（实际发现 6 个）
- [x] 所有 High-impact 决策有替代方案文档
- [x] 没有决策仅用"显然"或"标准"作为理由
- [x] 交叉一致性检查通过
- [x] 差距分析发现的问题已记录为新决策

---

## 下一步行动

1. **回答 Open Questions**：确认 SIGTERM 超时、配置开关、State Table 命名
2. **细化 reconcile 算法**：编写伪代码，处理边界情况
3. **更新设计文档**：将本探索的修订内容合并到原设计文档
4. **代码评审准备**：重点关注 reconcile 多轮处理和降级路径
