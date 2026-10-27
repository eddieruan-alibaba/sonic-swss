# SONiC SWSS warm reboot 端到端测试与设备验收计划

> **For agentic workers:** Use /alinos.subagent-dev (recommended) or /alinos.executing-plans to implement this plan task-by-task.

**Goal:** 验证本轮构建的 NHG 从正常冷预热到系统 warm reboot 的恢复、ID 保持、后续更新及转发，取得实际证据后才判定通过。
**Architecture:** 区分 DVS 进程集成、虚拟系统功能验证和物理设备验收，沿用独立期望状态与逐流判据。先最小静态、再 PIC/动态、最后连续轮次；缺能力只阻塞对应范围，不用 mock 或数据库注入冒充真实生产者端到端。
**Tech Stack:** 已构建的 SONiC 镜像与 GTest、实际 BGP/Zebra、fpmsyncd/swss/syncd、Redis、平台只读查询、外部流量发生与捕获、Python 离线判据。

<a id="e2e-plan"></a>
## 本轮端到端执行计划（2026-10-27）

本节是本轮测试入口，细化原 C03–C06 的执行顺序，不替代[精化规范](../specs/2026-10-26-sonic-swss-warmreboot-validation-design.md)或删减其必测项。本次只写计划，未运行测试、升级镜像、修改设备配置或触发重启；阅读本计划不构成设备操作授权。后文原 C01–C06 保留为工具实现与设备工作分解，不是已经完成的自动化。

### 1. 当前基线与环境选择

| 项目 | 当前事实及执行规则 |
|---|---|
| 源码 | 本地 `ribfib_3_wb_ai@d0faaf10`；含迁移 `a06bfd59`、离线门禁 `0a0340e3` 和 NHG ID 诊断 `d0faaf10`。实际镜像必须另核对完整 SWSS SHA、buildimage SHA、依赖和镜像摘要，不以分支名代替。 |
| 新构建 | 用户已报告编译产物可用；尚未核验本轮构建号、镜像种类及测试日志，不能据此填写 GTest 或 warm reboot PASS。 |
| 历史回归 | Jenkins #28 的 Assist 15 例中 14 例通过，`WarmRestart_EndToEnd` 出现旧 ID 1 恢复成 2、旧 APP_DB 键缺失；诊断提交不是修复证明。新构建必须保留该用例的结果，失败仍需定位，不跳过或放宽断言。 |
| 已有离线能力 | `nhg_warm_restart_result.py` 提供结果聚合、冷预热门禁、时间区间和序列统计；`0a0340e3` 提交前有 57 例 unittest 通过。它不发流、不控制重启；`missing_fields()` 仍只是原 C02 的拟实现接口。 |
| 已有 DVS smoke | `tests/test_warm_reboot.py:2478-2493` 仅启停 fpmsyncd，且人工设置 `restore_count`，没有 NHG/路由/流量恢复断言；不能复用其准备动作证明健康首次 warm。旧路由大用例仍被跳过。 |

按实际产物选择执行范围，不默认上一轮 VS 构建已经提供完整系统或物理 ASIC：

| 环境 | 可执行目标 | 不能给出的结论 |
|---|---|---|
| 只有 `docker-sonic-vs` 的专用 Linux DVS | L2 真实进程、Redis、恢复状态和引用；确定性 NHGFIB 场景还需 B 子计划的专用生产者、夹具及观察能力。 | 容器/进程 restart 不等于整机 warm reboot；DVS 不能证明物理无损或 ASIC 保持。 |
| 完整 vSONiC/VS 虚拟系统，具备实际 BGP/Zebra 和系统 warm 编排 | 虚拟系统上的 cold→系统 warm→恢复后增删改功能链路；按 E01–E06/E10 中适用项逐项记录。 | 不计为物理 G3/G4 通过；只有软件转发时，只报告所测软件路径的连通性。 |
| 隔离物理 DUT，具有带外管理和外部流量仪 | L3 状态、真实硬件引用及 L4 逐流测量，执行适用 S01–S04。 | 不外推到其他平台、规模、版本或未测业务。 |

**首轮路线：E00 版本/能力核验 → E01 冷预热 → E02 一次最小静态系统 warm → E09-N 普通对象恢复后生命周期。** 这四项先取得证据，再扩展双栈/VRF、SRv6/PIC 和动态事件。普通首轮不以 PIC 能力为前提；E09-P 随 PIC 轮次执行，不能从完整验收范围中删掉。若仅有 DVS，停止在明确标记的进程集成范围，不用重启容器替代系统步骤。

### 2. 执行前冻结的输入与操作清单

每个运行以 `run_id` 关联以下记录；本节由执行方在仓库外证据目录填写真实值，不在计划中虚构设备、预算或凭据。

| 输入 | 必须记录的内容 | 缺失时的处理 |
|---|---|---|
| 构建对应关系 | 新构建号/日志位置、镜像类型/摘要、实际安装包、SWSS/buildimage 完整 SHA、swss-common/nexthopgroup/FRR 版本、架构、已有测试 XML/退出码。 | 对应版本门禁 BLOCKED；不得用本地 HEAD 冒充运行版本。 |
| 独占与恢复 | DUT/VM/DVS 唯一标识、端口/命名空间、测试窗口、允许的单次重启及动态操作、带外通道、执行人、恢复责任人、已知可用镜像/配置与恢复步骤。 | 未授权不改状态；无带外或恢复方案不进行系统重启。 |
| 功能开关与协议 | 实际 `DEVICE_METADATA/localhost/nhg_fib=enabled`、warm 配置、BGP/Zebra 对 5000/5001、3000/3001、6000 及所测 SRv6/PIC 的支持；保存每个业务所走路径的证据。 | 不支持的测试能力记 BLOCKED；事先批准的产品不适用范围另记 NOT_APPLICABLE。不能因普通路由通了就认定走 NHGFIB。 |
| 状态与编排 | 实际 schema/ASIC 命名空间、`bgp` 与 `fpmsyncd-nhg` 状态键、正常 cold 建立的计数、finalizer 版本/成员、FPM 连接条件。 | 不人工补计数；仅 finalizer 退出不解锁成功判定。 |
| 时间契约 | 冷预热期限、停止服务期限、主恢复/EOIU/hold 配置、FPM 等待期限、状态可见期限、系统就绪定义；单位均显式记录。 | 未冻结有限完成期限不执行该生命周期判定；计时器配置不是完整恢复 SLA。 |
| 拓扑与流量 | 独立预期路由/NHG/PIC 图、静态/撤销流清单、速率/帧长、序号与时间戳能力、逐流预算/单位/来源、精度、排空时长。 | 无预算可做探索测量，性能为 BLOCKED；没有物理流量能力不签 G4。 |
| 证据与时钟 | 仓库外目录、日志跨重启保留方式、时钟基准及偏差、预先选定的查询范围/频率、脱敏办法。 | 无法取得某断言证据时该项 BLOCKED，已经观察到的错误仍为 FAIL。 |

操作清单必须从所选镜像/平台现有工具核验并审批，分别列出：只读版本/服务/路由/DB/硬件导出、业务建立、一次系统 warm reboot、受控路由变化、正常业务撤销及恢复动作；每项注明执行位置、精确命令、原始退出码获取方式和期限。当前没有部署平台信息，因此不猜测设备 CLI、FRR 配置或重启命令。升级、安装依赖、变更 warm 配置、停止生产者或调整调试权限不由本计划自动授权。

### 3. 最小拓扑与业务账本

```text
独立发流端 ── 入端口 ── DUT ── 出端口 A ── 邻居/接收端 A
                          └── 出端口 B ── 邻居/接收端 B
带外管理与日志采集独立于上述业务路径；实际 BGP/Zebra 位于被测系统内。
```

- **普通最小集：** 两个可区分的下一跳、一个双成员 ECMP、两条引用同一 RIB NHG 的路由，每条路由至少一条带序号的保持流。先 IPv4，再 IPv6；默认 VRF 与一个非默认 VRF 分列子例。单跳和非等权成员另作 E03 子例，不能只比较表项总数。
- **真实共享/PIC 集：** 两个不同 RIB NHG 满足 received SRv6 条件，公共下一跳相同、VPN SID 不同，再建立依赖它们的组合 PIC；生产者配置和冷态实际图都须证明共享。两条普通路由引用同一 NHG 不等于多 RIB 共享 Sonic 对象。
- **可销毁恢复探针：** 连续轮次保留承载业务的基线图，每轮 warm 之前通过真实生产者另建探针。普通轮次仅建独立 ECMP 及引用路由，供 E09-N 检查；共享/PIC 轮次再建独立共享 NHG/PIC 图，供 E09-P 检查。两者都必须销毁本轮实际经过恢复的探针对象，不能只检查恢复后新建对象的删除能力。
- **独立账本：** 每条记录包含 VRF/地址族/前缀、成员与关联权重、SID 序列、主备角色、RIB/Sonic/PIC 的关联、允许出口及流 ID。预期由配置和事件语义预先给出，不调用 Assist 或活管理器 `setEntry()` 生成 oracle。
- **流量覆盖：** 每个出口接收端在 DUT 重启时持续运行；使用足以覆盖两个 ECMP 成员的固定流集合，并在冷态证明实际覆盖，不假定一条流会均匀走两个成员。静态保持和受控撤销业务在发流前分组。
- **规模集：** 最小集通过后才冻结业务规模，包括路由数、NHG 数/依赖层级、共享者数、PIC 数和总发包速率。数值来自授权测试目标，不根据结果调整到容易通过的范围。

### 4. 每轮系统 warm 的实际步骤

1. **建立轮次和冷基线。** 第一轮用正常冷启动与真实生产者建立业务，不能靠手工写 APP_DB 种 NHG。核验输入、环境、夹具后，检查 BGP/邻居、实际 NHGFIB 路径、路由→NHG→成员/PIC→SID 引用及端到端接收。先按不低于 60 秒的预定稳定期观察；背景异常停止验收。cold PASS 只把 warm 解锁为 NOT_RUN；已核验 cold FAIL 保留 FAIL，warm BLOCKED。后续轮次沿用上一轮的合法状态，不清库、不人工补计数。
2. **保存 S0 并开始持续测量。** 导出范围内的生产者路由、APPL_DB、APPL_STATE_DB、STATE_DB 和硬件语义图；记录匹配候选 ID、完整 FV、共享引用及已有状态字段。确认日志/捕获已启动、有实际发送/接收；固定静态业务表示，不主动制造控制面变化。不要把此时正常消息写入的状态记录误判为已经执行了停机保存。
3. **执行一次批准的系统 warm reboot。** 外部流量和日志持续采集，记录操作、重启原因、服务退出/新进程启动和系统编排证据。系统测试必须出现实际系统重启证据；仅 PID/容器变化只计进程测试。不得在同一轮手工调用 `onWarmStartEnd()`、写完成态、改计数或为赶时限重启 fpmsyncd。
4. **观察停机与恢复。** 采集信号、保存/flush、FPM accept、加载、EOIU 或主超时、NHG 恢复、route/resolve 回放及路由 reconcile。确认健康轮次没有意外 cold fallback；保存应符合停机阶段契约，不让部分图覆盖旧完整状态。`fpmsyncd-nhg=RECONCILED` 早于 route/resolve 回放结束，必须继续核对必要 flush 后的最终可见状态、引用与系统就绪条件。
5. **比较 S1 与独立预期。** 分别判断语义、适用 Sonic ID 保持、PIC 关联和硬件图。固定合法表示且完整 FV 匹配的静态对象必须保留可匹配旧 ID；多个等价候选按候选集合判断。RIB ID 与 SAI OID 不当成 Sonic ID；成员/权重按关联元组比较，SID 顺序、主备角色和重复成员不能丢。快照相同不证明中间未删除重建，缺写事件证据时该子断言 BLOCKED。
6. **封存静态证据。** 恢复后按不低于 60 秒的预定稳定期继续发流；停止发送后等待冻结的接收排空窗口，再逐流导出序列、时间戳和捕获完整性。先判证据完整，再判丢包/中断/重复/乱序预算，不把后续业务删除计入静态测流。
7. **验证恢复后的操作能力。** 静态证据封存后执行 E09-N：新增路由/NHG、改成员/权重，撤销本轮已经恢复的普通探针路由和 NHG。共享/PIC 轮次另执行 E09-P：基于已恢复依赖创建组合 PIC，依次撤销已恢复探针图的一个共享者及最后共享者。检查剩余业务不被误删、最终对象及占用释放；再创建对象检查活 ID 无碰撞。普通轮次不要求 PIC 探针。重复轮次将探针撤销至本轮预定状态；下一轮若需要探针，用真实生产者正常建立并重新记录 S0，保留承载连续恢复的基线对象和数据库，不重新建库掩盖残留。
8. **出本轮结果再决定下一轮。** 每条断言记录证据和状态；任何真实错误保留 FAIL。失败时停止相关后续重启，封存现场后只按既定授权恢复；未经批准不冷重启清理或重装镜像。重试新建 run_id，并关联原失败。

其中第 4 步的时点不能凭发送端日志补造。需要严格证明恢复窗口内入队、跨类型顺序或同进程重连时，按 L2 的 `(run_id, process_incarnation, accept_ordinal)` 标识轮次，并用独立的 GDB/MI 观察运行取得接收证据；性能样本另跑无调试器版本。没有该能力，窗口覆盖断言 BLOCKED，最终状态仍可单独判定。

### 5. 用例清单与执行顺序

下列均为拟执行项，尚无本轮结果。E 编号是本节执行编号，括号内对应规范编号；不能把 12 个父项当作只有 12 个测试。

| 编号 / 阶段 | 输入和操作 | 必验结果及证据 | 环境 |
|---|---|---|---|
| E00 / 前检（C01） | 核对实际构建、镜像、依赖、原 15 例和全量回归；重点查看 ID 诊断用例。 | 版本可追踪、原始退出码和测试清单/XML 齐全，无相关未解决 FAIL；只完成构建不等于此项 PASS。 | 已有 Linux 构建产物；不在本机重新编译。 |
| E01 / 冷预热（C02、L03） | 按最小拓扑通过真实生产者建立业务；核对两个下一跳、两条路由、NHGFIB 路径、流量与正常计数。 | 独立图与冷转发正确；前提真实核验后才给 cold PASS。cold 错误不归咎于尚未执行的 warm。 | 所选系统；L2 使用已验证确定性生产者。 |
| E02 / 首次静态（C03、C04、L03、L04、S01） | 首个正常 cold 后进行一次系统 warm，不人工补计数，保持固定输入表示。 | 真正走 warm；适用旧 ID 保持、路由/成员引用闭合、状态保存/保持及流量预算满足；ID 1→2 类回归不可豁免。 | 虚拟系统功能或物理 DUT；L2 对应项仅报告进程恢复。 |
| E03 / 普通扩展（C03、C04、C08、S01） | IPv4/IPv6、单跳/ECMP/非等权、默认/非默认 VRF；另分 RIB 改号和成员表示重排。 | 语义正确；不创建 Sonic 对象的单跳不要求保存项；表示变化不承诺精确 FV 算法一定保 ID，必须单列 churn，不能放宽 E02。 | 协议和平台支持的系统。 |
| E04 / PIC 静态（C03–C05、C08、S02） | 真正多 RIB 共享、不同 VPN SID、组合 PIC；普通/封装/SRv6 路由分别核对 suppression 开/关。 | NHG 与 PIC 独立关联，SID/主备/路由引用正确；封存测流后验证 PIC 查询、组合新建及共享释放。没有内部观察时相关内存断言 BLOCKED，外部行为另判。 | 兼容生产者和 SRv6/PIC 平台；DB 注入用例不能代替。 |
| E05 / 窗口内增删改（C06、C08、S03） | 独立轮次施加同对象 NEW→DEL、DEL→NEW、多次成员/权重变化、路由/依赖撤销；每种序列列独立子例。 | 按事件账本检查最终图，DEL 真正生效、保留业务不误删；保持流和撤销流分别判定。只有发送记录时不声明窗口覆盖成功。 | 系统真实生产者；精确时序需独立 L2 观察。 |
| E06 / resolve（C07、S02、S03） | 6000 分别早于/晚于 NHG 与路由，撤销→恢复→再次撤销；恢复结束后再做实时变化。 | PIC fix-up/backwalk、主备关系、允许出口和最终状态符合账本，无丢事件/重复应用；每阶段消费证据不足则不声称逐事件通过。 | 支持 6000 的生产者；确定性先后顺序子例需 L2 能力。 |
| E07 / 定时器（L01、L02） | 独立测试：空 ROUTE_TABLE 且有/无旧 NHG；非空表双栈 EOIU/单族/无 EOIU；保持连接直到恢复结束。 | 空表主定时器真实启动并结束 Assist；非空按 EOIU+hold 或主超时触发；两个触发先后到达不重复应用，不无限缓存。 | 专用 L2 夹具；不在系统静态轮次中清 APP_DB 造空表。 |
| E08 / 生命周期与负例（C10、L03–L05） | 同进程 FPM 重连/新进程恢复分开；正常、首次 accept 前、恢复中、重连时 SIGTERM，另测 SIGINT；缺计数和损坏状态分开。 | 干净有限退出、阶段正确保存/保持、无旧队列泄入下轮、活 ID 不冲突；健康首次 cold fallback 是 FAIL，故意缺计数的预期安全降级不能计 warm 成功。 | 只在隔离 L2 按 B 计划执行；畸形消息 C09 仍限隔离组件环境。 |
| E09 / 恢复后更新（C04、C05、S01、S02） | E09-N：普通路由/NHG 新建与修改、删除已恢复 ECMP 探针后再分配；E09-P：基于已恢复依赖创建组合 PIC、删已恢复探针共享者及最后共享者。均在测流封存后执行。 | 保留业务/共享对象不早删，撤销对象及占用释放，NHG/PIC 各自非零活 ID 不碰撞；计数不回退不是泄漏证明。 | 普通首轮只要求 E09-N；E09-P 随 PIC 轮次执行。细粒度内存/占用断言另需 L1/L2 证据。 |
| E10 / 重复与规模（C10、L03、S04） | 最小集静态、动态各用独立序列；相关关卡通过后再做规模集；每个冻结序列连续三轮。 | 每轮独立记录，首轮为正常冷预热后的首次 warm，后两轮不清库；对象/引用残留按确定性契约，RSS/CPU/耗时按冻结预算。 | 所选系统；物理性能只使用物理数据面样本。 |
| E11 / 汇总（G1–G4） | 列出全部必测子例、每流/每轮结果、未执行/阻塞/不适用范围及重试关系。 | 对应范围只有全部必测项 PASS 才可通过；不把 E02 单轮成功、DVS 或离线测试当完整 S01–S04 验收。 | 离线报告，不自动提交或上传。 |

首轮最小闭环是诊断与功能推进顺序，不等于跳过 G1/G2 后签署 G3/G4。相关正确性问题或必测能力仍缺失时，物理验收保持未通过；另行授权的隔离诊断可以继续，已确认失败不改记环境问题。

### 6. 已有执行入口与尚缺能力

以下命令仅供后续在**已经授权、准备完成的 Linux 环境**执行，本次没有执行。每个目标使用独立运行目录，由执行器保存完整 stdout/stderr、原始退出码和实际测试数；不通过日志尾部命令掩盖失败，也不把零测试、skip/xfail 当通过。

**已有 GTest 产物：** 在同构建工作区的 `tests/mock_tests` 目录运行；若缺少该二进制或不能确认版本，则先取得产物/已有日志，不在本机重新构建。`EVIDENCE_DIR` 必须由执行方设为已存在的仓库外绝对目录。

```bash
: "${EVIDENCE_DIR:?必须设置为本轮已存在的证据目录}"
./tests_fpmsyncd --gtest_list_tests
./tests_fpmsyncd --gtest_filter='FpmSyncdNhgWarmAssist.WarmRestart_EndToEnd' --gtest_output="xml:${EVIDENCE_DIR}/id-reuse.xml"
./tests_fpmsyncd --gtest_filter='FpmSyncdNhgWarmAssist.*' --gtest_output="xml:${EVIDENCE_DIR}/assist.xml"
./tests_fpmsyncd --gtest_output="xml:${EVIDENCE_DIR}/fpmsyncd.xml"
```

主 `tests` 及其他 mock 全量回归沿用 [A 子计划](2026-10-26-sonic-swss-warmreboot-validation-components.md) 的清单核验，不能只用这四条命令声称 G1 全覆盖。`[nhg-warm] cold/restored/replayed/reconciled` 是 `d0faaf10` 在 **GTest stderr** 的诊断，不是设备守护进程日志。用它对比 allocator、候选 FV、计算 FV、旧/新 ID 与最终键；当前仅有 ifindex/name 变化的待验证假设，不能先判定一定是夹具或生产代码问题。

**DVS 现有 smoke：** 已核验版本且专用于 smoke 的 `vs` 持久容器，在仓库 `tests` 目录可使用下列入口（依据 `tests/README.md:150-153,189-195`）。它修改 warm 标志和计数，因此必须与健康首次 warm 的环境隔离，不得在首次基线之前运行，也不自动启动/删除容器或使用 `--forcedvs` 修改端口。

```bash
: "${EVIDENCE_DIR:?必须设置为本轮已存在的证据目录}"
sudo pytest --dvsname=vs test_warm_reboot.py::TestWarmReboot::test_FpmsyncdWarmRestart -v --junitxml="${EVIDENCE_DIR}/fpmsyncd-smoke.xml"
```

**离线判据回归：** 仓库根目录的标准库入口如下，只验证判据逻辑，不建立冷预热或发流。

```bash
PYTHONDONTWRITEBYTECODE=1 PYTHONPATH=tests python3 -m unittest discover -s tests/dvslib -p test_nhg_warm_restart_result.py -v
```

**不能直接调用的能力：** 本仓库尚无完整 NHG 系统 warm E2E 执行器、物理流量闭环、已落地的专用 TCP NHGFIB 生产者/独占夹具/GDB 观察控制器；原 C02 的 `missing_fields()` 也未实现。确定性 L2 子例依赖 [B 子计划](2026-10-26-sonic-swss-warmreboot-validation-dvs.md) 实施；真实系统步骤则用审批后的平台操作清单执行，不需要先制造一套远程自动化框架。现有 `test_srv6.py` 的 APP_DB 注入/ASIC 断言只能作局部回归，不能代替真实生产者 cold→warm。

### 7. 证据包、判据与停止规则

| 证据类别 | 每轮最少内容 |
|---|---|
| 版本与输入 | manifest、授权引用、操作清单、预期图、固定/变化表示分类、逐流预算、事件账本、run_id/重试关系。 |
| 状态 S0/S1 | 生产者路由、APPL_DB 的 NEXTHOP_GROUP_TABLE/PIC_CONTEXT_TABLE/相关路由与 SRv6 表，APPL_STATE_DB 的 NHG_FULL_STATE_TABLE 与 allocator，STATE_DB warm/EOIU；物理层另有 ASIC_DB 和支持的只读硬件图。使用实际 schema/命名空间，不假设固定 DB 编号或分隔符。 |
| 生命周期 | 系统重启与进程轮次、信号、保存/flush、FPM 连接、加载、触发定时器、NHG/route/resolve 完成与系统就绪；缺少某阶段日志不补写成观察结果。停机状态表有正常写入与保存写入的不同字段契约，保存 JSON 不是实际重建输入。 |
| 操作与引用 | 固定表示 ID 候选、完整关联 FV、PIC/SID/主备、适用的增删事件、E09 操作及结果。事件观察区分合法新建/撤销/状态上报与静态对象不必要的删除重建，不要求所有 DB 全程零写入。 |
| 流量 | 冻结流清单、逐流发送/接收序号与时间戳、序号回绕处理、背景观测、完整捕获/排空证据、丢失/重复/乱序/错误出口、最大中断区间。接收端与 DUT 必须独立。 |
| 结果 | 每条断言的 purpose、expected_outcome、证据路径、状态和原因，实际测试数/XML/退出码，以及所有未执行和阻塞项。原失败保留，不被重试覆盖。 |

统一判定：

- **cold 门禁：** 三项前提严格核验后，cold FAIL → warm BLOCKED；cold PASS → warm NOT_RUN。不以离线合成 PASS 或人工补计数解锁真实 warm。
- **证据门禁：** 零发送、缺必测流、捕获窗口不完整均为 BLOCKED；完整窗口有发无收是真实丢包，在零丢包预算下 FAIL。`traffic_evidence_status()` 的 PASS 只证明基础证据完整，每条流还须判性能。
- **语义与保持：** 错路由、悬空引用、错误 SID/主备、活 ID 覆盖、健康场景 cold fallback、崩溃、持续黑洞或超过冻结完成期限均为 FAIL。缺写事件时“无 churn”是 BLOCKED，不用两张快照相等替代。
- **时间与流量：** 分别记录 `T_logic`（加载至 NHG/route/resolve 与路由核对调用结束）、`T_restore`（加载至必要 flush 后期望状态可见）、`T_system`（发起重启至预定系统就绪）。保留时钟/轮询误差区间：上界不超预算才 PASS，下界超预算 FAIL，跨预算或缺预算 BLOCKED。最大中断使用仪表时间戳或校准恒定速率，不用简单 TX−RX 抵消重复。
- **结果汇总：** `FAIL > BLOCKED > NOT_RUN > PASS`；全 NOT_APPLICABLE 不是 PASS。未观测到内部事件不等于事件正确，缺证据不能抹掉其他已确认 FAIL。虚拟功能与物理性能分别报告。
- **失败停止与清理：** 失败停止相关序列，剩余轮次 NOT_RUN；先保留日志、状态、捕获与退出码，再使用已批准恢复方案。只撤销本次业务/恢复本次改变的服务状态，只清理本次创建的专用资源，不清共享 Redis、不删除他人容器、不改管理配置；重装、冷重启和现场删除不默认授权。

本轮交付是这份执行计划；下一执行阶段首先获取新构建的实际版本/日志和所选环境的操作清单，E00 未核验前不填写测试成功。本轮不创建单独 runbook、不实现测试工具、不启动任何 E 场景。

## 原实现任务：依赖与文件图

以下保留 2026-10-26 的 C01–C06 工作分解及当时拟建文件。现有 Python 实现以仓库代码和 `0a0340e3` 为准，禁止照抄旧“最小实现”覆盖它；未勾选项不表示已交付，`missing_fields()`、独立 runbook 和设备结果不能当作现有能力。本节不自动授权新实现或提交。

- 依据：[主计划](2026-10-26-sonic-swss-warmreboot-validation.md)、[规范](../specs/2026-10-26-sonic-swss-warmreboot-validation-design.md)。C01 可以独立开发；C03 起需相关 A/B 正确性证据及设备授权。
- Create: `tests/dvslib/nhg_warm_restart_result.py`，仅离线判据和运行契约前检。
- Create: `tests/dvslib/test_nhg_warm_restart_result.py`，标准库单测，不加载 DVS conftest。
- Create: `docs/alinos/validation/sonic-swss-warmreboot-runbook.md`，实施阶段形成的操作清单模板和字段说明，不包含真实凭据。
- 运行产物存执行方指定的仓库外目录；实际设备配置、地址、原始转储不进入 Git。此处不创建上述实现或运行产物。

## Task 1（C01）：实现离线状态、区间与序列判据

**Files:**
- Create: `tests/dvslib/nhg_warm_restart_result.py`
- Test: `tests/dvslib/test_nhg_warm_restart_result.py`

- [ ] **Step 1: 写失败测试。** 在测试文件加入下列完整断言；导入失败是首次预期 RED，不使用设备或 pytest fixture。

```python
import unittest

from dvslib.nhg_warm_restart_result import (
    aggregate,
    interval_status,
    sequence_metrics,
    traffic_evidence_status,
)


class ResultTests(unittest.TestCase):
    def test_failure_is_not_hidden_by_missing_evidence(self):
        self.assertEqual(aggregate(["PASS", "BLOCKED", "FAIL"]), "FAIL")
        self.assertEqual(aggregate(["PASS", "NOT_RUN"]), "NOT_RUN")
        self.assertEqual(aggregate(["NOT_APPLICABLE"]), "NOT_APPLICABLE")
        self.assertEqual(aggregate([]), "NOT_RUN")

    def test_interval_boundary_and_missing_budget(self):
        self.assertEqual(interval_status(8, 10, 10), "PASS")
        self.assertEqual(interval_status(11, 12, 10), "FAIL")
        self.assertEqual(interval_status(9, 11, 10), "BLOCKED")
        self.assertEqual(interval_status(0, 0, 0), "PASS")
        self.assertEqual(interval_status(0, 0, None), "BLOCKED")
        with self.assertRaises(ValueError):
            interval_status(12, 11, 10)

    def test_duplicates_do_not_cancel_loss(self):
        actual = sequence_metrics([1, 2, 3], [2, 1, 2])
        self.assertEqual(actual["lost"], [3])
        self.assertEqual(actual["duplicates"], 1)
        self.assertEqual(actual["out_of_order"], 1)
        self.assertEqual(actual["unexpected"], [])

    def test_empty_or_partial_traffic_evidence_cannot_pass(self):
        self.assertEqual(traffic_evidence_status(["keep"], {}), "BLOCKED")
        empty = {"keep": {"sent": [], "capture_complete": True}}
        self.assertEqual(traffic_evidence_status(["keep"], empty), "BLOCKED")
        partial = {"keep": {"sent": [1], "capture_complete": False}}
        self.assertEqual(traffic_evidence_status(["keep"], partial), "BLOCKED")
        valid = {"keep": {"sent": [1], "capture_complete": True}}
        self.assertEqual(traffic_evidence_status(["keep"], valid), "PASS")
        self.assertEqual(traffic_evidence_status(["keep", "withdraw"], valid), "BLOCKED")
        self.assertEqual(traffic_evidence_status([], valid), "BLOCKED")
```

- [ ] **Step 2: 运行 RED。** 仓库根执行 `PYTHONPATH=tests python3 -m unittest discover -s tests/dvslib -p test_nhg_warm_restart_result.py -v`，保留退出码；不得让旧同名模块或零测试掩盖缺能力。
- [ ] **Step 3: 最小实现。** 模块写入以下函数；序列输入为每流、每轮、排空后且已处理计数器回绕的唯一发送序号，不跨流合并。

```python
from collections import Counter
from math import isfinite


def aggregate(statuses):
    allowed = {"PASS", "FAIL", "BLOCKED", "NOT_RUN", "NOT_APPLICABLE"}
    if any(status not in allowed for status in statuses):
        raise ValueError("unknown assertion status")
    if not statuses:
        return "NOT_RUN"
    for status in ("FAIL", "BLOCKED", "NOT_RUN", "PASS"):
        if status in statuses:
            return status
    return "NOT_APPLICABLE"


def interval_status(lower, upper, budget):
    if not isfinite(lower) or not isfinite(upper) or not 0 <= lower <= upper:
        raise ValueError("invalid measurement interval")
    if budget is None:
        return "BLOCKED"
    if not isfinite(budget) or budget < 0:
        raise ValueError("invalid budget")
    if upper <= budget:
        return "PASS"
    if lower > budget:
        return "FAIL"
    return "BLOCKED"


def sequence_metrics(sent, received):
    if len(sent) != len(set(sent)):
        raise ValueError("send sequence must be unique within a flow epoch")
    positions = {value: index for index, value in enumerate(sent)}
    counts = Counter(received)
    seen = set()
    highest = -1
    reordered = 0
    for value in received:
        if value not in positions or value in seen:
            continue
        position = positions[value]
        reordered += position < highest
        highest = max(highest, position)
        seen.add(value)
    return {
        "lost": [value for value in sent if value not in counts],
        "duplicates": sum(max(count - 1, 0) for count in counts.values()),
        "out_of_order": reordered,
        "unexpected": [value for value in counts if value not in positions],
    }


def traffic_evidence_status(flow_ids, evidence):
    if not flow_ids:
        return "BLOCKED"
    for flow_id in flow_ids:
        sample = evidence.get(flow_id)
        if sample is None or not sample.get("sent") or not sample.get("capture_complete"):
            return "BLOCKED"
    return "PASS"
```

`traffic_evidence_status()` 的 PASS 只表示必测流具备非空发送和完整接收窗口证据，不是丢包预算判定；每个流仍需单独计算序列与区间结果后聚合。

- [ ] **Step 4: 运行 GREEN。** 重跑同一命令，新增空流、未知状态、非有限区间、未知接收序号和重复发送序号的断言；未知接收报文需归因，不能归并为合法业务成功。空发送、缺流或接收窗口不完整须为 BLOCKED，不能从零发送得到零丢包 PASS。最大中断不从这个函数臆算，必须使用仪表时间戳或预先校准的恒定包速率及误差区间。
- [ ] **Step 5: 检查点。** 记录实际测试数和输出，按主计划提交授权规则审阅；这只解锁离线判据，不解锁 G3/G4。

## Task 2（C02）：冻结分层运行契约及授权清单

**Files:**
- Modify: `tests/dvslib/nhg_warm_restart_result.py`
- Test: `tests/dvslib/test_nhg_warm_restart_result.py`
- Create: `docs/alinos/validation/sonic-swss-warmreboot-runbook.md`

- [ ] **Step 1: 写失败测试。** 为必填字段前检加入以下代码；合成样本只验证结构，`unit-test` 不是任何真实授权。

```python
from dvslib.nhg_warm_restart_result import missing_fields


class ContractTests(unittest.TestCase):
    def test_device_inputs_do_not_block_component_work(self):
        profile = {
            "revision": "unit-test", "image_digest": "unit-test",
            "dependencies": {"swsscommon": "unit-test"}, "architecture": "unit-test",
            "build_config": "unit-test", "test_ids": ["C01"], "evidence_dir": "unit-test",
        }
        self.assertEqual(missing_fields(profile, "L1"), [])
        self.assertIn("authorization", missing_fields(profile, "L3"))
        self.assertIn("traffic_budget", missing_fields(profile, "L4"))
```

- [ ] **Step 2: 运行 RED。** 使用 C01 的 unittest 命令，仅缺新增前检函数时是预期 RED。
- [ ] **Step 3: 实现必填集合。** 使用下面的精确分层字段；非空字段检查只证明结构完整，不证明版本兼容或授权有效，真实证据还需人工核对。

```python
LAYER_FIELDS = {
    "L1": ("revision", "image_digest", "dependencies", "architecture", "build_config", "test_ids", "evidence_dir"),
    "L2": ("exclusive_dvs", "protocol_capabilities", "symbol_capabilities", "timeouts", "observation_mode"),
    "L3": ("device", "asic_namespace", "producer_version", "finalizer_members", "topology", "timeouts", "system_ready_definition", "clock_basis", "authorization", "rollback"),
    "L4": ("flows", "traffic_budget", "measurement_precision", "drain_window_ms"),
}


def missing_fields(profile, layer):
    if layer not in LAYER_FIELDS:
        raise ValueError("unknown validation layer")
    required = list(LAYER_FIELDS["L1"])
    if layer == "L2":
        required.extend(LAYER_FIELDS["L2"])
    if layer in ("L3", "L4"):
        required.extend(LAYER_FIELDS["L3"])
    if layer == "L4":
        required.extend(LAYER_FIELDS["L4"])
    return [key for key in required if key not in profile or profile[key] in (None, "", [], {})]
```

- [ ] **Step 4: 完善清单并运行 GREEN。** 增加 `missing_fields({}, "invalid")` 抛错、L3 不要求 GDB、零丢包预算不被当缺值的测试。运行手册为每个字段列单位、来源和责任方，包含下列检查点：

| 输入 | 执行前必须核对的内容 | 缺失处理 |
|---|---|---|
| 版本与拓扑 | 实际镜像摘要、源码、库、FRR、平台/ASIC 命名空间、双栈/VRF/ECMP/SRv6/PIC 能力及规模。 | 对应层 BLOCKED，不用邻接仓库 HEAD 冒充部署版本。 |
| 操作授权 | 设备唯一标识、隔离用途、起止窗口、允许的重启/动态变更、停止条件、执行人与恢复责任人。 | 不发送任何改变设备状态的命令。 |
| 操作映射 | 执行方从所选平台现有工具填写并审批：只读状态导出、单次 warmreboot、受控路由变化、已知可用恢复动作及原始退出码取法。 | 未冻结该映射即不具备可执行设备步骤；不猜命令、接口或管理凭据。 |
| 状态与计时 | 实际 `bgp`/`fpmsyncd-nhg` 状态键、finalizer 实际成员与超时、停机期限、恢复起止点及有限完成期限。 | 对应生命周期断言 BLOCKED，不看计数或 finalizer 退出直接通过。 |
| 流量 | 流标识、固定保持/受控撤销分组、包速率/帧长、逐流丢包/中断/重复/乱序预算、单位、精度、接收排空窗口。 | 无预算可探索测量，不签署性能 PASS；不用事后测量反推预算。 |
| 安全与证据 | 脱敏方式、仓库外目录、时钟基准/偏差、只读查询范围、现场保留与已批准恢复方案。 | 不全库高频扫描、不上传敏感数据、不清理他人资源。 |

- [ ] **Step 5: 检查点。** 运行字段前检与 C01 单测，人工核对实际授权与能力证据。授权、平台和 SLA 是合法外部输入，不用虚构样本填满字段解锁设备运行。

## Task 3（C03）：最小拓扑静态与健康首次恢复

**Files:**
- Modify: `docs/alinos/validation/sonic-swss-warmreboot-runbook.md`
- Evidence: C02 指定的仓库外 `evidence_dir`，按新的 `run_id` 分目录。

此任务是授权运行清单，不新增设备控制代码，也不存在以合成数据代替的 TDD GREEN。

- [ ] **Step 1: 前置关卡。** 核对 C02 的 L3/L4 契约、相关 A/B 结果及审批命令映射；正确性 FAIL 未解决的范围不能进入签署验收，可另外批准隔离诊断。确认冷预热是实际正常生产者建立业务，不人工补 restore_count。
- [ ] **Step 2: 稳定基线。** 使用两个下一跳、一个 ECMP NHG、两条路由的普通拓扑；另设真正的多 RIB shared SRv6/PIC 拓扑。收集明确的路由/成员/权重/VRF/SID 预期和适用 ID 候选集，至少观察预先冻结的稳定期；背景异常则停止验收，不在统计中扣掉未知背景丢包。
- [ ] **Step 3: 单次授权动作。** 仪表持续运行时只执行命令映射中的一次 warmreboot；记录启动、保存/flush、退出、FPM 连接、加载、NHG、路由/resolve、系统就绪时点与原始日志。时间点不能观测的子断言记 BLOCKED；不要把 GDB 运行当正式流量样本。
- [ ] **Step 4: 独立状态与生命周期断言。** 比较完整路由→NHG→成员/PIC→SID 语义和引用；固定表示匹配对象保留适用 ID，SAI OID 以语义图核对。检查真实 warm 而非 cold fallback，保存 JSON 不作为唯一恢复依据。静态测流期间不删除承载被测流的业务；恢复后的正常创建/删除、组合 PIC、删除共享者及最后共享者留到 Step 5 停止发流并封存证据后执行。
- [ ] **Step 5: 流量、生命周期与失败处理。** 完成冻结的后稳定期、停止发送并等待接收排空；逐流导出序列和中断区间，先用 C01 `traffic_evidence_status()` 核对冻结流清单、非空发送和完整窗口，缺证据的流记 BLOCKED，不能按零丢包通过。随后按 C01 判据与冻结预算判流；证据封存后再执行正常创建/删除、组合 PIC、删除一个共享者及最后共享者，核对引用和各命名空间占用释放，且不得回写已封存流量证据。无写事件观测时“无删除重建”子项 BLOCKED，其他实际错误保留 FAIL；出现黑洞、崩溃、管理失联或状态破坏即停止后续动作并按授权恢复方案处理。

## Task 4（C04）：受控增删改与 resolve 收敛

**Files:**
- Modify: `docs/alinos/validation/sonic-swss-warmreboot-runbook.md`
- Evidence: 新 `run_id`，不复用 C03 的静态结果目录。

- [ ] **Step 1: 冻结账本。** 预先声明每条变化的对象键、NEW/DEL/resolve 语义、触发条件、保持流和撤销流。最小事件序列为目标依赖撤销→恢复→再次撤销；另列同 NHG NEW→DEL、DEL→NEW、多次权重/成员更新和共享引用释放，不根据设备实际输出生成预期。
- [ ] **Step 2: 建立新基线。** 按 C03 的已验证步骤建立业务，核对背景稳定及新 run 的状态快照；停止上一轮的调试观察，不复用其消费截止点。
- [ ] **Step 3: 执行变化。** 在已批准的系统恢复期间通过真实 BGP/Zebra 生产者施加账本变化；保留生产者、接收端可用日志及时间偏差。若只有发送证据，没有接收窗口证据，该窗口覆盖断言为 BLOCKED，最终语义仍须核对，不能捏造精确消费先后。
- [ ] **Step 4: 验证最终状态。** 每个合法事件生效一次、无过期引用/错出口，后续实时 6000 仍能收敛；检查 PIC fix-up/backwalk、路由 suppression 相关结果。按账本语义判定，不能按当前分队列回放顺序倒推预期。
- [ ] **Step 5: 独立判流。** 保持流和撤销流都先用 `traffic_evidence_status()` 核对冻结清单、非空发送和完整窗口，缺证据的流记 BLOCKED；保持流执行静态预算，撤销流执行预定收敛预算，不混入保持流也不从结果中删除。运行失败保留所有证据；重新运行必须新 run 并引用原失败，不能静默重试直到通过。

## Task 5（C05）：最小集与规模集分别连续三轮

**Files:**
- Modify: `docs/alinos/validation/sonic-swss-warmreboot-runbook.md`
- Evidence: 每个拓扑集合三个不同 run_id，关联同一冻结契约。

- [ ] **Step 1:** 最小集静态/动态相关关卡满足后，冻结规模集路由数、NHG 层级、共享者数量、PIC 数量和流量负载；不能临时扩大到未授权规模。
- [ ] **Step 2:** 同一集合每次只批准并执行一轮，第一轮是正常冷预热后的健康首次恢复，后续两轮不重建干净数据库掩盖残留；每轮独立执行 C03/C04 适用断言并留证。
- [ ] **Step 3:** 各轮核对引用、对象、ID 占用和清理后的确定性资源；RSS、CPU、峰值缓存和耗时按契约记录。计数不回退不等于泄漏，活对象被覆盖或应释放占用残留也不能因 RSS 平稳而豁免。
- [ ] **Step 4:** 有失败停止剩余循环并记录剩余 NOT_RUN；只有对应修复/环境恢复且取得继续许可后才新建运行序列，不覆盖第一次失败。
- [ ] **Step 5:** 每流先用 `traffic_evidence_status()` 核对非空发送与完整窗口，缺证据的流/轮记 BLOCKED；再逐流、逐轮独立汇总并按 C01 聚合。三轮只证明这些条件下的有限重复样本，不形成长期无泄漏或所有平台无损声明。

## Task 6（C06）：形成限定范围的验收报告

**Files:**
- Modify: `docs/alinos/validation/sonic-swss-warmreboot-runbook.md`
- Evidence: 本地运行报告，默认不上传、不提交。

- [ ] **Step 1:** 每条断言填写实际证据路径、purpose、expected_outcome、状态及原因；负例安全降级的 PASS 不能计为健康 warm 成功次数，不适用必须有运行前范围依据。
- [ ] **Step 2:** 使用 C01 的 `aggregate()` 汇总，不遗漏必测子例；缺运行的条目记 NOT_RUN，不从清单中删除。流量项先以 `traffic_evidence_status()` 检查冻结流、非空发送和完整窗口，缺证据记 BLOCKED；所有必测子项通过才允许该限定范围 PASS。
- [ ] **Step 3:** 报告分列 `T_logic`、`T_restore`、`T_system`，标明下上界、单位、精度和预算来源；调试运行只列因果，不参与性能排名或 SLA 判定。
- [ ] **Step 4:** 对照主计划 19 个父场景列出实际子例 ID、平台/版本/规模、测试次数、通过/失败/阻塞/不适用，以及未覆盖边界；不要用 DVS 或离线单测替代缺失的物理证据。
- [ ] **Step 5:** 保留原失败及重试关系，提出需要独立审批的修复或下一轮执行输入。仅在用户批准报告提交/上传及脱敏范围后进行对应动作；本任务默认只生成本地结果。
