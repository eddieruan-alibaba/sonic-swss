# SONiC SWSS warmreboot 测试与验收提案

- 变更标识：`sonic-swss-warmreboot-validation`
- 日期：2026-10-26
- 状态：初始提案，尚未完成验收
- 对象：`ribfib_3_wb_ai`，当前提交 `a06bfd59`，迁移前基线 `6fdc24fe`

## Why

NHG warm restart 已迁入更新后的 RIB/FIB 基线，但代码迁移和空白检查不能证明恢复正确：现有 15 个 NHG mock 用例尚未运行，新增的 resolve-via 事件缓冲和空路由表定时器适配缺少专项覆盖。需要在继续研发前建立分层测试、状态一致性与真机流量验收标准，避免将进程启动或 mock 恢复误判为系统 warmreboot 成功。

## What Changes

- 定义四层验证：Linux 编译与单测、真实进程/DVS 集成、设备状态恢复、物理数据面流量。
- 为状态持久化、Sonic NHG/PIC ID 复用、分配器防冲突、过期对象清理建立可核对的恢复前后证据。
- 将 `RTM_NEWRESOLVEDEVENT=6000` 延迟回放和空 `ROUTE_TABLE` 恢复结束列为迁移必验项，同时覆盖 EOIU、超时、首次及连续 warmreboot、FPM 重连和停机信号。
- 设计 IPv4/IPv6、VRF、ECMP、SRv6/PIC、共享/多级 NHG 的验证矩阵；静态保持与恢复窗口内的增删改分别验收。
- 显式跟踪来源实现的已知缺口，包括 NHG DELETE 被跳过、重复初始化未清原始缓存，以及恢复路径和正常路径的输入解析差异；不得用现有实现行为替代正确性标准。
- 定义 `PASS`、`FAIL`、`BLOCKED`、`NOT_RUN`、`NOT_APPLICABLE` 五类结果，以及环境、用例、日志、数据库快照、流量结果之间的关联方式。

本轮仅产出提案、初始设计和中文汇总设计；不增加测试代码、不修业务逻辑、不改 CI 或安全配置、不构建镜像、不操作设备、不提交文档。后续自动化实现及设备执行需要分别进入规划和授权阶段。

## Capabilities

### New Capabilities

以下是后续细化需求的能力边界，本轮不生成逐能力规范或 `tasks.md`。

- `nhg-warmrestart-component-validation`：组件测试、恢复期消息分类与回放、ID/FV 匹配、异常输入和既有 FPM 行为回归。
- `nhg-warmrestart-lifecycle-validation`：使用真实守护进程和 Redis 验证启动、停机、EOIU/超时、空表、重连及连续恢复。
- `nhg-warmreboot-state-acceptance`：在受控设备上核对路由、NHG、PIC、分配器与硬件转发表的一致性。
- `nhg-warmreboot-traffic-acceptance`：通过外部流量仪测量丢包、中断、重复/乱序和恢复后转发，形成有适用范围的验收结论。

### Modified Capabilities

无现有 OpenSpec 能力规范需要变更。本提案不承诺新的生产接口或修改路由协议行为。

## Impact

| 范围 | 当前依据与后续影响 |
|---|---|
| NHG 组件测试 | `tests/mock_tests/fpmsyncd/nhgWarmRestartAssist_ut.cpp:79` 起有 15 个用例；`WarmRestart_EndToEnd` 仅重建 mock 中的管理器，不是设备端到端测试。后续优先在现有测试文件补充断言。 |
| FPM 集成 | `tests/mock_tests/fpmsyncd/test_fpmlink.cpp:55`、`tests/mock_tests/fpmsyncd/test_routesync.cpp:159` 可复用消息构造与正常路径断言；当前测试目标使用 fake helper，不能证明 daemon 主循环和信号行为。 |
| DVS | `tests/test_warm_reboot.py:2478` 的 fpmsyncd 用例仅启停；`:1123` 起的完整路由 warm restart 用例被跳过。后续新增小型定向场景，不直接解除整个旧用例的跳过标记。 |
| 构建环境 | 依照 `build-env/README.md:9` 使用可信 Linux/Bookworm 构建环境。已有镜像拉取被 TLS 证书验证失败阻塞，编译与单测尚未执行；禁止关闭证书验证来解锁。 |
| 数据与协议 | 核对 `APPL_STATE_DB/NHG_FULL_STATE_TABLE`、`APPL_DB/NEXTHOP_GROUP_TABLE`、`PIC_CONTEXT_TABLE`、路由/SRv6 相关表和 `STATE_DB` 恢复状态；设备需使用兼容 5000/5001、3000/3001、6000 消息的路由栈与相关库。 |
| 设备与流量 | 需要已授权、可恢复的隔离测试设备、兼容镜像和外部流量仪；DVS 不提供物理数据面验收证据。设备、规模和性能预算未冻结时，只能记录测量或阻塞，不能签署性能达标。 |

初始设计见 [测试与验收设计](../specs/2026-10-26-sonic-swss-warmreboot-validation-design.md)，项目级摘要见 [汇总设计](../sonic-swss-warmreboot-validation_design.md)。
