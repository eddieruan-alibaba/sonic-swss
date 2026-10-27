# SONiC SWSS NHG Component Validation Implementation Plan

> **For agentic workers:** Use /alinos.subagent-dev (recommended) or /alinos.executing-plans to implement this plan task-by-task.

**Goal:** 扩展现有 fpmsyncd 组件用例，验证 NHG 恢复、6000 回放、共享/PIC 生命周期、删除与边界输入，并取得真实编译及回归结果。
**Architecture:** 优先在既有 GTest 文件中增加确定性消息和独立断言，测试侧可观测性只修改 mock，不修改生产恢复实现。明确 fake helper、普通路由分发和真实 daemon 的边界；发现产品 RED 进入独立修复审批，不通过改断言或豁免缺陷实现 GREEN。
**Tech Stack:** C++/GTest、Autotools、nexthopgroup JSON、netlink、现有 Redis mock、可信 Linux/Bookworm。

## 文件图与共同约束

- Modify: `tests/mock_tests/fpmsyncd/nhgWarmRestartAssist_ut.cpp:38-77,260-299,475-510`，fixture、消息构造及新增测试。
- Modify: `tests/mock_tests/fpmsyncd/test_routesync.cpp:131-221,1598-1608,1865-1956`，复用本文件 static NHG/route builders，增加真实 RouteSync 分发断言。
- Modify: `tests/mock_tests/mock_table.h:8-11`、`tests/mock_tests/mock_table.cpp:18-21,86-127,159-182`，按需启用的测试侧操作日志，默认关闭并随 reset 清理。
- Modify: `tests/mock_tests/fake_warmstarthelper.cpp:6-8,61-70,92-97`，只增加测试侧 refresh map 快照，不改变 fake reconcile 语义。
- Modify: `tests/mock_tests/fpmsyncd/ut_helpers_fpmsyncd.h`、`.cpp:562-687`，仅在多个 TU 真正共享时抽取新测试构造/声明；不改变既有随机 helper 对所有旧用例的行为。
- Modify: `tests/mock_tests/Makefile.am:324-351`，仅为接口解析的测试 wrapper 增加链接 wrap，不改变生产链接，也不同时链接真实与 fake WarmStartHelper。
- Read only: `fpmsyncd/nhgWarmRestartAssist.{h,cpp}`、`nhgmgr.{h,cpp}`、`routesync.cpp`、`fpmlink.cpp`、`fpmsyncd.cpp`。

代码块是拟写入测试的示例，尚未编译。没有 `setEntry()` oracle；预期字段使用显式值。既有 fixture 用 `#define private public` 访问 Assist 私有字段，沿用该测试方式，不新增生产 getter。独立 source file 的 static helper 不能跨 TU 直接调用。

## Task 1（A01）：可信环境、真实 daemon 与原有基线

**Files:**
- Read: `build-env/README.md:9-16`、`configure.ac:74-78,173-175`、`fpmsyncd/Makefile.am:3-17`。
- Modify: 无；日志写执行方指定的仓库外目录。

- [ ] **Step 1:** 冻结提交、工作区差异、镜像摘要、实际架构、编译器、swsscommon/nexthopgroup 版本及日志目录；可信 Bookworm 容器不可用时保留 TLS 错误和 BLOCKED，不改信任或卸载 SAI。
- [ ] **Step 2:** 按 README 准备已批准容器；首次 `env_init.sh` 会创建本地设置，先确认不是覆盖已有定制。进入 Linux 容器仓库根后执行以下命令，保留原始退出码：

```bash
set -euo pipefail
./autogen.sh
./configure
make
test -f tests/mock_tests/Makefile
make -C tests/mock_tests tests_fpmsyncd
```

mock Makefile 仅在无 SAI 的配置下生成；缺目标是环境前提未满足，不跳过后宣称成功。

- [ ] **Step 3:** 在 `tests/mock_tests` 运行清单和旧 15 例，先取修改前真实基线：

```bash
set -euo pipefail
./tests_fpmsyncd --gtest_list_tests
./tests_fpmsyncd --gtest_filter='FpmSyncdNhgWarmAssist.*' --gtest_output=xml:nhg-baseline.xml
./tests_fpmsyncd --gtest_output=xml:fpmsyncd-baseline.xml
make check
```

- [ ] **Step 4:** 核对 `FpmSyncdNhgWarmAssist` 15 个实际声明逐一执行，过滤为空不是通过。编译告警/测试失败分别定位，不去掉 `-Werror/-Wconversion`；受环境阻塞时不执行后续假基线。
- [ ] **Step 5:** 记录基线运行 ID、每条命令与退出码。没有本任务的完整证据，不能宣布组件已验证；本步骤无代码可提交。

## Task 2（A02）：确定性构造与测试侧操作证据

**Files:**
- Modify/Test: `tests/mock_tests/fpmsyncd/nhgWarmRestartAssist_ut.cpp`
- Modify: `tests/mock_tests/mock_table.h`、`tests/mock_tests/mock_table.cpp`

- [ ] **Step 1:** 在 Assist UT 的 namespace 内新增下面的确定性辅助函数；`ifindex=0` 只隔离这些组件用例的宿主接口查询，不覆盖正常接口解析。头部在 namespace 外增加 `<stdexcept>`、`fpmsyncd/routesync.h` 及全局声明 `extern void resetMockWarmStartHelper();`，保留现有 private 访问 include 的顺序。

```cpp
static NextHopGroupFull fixedNhg(NextHopGroupFull nhg)
{
    nhg.key = nhg.id;
    nhg.vrf_id = 0;
    nhg.ifindex = 0;
    nhg.ifname = "Ethernet0";
    nhg.rmap_src = {};
    return nhg;
}

static nlmsghdr *rawHeader(std::vector<uint8_t> &bytes)
{
    return static_cast<nlmsghdr *>(static_cast<void *>(bytes.data()));
}

static void queueNhg(NhgWarmRestartAssist &assist, const NextHopGroupFull &nhg,
                     uint16_t command, uint8_t af)
{
    auto bytes = buildNHGRawMsg(nhg, command, af);
    assist.bufferNHG(rawHeader(bytes));
}

TEST_F(FpmSyncdNhgWarmAssist, DeterministicMessageRoundTrip)
{
    auto nhg = fixedNhg(createSingleIPv4NextHopNHGFull("10.0.0.1", "0.0.0.0", 100));
    nlohmann::ordered_json encoded = nhg;
    auto decoded = encoded.get<NextHopGroupFull>();
    EXPECT_EQ(decoded.id, 100u);
    EXPECT_EQ(decoded.ifindex, 0u);
    EXPECT_EQ(nlohmann::ordered_json(decoded), encoded);
    auto bytes = buildNHGRawMsg(nhg, RTM_NEWNHGFIB, AF_INET);
    EXPECT_EQ(rawHeader(bytes)->nlmsg_len, bytes.size());
}
```

- [ ] **Step 2:** 单例编译/运行 `make -C tests/mock_tests tests_fpmsyncd` 后在该目录运行 `./tests_fpmsyncd --gtest_filter='FpmSyncdNhgWarmAssist.DeterministicMessageRoundTrip'`。先写调用不存在的新 helper 取得 RED，再加入上述 helper；若库 round-trip 本身失败，先核对构造而不是修改生产 parser。
- [ ] **Step 3:** 在 mock_table.h 定义新 `testing_db::Operation`（`int dbId; std::string table,key,verb; std::vector<swss::FieldValueTuple> values;`）、`captureOperations(bool)` 和 `const std::vector<Operation>& operations()`。默认不捕获，开启时清旧日志；`testing_db::reset()` 关闭并清空。只在现有 Table/ProducerStateTable 的标量 set/del 和 Table hdel 实际修改入口记录，使用 `m_pipe->getDbId()`、`getTableName()`，避免批量入口与标量入口双记。
- [ ] **Step 4:** 写操作日志单测：开启捕获后同键 SET→DEL→SET，断言依次三条事件、db/table/key/values 正确；关闭捕获后再 SET 不增加事件。以下核心断言应放在完整 fixture 测试中：

```cpp
TEST_F(FpmSyncdNhgWarmAssist, OperationLogKeepsIntermediateDelete)
{
    testing_db::captureOperations(true);
    m_nextHopTable->set("91", {{"nexthop", "10.0.0.1"}});
    m_nextHopTable->del("91");
    m_nextHopTable->set("91", {{"nexthop", "10.0.0.1"}});
    const auto &events = testing_db::operations();
    ASSERT_EQ(events.size(), 3u);
    EXPECT_EQ(events[0].verb, "SET");
    EXPECT_EQ(events[1].verb, "DEL");
    EXPECT_EQ(events[2].verb, "SET");
    EXPECT_EQ(events[1].table, APP_NEXTHOP_GROUP_TABLE_NAME);
    EXPECT_EQ(events[1].key, "91");
    testing_db::captureOperations(false);
    m_nextHopTable->set("92", {{"nexthop", "10.0.0.2"}});
    EXPECT_EQ(testing_db::operations().size(), 3u);
}
```

- [ ] **Step 5:** 对双栈、ECMP、多级、received SRv6、组合 PIC 构造分别做库 round-trip；把通过的完整 JSON 作为 B03 样本来源，实际接口参数留给 DVS 明确字段替换。单测不默认写外部文件，导出需显式运行并审阅；元数据/语义预期仍人工独立声明。运行整个 Assist suite 和受 mock_table 影响的 `make check` 后进入提交检查点。

## Task 3（A03）：类型×状态与缓存所有权

**Files:**
- Modify/Test: `tests/mock_tests/fpmsyncd/nhgWarmRestartAssist_ut.cpp`

- [ ] **Step 1:** 添加明确类型集合，分别遍历 NONE、INITIALIZED、RESTORED、RECONCILING、RECONCILED；只在中间三个状态截获指定类型。

```cpp
TEST_F(FpmSyncdNhgWarmAssist, MessageTypesRespectWarmState)
{
    for (auto state : {NhgWarmRestartAssist::NHG_WR_NONE,
                       NhgWarmRestartAssist::NHG_WR_INITIALIZED,
                       NhgWarmRestartAssist::NHG_WR_RESTORED,
                       NhgWarmRestartAssist::NHG_WR_RECONCILING,
                       NhgWarmRestartAssist::NHG_WR_RECONCILED})
    {
        setWrState(state);
        const bool active = state != NhgWarmRestartAssist::NHG_WR_NONE &&
                            state != NhgWarmRestartAssist::NHG_WR_RECONCILED;
        for (uint16_t type : std::vector<uint16_t>{5000, 5001, 24, 25, 3000, 3001, 6000})
        {
            auto raw = makeRawNlMsg(type);
            EXPECT_EQ(m_assist->shouldIntercept(rawHeader(raw)), active);
        }
        for (uint16_t type : std::vector<uint16_t>{1000, 1001, 2000, 2001, 16, 17})
        {
            auto raw = makeRawNlMsg(type);
            EXPECT_FALSE(m_assist->shouldIntercept(rawHeader(raw)));
        }
    }
}
```

- [ ] **Step 2:** 单独运行新增测试；这些合法控制头只用于分类，不传入需要完整载荷的回放函数。
- [ ] **Step 3:** 分别对三类 buffer 保存原始字节副本，调用后修改/释放输入，再比较队列字节仍等于原值，断言队列只增一。相反队列长度不变；NONE/RECONCILED 下直接调用也不得追加。
- [ ] **Step 4:** 冷启动/nhg_fib 关闭在 B09 的真实启动路径测。Assist 本身不读取配置，不能先手工 init 再期待它因开关关闭不截获；该错误测试不是生产缺陷。
- [ ] **Step 5:** 运行 `FpmSyncdNhgWarmAssist.*`，将分类和 buffer 所有权结果与真实 FpmLink 集成范围分开记录。

## Task 4（A04）：双栈单跳、ECMP 与依赖顺序

**Files:**
- Modify/Test: `tests/mock_tests/fpmsyncd/nhgWarmRestartAssist_ut.cpp`

- [ ] **Step 1:** 用固定对象 100/200 为叶、300 为父，先 buffer 父再 buffer 叶；以下用例检查独立 nexthop 语义，不以生产 setEntry 算预期：

```cpp
TEST_F(FpmSyncdNhgWarmAssist, ParentBeforeLeavesRestoresEcmp)
{
    auto a = fixedNhg(createSingleIPv4NextHopNHGFull("10.0.0.1", "0.0.0.0", 100));
    auto b = fixedNhg(createSingleIPv4NextHopNHGFull("10.0.0.2", "0.0.0.0", 200));
    auto parent = createMultiNextHopNHGFull(
        {{100, a}, {200, b}}, {{100, 1}, {200, 2}},
        {{100, 0}, {200, 0}}, {100, 200}, {}, 300);
    createStateTable();
    m_assist->initWarmStart();
    m_assist->loadState(*m_stateTable, *m_nextHopTable);
    queueNhg(*m_assist, parent, RTM_NEWNHGFIB, AF_INET);
    queueNhg(*m_assist, a, RTM_NEWNHGFIB, AF_INET);
    queueNhg(*m_assist, b, RTM_NEWNHGFIB, AF_INET);
    m_assist->reconcileNHGs();
    auto *entry = m_nhgmgr->getRIBNHGEntryByRIBID(300);
    ASSERT_NE(entry, nullptr);
    std::string value;
    ASSERT_TRUE(m_nextHopTable->hget(std::to_string(entry->getSonicObjIDNum()), "nexthop", value));
    EXPECT_EQ(value, "10.0.0.1,10.0.0.2");
    ASSERT_TRUE(m_nextHopTable->hget(std::to_string(entry->getSonicObjIDNum()), "weight", value));
    EXPECT_EQ(value, "1,2");
}
```

- [ ] **Step 2:** 编译并单独运行该例，确认结果不是随机 ifindex/VRF 造成；保留错误原始输出。
- [ ] **Step 3:** 新增 IPv6 对应例，叶为 `2001:db8::1/::2`、源 `::`；新增一个仅普通单跳例，不强制其存在 Sonic 对象或保存记录。
- [ ] **Step 4:** 再加父依赖中间组的合法多级例，以及 RIB ID 改号/表示顺序变化例；成员、权重、接口保持关联元组，语义与固定表示 ID 保持分别断言，不能独立排序字段列表。
- [ ] **Step 5:** 运行新例与旧 phase1/phase2 回归；这些通过不证明真实多 RIB shared/PIC，后者归 A06。

## Task 5（A05）：精确匹配、旧项清理与 ID 占用

**Files:**
- Modify/Test: `tests/mock_tests/fpmsyncd/nhgWarmRestartAssist_ut.cpp`

- [ ] **Step 1:** 从 A04 的固定对象经正常 `addNHGFull()` 创建业务，记录旧 Sonic ID 和独立预期；saveState 后销毁原 Assist/manager，保留 mock DB，再建新 manager 执行 load/replay。禁止通过 setEntry 的临时 dry-run 生成期望。固定表示匹配应满足：

```cpp
EXPECT_EQ(restored->getSonicObjIDNum(), savedId);
EXPECT_TRUE(m_nhgmgr->isSonicNHGIDInUsed(savedId));
EXPECT_EQ(m_nhgmgr->delNHGFull(300), 0);
EXPECT_FALSE(m_nhgmgr->isSonicNHGIDInUsed(savedId));
```

其中 `restored` 必须先 `ASSERT_NE` 检查，`savedId` 来自正常恢复前对象，不是恢复结果反向赋值。

- [ ] **Step 2:** 分别增加旧 FV 字段顺序重排、成员变化、权重变化、未匹配旧对象；固定表示只允许合法必要写入，无必要的 DEL→SET 是失败。开启 A02 日志后只对对应表/键和恢复阶段断言，不要求所有数据库零写入。
- [ ] **Step 3:** 保存 allocator 计数改为 1，同时保留较高活 ID；load 后创建新对象，要求新 ID 不覆盖旧活对象。NHG/PIC 各自命名空间独立比较，同号跨类型不报冲突。
- [ ] **Step 4:** 独立构造只在 APP_DB 存在、无保存记录的活 ID 和历史 RIB 键残留例；只有实际后续分配/删除证据才能说明冲突或残留，不凭源码风险预判运行结果。删除后查 `isSonicNHGIDInUsed()`/PIC 对应接口，不要求计数回退。
- [ ] **Step 5:** 每个子例单独运行后运行 Assist 回归；发现清理/占用缺陷留 FAIL，修复另审，不将仍存在的错误对象作为“保持成功”。

## Task 6（A06）：真实多 RIB 共享、PIC 组合与最后引用释放

**Files:**
- Modify/Test: `tests/mock_tests/fpmsyncd/nhgWarmRestartAssist_ut.cpp`
- Read: `tests/mock_tests/fpmsyncd/nhgmgr_ut.cpp:1006-1066,1485-1518`。

- [ ] **Step 1:** 新建派生 fixture，记录两个不同 VPN SID 的 shared NHG 及各自 PIC；恢复 fixture 只构造被测状态，不作为预期 oracle。

```cpp
struct FpmSyncdWarmShared : FpmSyncdNhgWarmAssist
{
    NextHopGroupFull a, b;
    uint32_t sonicId = 0, picA = 0, picB = 0;

    void SetUp() override
    {
        FpmSyncdNhgWarmAssist::SetUp();
        createStateTable();
        a = fixedNhg(createSingleSRv6VPNNextHopNHGFull("1::1", "a::a", "b::b", 100));
        b = fixedNhg(createSingleSRv6VPNNextHopNHGFull("2::2", "a::a", "b::b", 200));
        ASSERT_EQ(m_nhgmgr->addNHGFull(a, AF_INET6), 0);
        ASSERT_EQ(m_nhgmgr->addNHGFull(b, AF_INET6), 0);
        auto *ea = m_nhgmgr->getRIBNHGEntryByRIBID(100);
        auto *eb = m_nhgmgr->getRIBNHGEntryByRIBID(200);
        ASSERT_NE(ea, nullptr);
        ASSERT_NE(eb, nullptr);
        sonicId = ea->getSonicObjIDNum();
        picA = ea->getSonicPICObjIDNum();
        picB = eb->getSonicPICObjIDNum();
        ASSERT_GT(sonicId, 0u);
        ASSERT_EQ(sonicId, eb->getSonicObjIDNum());
        ASSERT_GT(picA, 0u);
        ASSERT_GT(picB, 0u);
        ASSERT_NE(picA, picB);
        m_assist->saveState(*m_stateTable);
        m_assist.reset();
        m_nhgmgr = std::make_shared<NHGMgr>(pipeline.get(),
            APP_NEXTHOP_GROUP_TABLE_NAME, APP_PIC_CONTEXT_TABLE_NAME, true);
        m_assist = std::make_shared<NhgWarmRestartAssist>(pipeline.get(), m_nhgmgr.get());
        m_assist->initWarmStart();
        m_assist->loadState(*m_stateTable, *m_nextHopTable);
        queueNhg(*m_assist, a, RTM_NEWNHGFIB, AF_INET6);
        queueNhg(*m_assist, b, RTM_NEWNHGFIB, AF_INET6);
        m_assist->reconcileNHGs();
        ASSERT_NE(m_nhgmgr->getRIBNHGEntryByRIBID(100), nullptr);
        ASSERT_NE(m_nhgmgr->getRIBNHGEntryByRIBID(200), nullptr);
    }
};

TEST_F(FpmSyncdWarmShared, LastReferenceReleasesObject)
{
    auto *entry = m_nhgmgr->getRIBNHGEntryByRIBID(100);
    const auto key = entry->getSonicNHGObjectKey();
    EXPECT_EQ(entry->getSonicObjIDNum(), sonicId);
    ASSERT_EQ(m_nhgmgr->delNHGFull(100), 0);
    EXPECT_TRUE(m_nhgmgr->isSonicNHGIDInUsed(sonicId));
    ASSERT_EQ(m_nhgmgr->delNHGFull(200), 0);
    EXPECT_FALSE(m_nhgmgr->isSonicNHGIDInUsed(sonicId));
    EXPECT_TRUE(getRibNhgTable()->getNhgMap().empty());
    EXPECT_EQ(getRibNhgTable()->getCreatedSharedNHGObjectID(key), 0);
    std::vector<FieldValueTuple> fvs;
    EXPECT_FALSE(m_nextHopTable->get(std::to_string(sonicId), fvs));
}
```

- [ ] **Step 2:** 单例运行 `FpmSyncdWarmShared.LastReferenceReleasesObject`，保留期望与实际引用/对象状态。来源代码中的临时 setEntry 可能增加引用，但未运行前不写“已复现”。
- [ ] **Step 3:** 添加独立 PIC 例，不能由上个用例的 ASSERT 提前退出遮蔽 PIC 行为：

```cpp
TEST_F(FpmSyncdWarmShared, RestoredPicSupportsComposite)
{
    const auto type = SONIC_NHG_OBJ_TYPE_NHG_WITH_SRV6_PIC_CONTEXT;
    EXPECT_EQ(m_nhgmgr->getRIBNHGEntryByRIBID(100)->getSonicPICObjIDNum(), picA);
    EXPECT_EQ(m_nhgmgr->getRIBNHGEntryByRIBID(200)->getSonicPICObjIDNum(), picB);
    EXPECT_NE(m_nhgmgr->getSonicPICByID(type, picA), nullptr);
    EXPECT_NE(m_nhgmgr->getSonicPICByID(type, picB), nullptr);
    auto parent = createMultiNextHopNHGFull(
        {{100, a}, {200, b}}, {{100, 1}, {200, 2}},
        {{100, 0}, {200, 0}}, {100, 200}, {}, 300);
    ASSERT_EQ(m_nhgmgr->addNHGFull(parent, AF_INET6), 0);
    auto *pic = m_nhgmgr->getSonicPICByRIBID(300);
    ASSERT_NE(pic, nullptr);
    const auto parentId = pic->getSonicPicContentObjIdNum();
    std::string value;
    ASSERT_TRUE(m_picContextTable->hget(std::to_string(parentId), "vpn_sid", value));
    EXPECT_EQ(value, "1::1,2::2");
    ASSERT_TRUE(m_picContextTable->hget(std::to_string(parentId), "weight", value));
    EXPECT_EQ(value, "1,2");
    ASSERT_EQ(m_nhgmgr->delNHGFull(300), 0);
    ASSERT_EQ(m_nhgmgr->delNHGFull(100), 0);
    ASSERT_EQ(m_nhgmgr->delNHGFull(200), 0);
    for (uint32_t id : {picA, picB, parentId})
    {
        EXPECT_FALSE(m_nhgmgr->isSonicPICIDInUsed(type, id));
        EXPECT_EQ(m_nhgmgr->getSonicPICByID(type, id), nullptr);
    }
}
```

- [ ] **Step 4:** 补低 allocator 计数、反向输入顺序、组合后删除一个共享者仍保留另一活引用的独立例；检查 `vpn_sid/nexthop/weight` 配对和 PIC DB 最后为空。PIC ID 数字保持和 NHG FV 相同不能代替内存 PIC 查询/后续组合能力。
- [ ] **Step 5:** 独立运行两个主例和新变体，运行整个 NHGMgr/Assist 回归；确认失败的任务保持未完成并提出独立修复，不强行修改生产接口。

## Task 7（A07）：NHG NEW/DEL 最终语义

**Files:**
- Modify/Test: `tests/mock_tests/fpmsyncd/nhgWarmRestartAssist_ut.cpp`

- [ ] **Step 1:** 新增实际删除契约，不固化“恢复忽略 DELETE”的当前行为。

```cpp
TEST_F(FpmSyncdNhgWarmAssist, NewThenDeleteLeavesNoRibEntry)
{
    auto nhg = fixedNhg(createSingleIPv4NextHopNHGFull("10.0.0.1", "0.0.0.0", 100));
    createStateTable();
    m_assist->initWarmStart();
    m_assist->loadState(*m_stateTable, *m_nextHopTable);
    queueNhg(*m_assist, nhg, RTM_NEWNHGFIB, AF_INET);
    queueNhg(*m_assist, nhg, RTM_DELNHGFIB, AF_INET);
    m_assist->reconcileNHGs();
    EXPECT_TRUE(getRibNhgTable()->getNhgMap().empty());
    EXPECT_TRUE(m_assist->m_nhg_raw_buffer.empty());
    EXPECT_FALSE(m_assist->inProgress());
}
```

- [ ] **Step 2:** 单例运行，记录图和相关 DB/操作日志；如果最终还存在 ID 100，判产品 FAIL，不把断言改为存在。
- [ ] **Step 3:** 新增 DEL→NEW（最终为 NEW 内容）、NEW→NEW 改下一跳（最终后值）、父/叶恢复期间被撤销三个独立子例；预期来自最终合法事件和引用关系。
- [ ] **Step 4:** 加普通/SRv6 路由撤销与依赖撤销的联合输入到 A08/B08，区分缓冲期间撤销和结束后的实时撤销，不仅断言队列空。
- [ ] **Step 5:** 运行全部删除相关单例和旧 NHGMgr 删除回归；生产修复获批并通过原用例前，该范围不进入验收 GREEN。

## Task 8（A08）：6000 回放及普通/raw 路由协同

**Files:**
- Modify/Test: `tests/mock_tests/fpmsyncd/nhgWarmRestartAssist_ut.cpp`
- Modify/Test: `tests/mock_tests/fpmsyncd/test_routesync.cpp`
- Modify: `tests/mock_tests/fake_warmstarthelper.cpp`、`tests/mock_tests/fpmsyncd/ut_helpers_fpmsyncd.h`

- [ ] **Step 1:** 在 Assist UT 定义 6000 的真实解析载荷；`nlmsg`、`nl_attr_put` 来自既有 UT helper。

```cpp
static nlmsg resolvedMessage(uint32_t previous, uint32_t current)
{
    nlmsg msg{};
    msg.n.nlmsg_len = static_cast<uint32_t>(NLMSG_LENGTH(sizeof(rtmsg)));
    msg.n.nlmsg_type = 6000;
    msg.r.rtm_family = AF_INET;
    nlohmann::ordered_json body = {
        {"rnh_prefix", "192.0.2.1/32"},
        {"prev_resolved_prefix", "10.0.0.0/24"},
        {"prev_resolved_nhg_id", previous},
        {"curr_resolved_prefix", current ? "10.0.0.0/24" : ""},
        {"curr_resolved_nhg_id", current}
    };
    const auto json = body.dump();
    if (!nl_attr_put(&msg.n, static_cast<unsigned int>(sizeof(msg)), NHA_JSON_STR,
                     json.c_str(), static_cast<unsigned int>(json.size() + 1)))
        throw std::runtime_error("resolvedMessage capacity");
    return msg;
}
```

- [ ] **Step 2:** 使用 RouteSync 自己的 manager，而不是 fixture 的另一个 manager。完整基础例：

```cpp
TEST_F(FpmSyncdNhgWarmAssist, Buffered6000ReplaysAfterNhgs)
{
    ::resetMockWarmStartHelper();
    createStateTable();
    RedisPipeline appStatePipeline(m_state_db.get());
    DBConnector stateDb("STATE_DB", 0);
    RouteSync sync(pipeline.get(), &appStatePipeline);
    sync.setSuppressionEnabled(false);
    auto &assist = sync.getNhgWarmAssist();
    auto &mgr = sync.getNHGMgr();
    auto a = fixedNhg(createSingleIPv4NextHopNHGFull("10.0.0.1", "0.0.0.0", 100));
    auto b = fixedNhg(createSingleIPv4NextHopNHGFull("10.0.0.2", "0.0.0.0", 200));
    auto parent = createMultiNextHopNHGFull(
        {{100, a}, {200, b}}, {{100, 1}, {200, 1}},
        {{100, 0}, {200, 0}}, {100, 200}, {}, 300);
    assist.initWarmStart();
    assist.loadState(*m_stateTable, *m_nextHopTable);
    auto down = resolvedMessage(100, 0);
    assist.bufferResolvedEvent(&down.n);
    queueNhg(assist, parent, RTM_NEWNHGFIB, AF_INET);
    queueNhg(assist, a, RTM_NEWNHGFIB, AF_INET);
    queueNhg(assist, b, RTM_NEWNHGFIB, AF_INET);
    ASSERT_EQ(mgr.getRIBNHGEntryByRIBID(300), nullptr);
    sync.onWarmStartEnd(stateDb);
    auto *entry = mgr.getRIBNHGEntryByRIBID(300);
    ASSERT_NE(entry, nullptr);
    const auto key = std::to_string(entry->getSonicObjIDNum());
    std::string value;
    ASSERT_TRUE(m_nextHopTable->hget(key, "nexthop", value));
    EXPECT_EQ(value, "10.0.0.2");
    EXPECT_TRUE(assist.m_resolved_raw_buffer.empty());
    sync.onWarmStartEnd(stateDb);
    ASSERT_TRUE(m_nextHopTable->hget(key, "nexthop", value));
    EXPECT_EQ(value, "10.0.0.2");
    auto up = resolvedMessage(100, 100);
    sync.onMsgRaw(&up.n);
    ASSERT_TRUE(m_nextHopTable->hget(key, "nexthop", value));
    EXPECT_EQ(value, "10.0.0.1,10.0.0.2");
    sync.onMsgRaw(&down.n);
    ASSERT_TRUE(m_nextHopTable->hget(key, "nexthop", value));
    EXPECT_EQ(value, "10.0.0.2");
}
```

`resetMockWarmStartHelper()` 的声明实际放 namespace 外或既有测试声明头，避免上述使用全局限定名时链接到错误 namespace。该例含真实 6000 解析/backwalk，但没有 route 输入，不能计为全部 C07/C08 覆盖。

- [ ] **Step 3:** 在 `test_routesync.cpp` 复用本 TU `createNhgFibMessage/create_route` 与 3000 构造；给普通、封装、SRv6 各增加 suppression 开/关，以及双栈/VRF 正向输入。普通路由注册并在结束时清理 NetDispatcher handler，避免悬空全局指针污染其他用例。
- [ ] **Step 4:** fake refresh map 是 TU 内 static，增加全局测试侧 `std::unordered_map<std::string, swss::KeyOpFieldsValuesTuple> snapshotMockWarmRefreshMap()` 返回副本，并在既有测试声明头声明。测普通路由进入 refresh map、raw 路由正确写表/引用及 NEW/DEL 最终语义；不 extern static 变量、不把 fake 的空 reconcile 当真 helper 验证。
- [ ] **Step 5:** 运行上述 6000 单例、路由新组合和完整 `tests_fpmsyncd`。真实 helper/主循环的定时器和误删检查由 B07/B09 提供，A 的直接调用不能代替它。

## Task 9（A09）：有界异常输入与接口一致性

**Files:**
- Modify/Test: `tests/mock_tests/fpmsyncd/test_routesync.cpp`
- Modify: `tests/mock_tests/fpmsyncd/ut_helpers_fpmsyncd.h`、`.cpp`
- Modify: `tests/mock_tests/Makefile.am`

- [ ] **Step 1:** 在现有 response fixture 加入两条独立契约测试；保持正常 parser 和 warm parser 的输入字节一致，不跨 TU 调用 static builder：

```cpp
TEST_F(FpmSyncdResponseTest, WarmNhgRejectsMalformedJson)
{
    alignas(nlmsghdr) unsigned char buffer[NLMSG_SPACE(MAX_PAYLOAD)]{};
    auto *header = createNhgFibMessage(buffer, sizeof(buffer), RTM_NEWNHGFIB, 124, "{invalid");
    ASSERT_NE(header, nullptr);
    auto &assist = m_mockRouteSync.getNhgWarmAssist();
    assist.initWarmStart();
    assist.bufferNHG(header);
    EXPECT_NO_THROW(assist.reconcileNHGs());
    EXPECT_TRUE(m_mockRouteSync.getNHGMgr().getRIBNHGTable()->getNhgMap().empty());
}

TEST_F(FpmSyncdResponseTest, WarmNhgRejectsMismatchedIds)
{
    auto nhg = createSingleIPv4NextHopNHGFull("192.0.2.3", "0.0.0.0", 125);
    nhg.ifindex = 0;
    const auto json = nlohmann::ordered_json(nhg).dump();
    alignas(nlmsghdr) unsigned char buffer[NLMSG_SPACE(MAX_PAYLOAD)]{};
    auto *header = createNhgFibMessage(buffer, sizeof(buffer), RTM_NEWNHGFIB, 126, json.c_str());
    ASSERT_NE(header, nullptr);
    auto &assist = m_mockRouteSync.getNhgWarmAssist();
    assist.initWarmStart();
    assist.bufferNHG(header);
    EXPECT_NO_THROW(assist.reconcileNHGs());
    EXPECT_EQ(m_mockRouteSync.getNHGMgr().getRIBNHGEntryByRIBID(125), nullptr);
    EXPECT_EQ(m_mockRouteSync.getNHGMgr().getRIBNHGEntryByRIBID(126), nullptr);
}
```

- [ ] **Step 2:** 两例分别运行保存 RED/GREEN。随后新增短 nlmsghdr、短 nhmsg、短属性/越界长度、缺 NUL、未知接口、依赖缺失/环独立子例；每个输入实际分配长度与宣称长度分别记录，构造器不得先读写越界。
- [ ] **Step 3:** 危险字节先只用隔离组件进程/已批准 ASAN 构建执行，设置有限超时和单例过滤。预期是安全拒绝且无错误对象；进程崩溃/ASAN 报错是 FAIL，不能用 EXPECT_DEATH 把产品崩溃变成通过。异常造包不发给 DVS 或设备。
- [ ] **Step 4:** 现有链接仅 wrap `rtnl_link_i2name`，新增测试链接 `-Wl,--wrap=if_indextoname` 和受控 `__wrap_if_indextoname(unsigned int, char*)`。默认委托 `__real_if_indextoname`；仅这些用例启用固定映射/ENOENT，结束复位。增加“有效接口名/未知接口/JSON 与属性 ID 不同”在两个 parser 的独立断言，不能用 ifindex=0 代替接口覆盖。
- [ ] **Step 5:** wrapper 自测及 parser 单例完成后回归整个 fpmsyncd；安全错误优先独立修复审批。没有 sanitizer 或隔离条件时相关验证 BLOCKED，不以普通编译代替内存安全证据。

## Task 10（A10）：重复初始化与重复结束

**Files:**
- Modify/Test: `tests/mock_tests/fpmsyncd/nhgWarmRestartAssist_ut.cpp`

- [ ] **Step 1:** 新增直接揭示旧缓存跨 init 的短例；原始短头只用于缓存观察，禁止回放解析。

```cpp
TEST_F(FpmSyncdNhgWarmAssist, ReinitDropsPreviousBuffers)
{
    m_assist->initWarmStart();
    auto nhg = makeRawNlMsg(RTM_NEWNHGFIB);
    auto route = makeRawNlMsg(RTM_NEWROUTE);
    auto event = makeRawNlMsg(6000);
    m_assist->bufferNHG(rawHeader(nhg));
    m_assist->bufferRoute(rawHeader(route));
    m_assist->bufferResolvedEvent(rawHeader(event));
    ASSERT_EQ(m_assist->m_nhg_raw_buffer.size(), 1u);
    ASSERT_EQ(m_assist->m_route_raw_buffer.size(), 1u);
    ASSERT_EQ(m_assist->m_resolved_raw_buffer.size(), 1u);
    m_assist->initWarmStart();
    EXPECT_TRUE(m_assist->m_nhg_raw_buffer.empty());
    EXPECT_TRUE(m_assist->m_route_raw_buffer.empty());
    EXPECT_TRUE(m_assist->m_resolved_raw_buffer.empty());
}
```

- [ ] **Step 2:** 单例运行并记录首次结果；同一个 Assist 保留至第二次 init，不重建 fixture 掩盖残留。
- [ ] **Step 3:** 对完整合法图连续调用两次 onWarmStartEnd，A02 日志核对第二次没有重复应用路由/resolve 或无必要删建；不只看最后快照相等。
- [ ] **Step 4:** 同一实例开始第二轮，输入不同 NHG/route/6000，断言没有上一轮字节/状态泄漏；真实连接计时与进程实例语义在 B10 另测，组件不能证明 accept/read/select 行为。
- [ ] **Step 5:** 运行重复/init/原始缓存相关全部单例，保留产品 FAIL 和独立修复边界。

## Task 11（A11）：清单、全量回归与交付检查

**Files:**
- Test: `tests/mock_tests/fpmsyncd/nhgWarmRestartAssist_ut.cpp`
- Test: `tests/mock_tests/fpmsyncd/test_routesync.cpp`
- Test: `tests/mock_tests/Makefile.am` 的全部现有目标。

- [ ] **Step 1:** 重新列出测试，把每个 C02–C10 子例对应实际 GTest ID；新 shared suite 不在原 `FpmSyncdNhgWarmAssist.*` 过滤内，必须纳入显式清单。
- [ ] **Step 2:** 按 A01 重新构建真实 daemon 和 tests_fpmsyncd；测试侧 wrapper 不应影响生产链接。保持所有既有告警门槛。
- [ ] **Step 3:** 在 mock 目录运行下列命令；命令分组各自记录退出码，遇失败停止该组，但不丢失已获得的结果。

```bash
set -euo pipefail
./tests_fpmsyncd --gtest_filter='FpmSyncdNhgWarmAssist.*:FpmSyncdWarmShared.*' --gtest_output=xml:nhg-expanded.xml
./tests_fpmsyncd --gtest_output=xml:fpmsyncd-expanded.xml
make check
```

- [ ] **Step 4:** 原 15 例、新例、接口 wrapper、全量 mock 回归和隔离边界检查分别记录状态；fake helper 不证明真实 reconcile，库/编译问题与产品 FAIL 不混淆。未修复的新增回归不能被 skip/xfail 后宣布 G1。
- [ ] **Step 5:** 复核限定测试文件 diff 和日志，更新主计划覆盖表的运行记录；只有用户明确授权才提交，不推送。新发现的生产修复进入独立计划与审批，相关任务在真实回归通过前保持未完成。
