# SONiC SWSS DVS Warm Restart Implementation Plan

> **For agentic workers:** Use /alinos.subagent-dev (recommended) or /alinos.executing-plans to implement this plan task-by-task.

**Goal:** 用确定性合法 FPM 输入验证真实 fpmsyncd 的缓冲、定时器、EOIU、停机和重连路径。
**Architecture:** 在专用 Linux runner 上复用可丢弃 DVS，测试端与 daemon 位于同一网络命名空间并替代 Zebra；保留真实 helper 与 Redis。标准库 peer/observer 是测试辅助，外置 GDB/MI 只证明接收因果，无调试器的新运行才承担期限和性能判定。
**Tech Stack:** Python 标准库/unittest/pytest、DVS、Docker/nsenter、swsscommon/DVSDatabase、FPM v1/netlink、GDB/MI。

## 文件图与边界

- Modify: `tests/test_warm_reboot.py:2478-2493` 附近追加独立 `TestFpmsyncdL2` 和函数级夹具，不改变原 smoke，不解除 `:1123` 的旧跳过用例。
- Create: `tests/dvslib/dvs_fpm_peer.py`，合法帧、消息构造、TCP 收发及命令行子进程入口。
- Create: `tests/dvslib/dvs_warm_observer.py`，epoch、接收证据判定、窄范围 MI 控制器。
- Create: `tests/dvslib/test_warm_helpers.py`，不依赖 DVS 的标准库单测。
- Create: `tests/dvslib/warm_restart_cases.json`，经当前 nexthopgroup 库 round-trip 验证的合法消息、显式独立语义预期和参数集合。
- Reuse: C01 的 `tests/dvslib/nhg_warm_restart_result.py`，不再实现第二套汇总/区间函数。B02–B05 可先开发，不依赖设备实参。
- Read only: `tests/conftest.py:1898-2002`、`tests/dvslib/dvs_database.py:16-30`、`fpmsyncd/fpmlink.cpp:134-323`、`fpmsyncd/routesync.cpp:3983-4008`。

新测试辅助接口由本计划定义，不声称仓库已有；每个接口须先通过独立短序列自测，再用于产品断言。不要新增生产 ACK、控制端口、调试 hook、schema 或依赖包。

## Task 1（B01）：固定专用执行环境及能力前检

**Files:**
- Modify: `tests/test_warm_reboot.py`，新增夹具前检。
- Test: `tests/dvslib/test_warm_helpers.py`，纯前检测试。

- [ ] **Step 1:** 列明所选 Linux runner 的独占授权、Docker/内核/架构、批准的 DVS 镜像摘要、实际 daemon 与 nexthopgroup/swsscommon 版本。DVS 构造会修改宿主 Redis socket 链接、创建辅助容器、使用 team 模块；只独占一个容器不等于宿主隔离，不在普通共享开发机上直接执行。
- [ ] **Step 2:** 在请求 `manage_dvs` 之前校验不使用 `--dvsname`、不使用 `--keeptb`，且启用 `--force-recreate-dvs`；不满足则记录 BLOCKED 并跳过。不要把 `manage_dvs` 作为夹具形参后才提前跳过：其 session teardown 假设实例已创建。
- [ ] **Step 3:** 仅前检通过后调用 `request.getfixturevalue("manage_dvs")`，再以带 run_id 的独立日志路径调用工厂；同一用例只调用一次，不同时依赖另一个 `dvs/testlog` 生命周期。源码 `update_dvs()` 每次调用都可能重建，而非自动逐测试重建。
- [ ] **Step 4:** 检查 Supervisor 项、daemon PID、网络命名空间、NHGFIB/6000/SRv6/PIC、DB schema、符号/ABI和合法 ptrace 能力。DVS 不保证有独立名为 bgp 的容器，不按容器名猜命名空间；缺能力按具体子例 BLOCKED，不自动改权限、镜像或安全设置。
- [ ] **Step 5:** 记录前检结果；纯辅助测试在普通环境可运行，真实 DVS 在环境未齐时保持 NOT_RUN/BLOCKED，不把前检通过当产品通过。

## Task 2（B02）：合法 FPM 帧与流分片

**Files:**
- Create: `tests/dvslib/dvs_fpm_peer.py`
- Create/Test: `tests/dvslib/test_warm_helpers.py`

- [ ] **Step 1: 写 RED 测试。** 以下代码只测试 framing，NOOP 不是转发种子。

```python
import struct
import unittest

from dvslib.dvs_fpm_peer import encode_fpm, FpmDecoder


def noop(body=b"", seq=1):
    return struct.pack("=IHHII", 16 + len(body), 1, 0, seq, 0) + body


class FpmFramingTests(unittest.TestCase):
    def test_every_split_and_coalescing(self):
        messages = [noop(b"x", 1), noop(b"yz", 2)]
        wire = b"".join(encode_fpm(message) for message in messages)
        expected = [message + bytes((-len(message)) % 4) for message in messages]
        self.assertEqual(wire[:4], b"\x01\x01\x00\x18")
        for split in range(len(wire) + 1):
            decoder = FpmDecoder()
            self.assertEqual(decoder.feed(wire[:split]) + decoder.feed(wire[split:]), expected)
            decoder.eof()

    def test_size_and_incomplete_frame(self):
        self.assertEqual(len(encode_fpm(noop(bytes(16364)))), 16384)
        with self.assertRaises(ValueError):
            encode_fpm(noop(bytes(16365)))
        decoder = FpmDecoder()
        decoder.feed(encode_fpm(noop())[:-1])
        with self.assertRaises(ValueError):
            decoder.eof()
```

- [ ] **Step 2:** 运行 `PYTHONPATH=tests python3 -m unittest discover -s tests/dvslib -p test_warm_helpers.py -v`，确认缺模块是预期 RED。
- [ ] **Step 3: 实现最小编解码。** netlink 整数字节序按目标 Linux ABI，正向运行前要求 ABI 一致；FPM 总长始终网络序。

```python
import struct


def encode_fpm(message):
    if len(message) < 16 or struct.unpack_from("=I", message)[0] != len(message):
        raise ValueError("incomplete netlink message")
    total = (4 + len(message) + 3) & ~3
    if total > 16384:
        raise ValueError("oversized FPM frame")
    return struct.pack("!BBH", 1, 1, total) + message + bytes(total - 4 - len(message))


class FpmDecoder:
    def __init__(self):
        self.pending = bytearray()

    def feed(self, chunk):
        self.pending.extend(chunk)
        output = []
        while len(self.pending) >= 4:
            version, kind, total = struct.unpack_from("!BBH", self.pending)
            if (version, kind) != (1, 1) or not 20 <= total <= 16384 or total % 4:
                raise ValueError("outside positive FPM contract")
            if len(self.pending) < total:
                break
            output.append(bytes(self.pending[4:total]))
            del self.pending[:total]
        return output

    def eof(self):
        if self.pending:
            raise ValueError("truncated FPM frame")
```

- [ ] **Step 4:** 重跑并补每次一个字节输入、非法版本/type、非法对齐和最大长测试；异常字节只进入纯 helper 单测，不发送到真实 DVS。
- [ ] **Step 5:** 核对 framing 行为与 `fpmsyncd/fpm/fpm.h:90-143`，保留测试数/退出码，按授权提交规则进入检查点。

## Task 3（B03）：库兼容消息与独立语义数据

**Files:**
- Modify: `tests/dvslib/dvs_fpm_peer.py`
- Create: `tests/dvslib/warm_restart_cases.json`
- Test: `tests/dvslib/test_warm_helpers.py`

- [ ] **Step 1:** 从 A02 的真实 `NextHopGroupFull` 构造/JSON round-trip 获取普通双栈、ECMP、多级、received SRv6 共享及组合 PIC 的合法表示。数据文件每例包含 `id`、`purpose`、`seed_events`、`seed_expected`、`restore_events`、`expected`、独立路由/成员/PIC语义、需替换的接口名与 ifindex 字段路径；不要从恢复后的 APP_DB 生成预期。
- [ ] **Step 2:** 写帧属性断言，确认 5000/5001 使用 nhmsg，6000 使用 rtmsg，NHA_ID 为属性 1 的 U32，JSON 为属性 2 的 NUL 结尾字符串；同时核对 netlink 长度、四字节属性对齐与 ID 一致。新增 `nhg_message(kind, family, rib_id, body, seq)` 后以下断言应成立：

```python
from dvslib.dvs_fpm_peer import nhg_message


class PrivateMessageTests(unittest.TestCase):
    def test_nhg_envelope(self):
        message = nhg_message(5000, 2, 101, {"id": 101}, 7)
        length, kind, flags, seq, pid = struct.unpack_from("=IHHII", message)
        self.assertEqual((length, kind, flags, seq, pid), (len(message), 5000, 0, 7, 0))
        attr_len, attr_type, rib_id = struct.unpack_from("=HHI", message, 24)
        self.assertEqual((attr_len, attr_type, rib_id), (8, 1, 101))
```

这个最小 JSON 仅验证封装，不是可发往 daemon 的完整 NHG。

- [ ] **Step 3:** 实现确定性属性封装，完整业务数据经 A02 库 round-trip 后才开放发送。

```python
import json


def attribute(kind, payload):
    length = 4 + len(payload)
    return struct.pack("=HH", length, kind) + payload + bytes((-length) % 4)


def nhg_message(kind, family, rib_id, body, seq):
    if kind not in (5000, 5001) or family not in (2, 10) or body["id"] != rib_id:
        raise ValueError("invalid NHGFIB envelope")
    payload = struct.pack("=BBBBI", family, 0, 0, 0, 0)
    payload += attribute(1, struct.pack("=I", rib_id))
    payload += attribute(2, json.dumps(body, separators=(",", ":")).encode() + b"\0")
    return struct.pack("=IHHII", 16 + len(payload), kind, 0, seq, 0) + payload
```

- [ ] **Step 4:** 为 6000 按 `fpmsyncd/routesync.cpp:2398-2448` 构造 rtmsg 和属性 2；JSON 必含 `rnh_prefix`、`prev_resolved_prefix`、`prev_resolved_nhg_id`、`curr_resolved_prefix`、`curr_resolved_nhg_id`，按源码使用对应前缀和 ID 类型。普通/封装/SRv6 路由及 1000/1001/2000/2001 使用 A08 的合法消息样本和实际库字段，不用 NOOP 代替；写逐字段 round-trip 断言并在冷进程验证所需状态。
- [ ] **Step 5:** 重跑全部纯测试，确认只允许在明确列出的字段路径替换实际 ifindex/接口，不全局字符串替换。未经库/冷进程验证的样本不得用于报告后续 warm 恢复中的 daemon 缺陷；若合法输入、环境和夹具已核验，冷预热本身暴露的产品错误仍记 FAIL，依赖该前置的后续 warm 场景记 BLOCKED，不能归因到 warm 路径。

## Task 4（B04）：同命名空间 TCP peer

**Files:**
- Modify: `tests/dvslib/dvs_fpm_peer.py`
- Test: `tests/dvslib/test_warm_helpers.py`

- [ ] **Step 1:** 定义窄命令行协议：stdin 每行 JSON 命令 `connect`、`send`、`disconnect`、`close`，`send` 带已校验帧的十六进制和指定分片长度；stdout 每行 JSON 记录命令号、连接号、monotonic 时间与 `sent/received/error`。发送回执名称只能是 `sent`，不能是 `consumed`。
- [ ] **Step 2:** 用标准库本地 socket server 的测试验证：两帧合发、按每个字节分发、server 持续返回数据时仍可发送、对端关闭产生有界错误、close 后子进程退出。例子的核心断言是接收端重组字节完全等于 `b"".join(frames)`，而非只核对 send 返回值；测试 server 与 peer 都由测试创建并在 finally 中关闭。
- [ ] **Step 3:** 使用 `selectors` 同时处理 stdin 和 socket，socket 读返回 0 记录 disconnect，不循环忙等；持续排空返回帧。每条命令有预先给定的有限截止点，单次 send/read 不得无限阻塞；错误日志不含凭据。
- [ ] **Step 4:** 宿主以参数数组 `nsenter -t <dvs.pid> -n -- <python3> <peer绝对路径>` 启动该子进程，先核对目标 netns 与真实 daemon 相同；既有 `127.0.0.1:2620` 不新增监听。连接成功由 server accept 观察定 epoch，不用 peer connect 回执代替。
- [ ] **Step 5:** 每次重连命令发送 B03 定义的完整状态列表；只关闭自己创建的 peer，核对原始返回帧不会被当通用 ACK。正常/异常关闭测试均通过后进入提交检查点。

## Task 5（B05）：epoch、真实入队与外置观察

**Files:**
- Create: `tests/dvslib/dvs_warm_observer.py`
- Test: `tests/dvslib/test_warm_helpers.py`

- [ ] **Step 1:** 写 epoch 和入队判据测试。`onFpmConnected()` 早于 accept，不能给它加一当成功连接；只在 accept 返回非负 fd 后调用下面的方法。

```python
from dvslib.dvs_warm_observer import Epochs, buffered_before_end


class ObservationTests(unittest.TestCase):
    def test_process_identity_and_real_append(self):
        tracker = Epochs("test-run")
        first = tracker.accept_return(71, 1000)
        second = tracker.accept_return(71, 1000)
        reused = tracker.accept_return(71, 2000)
        self.assertEqual((first[2], second[2], reused[2]), (1, 2, 1))
        event = dict(epoch=first, type=6000, fsm_before=2, fsm_after=2,
                     before=(0, 0, 0), after=(0, 0, 1),
                     input_digest="fixture", appended_digest="fixture",
                     entry=(10, 11), returned=(12, 13))
        self.assertTrue(buffered_before_end(event, first, (20, 21)))
        self.assertFalse(buffered_before_end(dict(event, after=(0, 0, 0)), first, (20, 21)))
        self.assertFalse(buffered_before_end(event, second, (20, 21)))
        self.assertFalse(buffered_before_end(dict(event, type=2000), first, (20, 21)))
```

- [ ] **Step 2:** 运行纯辅助单测取得 RED，然后实现以下小函数；实际 observer 用 SHA256 原始消息摘要，测试字符串只作合成标签。

```python
class Epochs:
    def __init__(self, run_id):
        self.run_id = run_id
        self.counts = {}

    def accept_return(self, pid, start_ticks):
        process = (pid, start_ticks)
        self.counts[process] = self.counts.get(process, 0) + 1
        return self.run_id, process, self.counts[process]


def buffered_before_end(event, epoch, cutoff):
    queue = {5000: 0, 5001: 0, 24: 1, 25: 1, 3000: 1, 3001: 1, 6000: 2}.get(event["type"])
    if queue is None or len(event["before"]) != 3 or len(event["after"]) != 3:
        return False
    delta = tuple(after - before for before, after in zip(event["before"], event["after"]))
    return (event["epoch"] == epoch and event["fsm_before"] in (1, 2, 3)
            and event["fsm_after"] == event["fsm_before"]
            and delta == tuple(int(index == queue) for index in range(3))
            and event["input_digest"] == event["appended_digest"]
            and event["entry"][1] <= event["returned"][0]
            and event["returned"][1] < cutoff[0])
```

- [ ] **Step 3:** 实现标准库 Popen 的 MI 控制：`docker exec -i` 在专用 DVS 内启动 `gdb --nx --nh --quiet --interpreter=mi2 --pid <真实容器内PID>`，附着 Supervisor 子进程，不另起 daemon。使用 token 关联命令结果，保留原始 MI；按线程、栈帧、call_id 关联 entry 和 `-exec-finish` 正常返回，信号/异常/其他断点不得误作 return。对 MI 分片、异步通知、嵌套停止和正常返回分别写固定 transcript 输入的测试。
- [ ] **Step 4:** 观察成功 accept、三个 buffer、实际 raw/普通 handler、`onWarmStartEnd`、load/save/flush 的所需调用。捕获入口 `this`，通过当前 ABI 的只读内存/调试符号读取 FSM、vector 长度及新增项内容，禁止调用 inferior 的 `.size()` 或其他函数。参数优化掉、ABI不符或 ptrace 不允许则 BLOCKED，不改权限或编译告警。信号测试必须把信号传给 inferior。
- [ ] **Step 5:** 短序列自测核对“一次成功 accept、每条各一次追加、提前返回不算追加、结束后不再缓存”。另开 `plain` run 运行 C01 `interval_status()`；observed run 或跨 run/epoch 的数据一律不能用于期限 PASS。调试停顿错过窗口记前提不满足，同时保留已知 FAIL。

## Task 6（B06）：独占夹具、真实冷预热与最小控制接口

**Files:**
- Modify: `tests/test_warm_reboot.py`
- Reuse: peer、observer、DVSDatabase。

- [ ] **Step 1:** 增加函数级 `fpm_l2_dvs(request)`，按 B01 前检再取得 manager。使用 `dvs.ctn.id`、`dvs.pid`、`dvs.redis_sock`；`runcmd(argv)` 返回 `(exitcode, output)` 必须检查，不直接依赖其无限执行来实现 deadline。
- [ ] **Step 2:** 建立只用于本组测试的 `FpmL2Run` 控制对象，明确以下新接口，并为每个方法保留事件记录：

| 新接口 | 具体行为与后置条件 |
|---|---|
| `seed(case_id)` | 加载 B03 完整合法样本，配置样本声明的实际接口与邻接；Supervisor 运行真实冷进程，peer 发送 seed_events，按 seed_expected 等待 APP/相关下游表并返回完整样本；恢复后另用 expected，不写 APP_DB 冒充种子、不补恢复计数。 |
| `restart(mode, route_table_empty=False)` | 按用例申请 warm 状态并对真实进程进行受控停止/启动；空表场景通过合法撤路由形成；新进程开始前/后记录 PID+启动时刻，observer 在首次连接前装好。 |
| `send(events)` | 仅把有序合法帧交给 peer，返回发送记录；消费证据必须另外取 observer。 |
| `wait_received(event_ids)` | 每条消息的 entry/return、状态机、长度增量和摘要满足 B05，且早于结束入口；不能通过关闭 TCP 或固定 sleep 实现。 |
| `set_eoiu(families)` | 独占写入运行 schema 中的 BGP EOIU 状态，记录写前/写后与时间；只在对应接收截止证据齐全后使用。 |
| `wait_restored()` | 在真实 monotonic 有限期限内等待逻辑完成及必要 flush 后状态可见，返回本 run 证据，不把 STATE_DB 一个字段作为成功。 |
| `assert_semantics(expected)` | 比较独立路由/NHG/PIC 关联元组及引用；不调用生产 setEntry，不独立排序关联字段。 |
| `close()` | 先保留证据，再关闭自己的 peer/observer、恢复改变的 Supervisor 状态；DVS 销毁交给 manager。 |

- [ ] **Step 3:** 初始化时保存 Supervisor 状态，`supervisorctl stop zebra` 后核对 STOPPED 与进程消失，确认 EOIU 写者独占；停止 fpmsyncd 后以 `update_entry()` 更新 `DEVICE_METADATA/localhost/nhg_fib`，不使用会删除其他字段的 `set_entry()`。所有修改只作用于本例创建的 DVS。
- [ ] **Step 4:** APP_STATE_DB 无现成 getter，使用 `DVSDatabase(swsscommon.APPL_STATE_DB, dvs.redis_sock)`；其他库复用 `get_app_db/get_config_db/get_state_db/get_asic_db`。使用实际 separator，不硬编码 DB 号。实现 elapsed time 约束，现有 `wait_for_result()` 的重试次数不包含查询耗时，不能当精确 deadline。
- [ ] **Step 5:** 先执行一例冷合法 NHG+路由和结束后的正常更新，确认工具闭环；单独记录预热失败、工具错误和产品失败。fixture 初始化中途失败也须进入 finally 清理自己的子进程，不能破坏共享 Redis。

## Task 7（B07）：空表与 EOIU 的真实结束路径

**Files:**
- Modify/Test: `tests/test_warm_reboot.py` 的 `TestFpmsyncdL2`。

- [ ] **Step 1:** 新增以下参数测试；`two_hop_v4` 是 B03 必建样本 ID，包含两个不同下一跳和可匹配 ECMP 对象，`expected` 在样本中显式列出。

```python
@pytest.mark.parametrize("old_nhg", [False, True])
def test_empty_route_timer(self, fpm_l2_dvs, old_nhg):
    run = fpm_l2_dvs
    case = run.seed("two_hop_v4" if old_nhg else "empty")
    run.restart("observe", route_table_empty=True)
    sent = run.send(case["restore_events"])
    run.wait_received([record["event_id"] for record in sent])
    evidence = run.wait_restored()
    assert evidence["timer_started"]
    assert evidence["trigger"] == "main_timeout"
    assert evidence["route_helper_was_reconciled"]
    run.assert_semantics(case["expected"])
```

`empty` 样本仍有明确的恢复输入变体，包含 NHG、route、6000 的合法序列；不能用无输入的空成功替代有/无旧 NHG 的两条路径。

- [ ] **Step 2:** 在所选专用 Linux runner 上运行此单例，真实路径不成立时保留 RED/FAIL；不直接调用 `onWarmStartEnd()` 人工触发。
- [ ] **Step 3:** 增加非空表参数 `families=(ipv4,ipv6)`、仅 ipv4、仅 ipv6、空集合。双栈到齐走 hold 或实际先到主超时，缺一族不能因单族 EOIU 提前完成；期限、hold 和主 timer 配置预先冻结。
- [ ] **Step 4:** 在接收证据齐后再写 EOIU，记录两个 timer 的实际顺序；结束函数可能被调用多次，断言是消息和对象不重复应用，不是强行要求函数只调用一次。
- [ ] **Step 5:** 每例另外执行无调试同提交运行核对有限完成和最终状态；缺接收观察的 plain run 不声称输入精确落在缓冲窗口。相关生产失败进入独立修复，不修改辅助断言使其通过。

## Task 8（B08）：交错 NHG、路由与 6000

**Files:**
- Modify/Test: `tests/test_warm_reboot.py`
- Modify: `tests/dvslib/warm_restart_cases.json`

- [ ] **Step 1:** 增加固定事件账本：NHG NEW→DEL、DEL→NEW、两次 NEW 不同权重；路由先到/后到；6000 先于/晚于 NHG 和路由，以及撤销→恢复→再次撤销。每条记录带 seq/event_id、完整载荷与独立最终预期，不按当前三队列实现重排预期。
- [ ] **Step 2:** 为每个序列调用 `seed()`、`restart("observe")`、`send()`、`wait_received()`、`wait_restored()`、`assert_semantics()`；消息截止必须早于结束入口。首次运行保留原始失败，已知忽略 DELETE 不能写成预期。
- [ ] **Step 3:** `assert_semantics()` 必须核对 PIC fix-up/backwalk 和引用，不只确认 6000 handler 被调用；图的构建依赖 A06 的真实 shared/PIC 样本，不用两条路由引用同一 RIB 代替共享表路径。
- [ ] **Step 4:** 完成后不重启进程，继续发送实时 resolve 撤销/恢复和一次普通增删，确认不再入队且最终状态再次正确。
- [ ] **Step 5:** 对所有适用顺序重跑单例，按表/键/阶段记录必要与不必要写事件；未具备完整操作事件流时“无删除重建”子断言 BLOCKED，不用前后快照补证。

## Task 9（B09）：路由分发、VRF 与 suppression

**Files:**
- Modify/Test: `tests/test_warm_reboot.py`
- Modify: `tests/dvslib/warm_restart_cases.json`

- [ ] **Step 1:** 将普通、带封装、SRv6 VPN 三类分别与 suppression 开/关组成六个必测基例；再给协议支持的 IPv4/IPv6、默认/非默认 VRF 各至少一个正向路由。用例 ID 明确各维度，不在失败后删去组合。
- [ ] **Step 2:** 对每个基例预热并恢复，核对 raw/NetDispatcher 实际 handler、路由表与 SRv6/PIC 表的完整字段及引用。回放不再截获，原 helper reconcile 不得误删有效路由；未匹配/过期路由仍应清理。
- [ ] **Step 3:** 混入合法 localSID 1000/1001、PIC context 2000/2001 和链接事件，核对它们走原路径且未增加三类缓存；这些消息可能改变前置依赖，最终语义必须同时核对。
- [ ] **Step 4:** 冷启动、nhg_fib 关闭、恢复结束三个状态分别发送相同合法输入，证明拦截开关正确，不仅测试 standalone `shouldIntercept()` 的类型集合。
- [ ] **Step 5:** 每个实际支持组合保存运行结果和无支持依据；缺 SRv6/PIC 镜像不能解释成全范围 PASS。真实分发失败另立修复，不链接 mock helper 替代真实主循环。

## Task 10（B10）：重连、健康首轮与分阶段退出

**Files:**
- Modify/Test: `tests/test_warm_reboot.py`

- [ ] **Step 1:** 参数区分同进程断连/重连与真实进程重建，记录 `(run_id, pid+start_ticks, accept_ordinal)`；重连重放完整状态，不能把增量输入导致的缺项误报为恢复丢失。
- [ ] **Step 2:** 健康冷预热后的第一次 warm 和之后 warm 都核对真实 checkAndStart、必要保存/加载与最终图；不得沿用旧 smoke 手填计数。另立 `purpose=拒绝/降级安全` 的缺 restore_count 场景，按冻结依赖版本期望安全 cold fallback，该 PASS 不计 warm 成功。
- [ ] **Step 3:** SIGTERM 与 SIGINT 各覆盖完整工作态、首次 accept 阻塞、恢复中部分图、read/select 等待和断连重连；观察点达到后才发送信号。完整态保全恢复所需数据；首次无图不强制空 save；部分图不得覆盖旧完整记录，所有路径须干净有界退出，无未处理 EINTR。
- [ ] **Step 4:** 信号传给 Supervisor 管理的真实进程，观察器不能吞信号；记录是否自动重新拉起，并以目标进程实例退出为断言。后续新进程恢复验证数据保全，而不是只检查 save 调用计数。Supervisor 的实际 stopsignal/stopwaitsecs 与自动重启行为必须先记录。
- [ ] **Step 5:** 同一完整恢复中重复结束触发、同进程 init 后残留、第二轮新消息各有独立断言；当前状态破坏、队列残留或退出失败保留 FAIL 并进入独立修复，不能整体重建 fixture 将其隐藏。

## Task 11（B11）：真实加载的负向状态

**Files:**
- Modify/Test: `tests/test_warm_reboot.py`

- [ ] **Step 1:** 从合法冷预热和保存状态派生六类互不混合的负例：合法空状态、仅损坏保存 JSON、无效 ID、低/非法 allocator、PIC 关联不一致、缺依赖。每例新的 run，记录修改前值与预声明 expected_outcome。
- [ ] **Step 2:** 仅该负例允许直接修改本 DVS 的状态 DB；保存 JSON 不被加载器消费，合法元数据与完整 FPM 输入仍应重建，不要求出现解析错误。FPM 非法 JSON 留在 A09 隔离组件层，不混成同一个用例。
- [ ] **Step 3:** 增加 APP_DB-only 活 ID 和旧保存键残留场景；恢复后再次创建/删除 NHG/PIC，核对两种命名空间各自无碰撞和占用正确释放，不要求全局计数回退。
- [ ] **Step 4:** 对合法非空期望仍缺图的场景，即使 STATE_DB 显示 RECONCILED 也判 FAIL；预期拒绝/降级只按安全负例范围判，不把缺数据接受为健康成功。
- [ ] **Step 5:** 保存受影响键、加载日志和后续操作证据。缺必要观察能力记对应 BLOCKED，但已经观测的覆盖活 ID、错误引用或状态破坏仍保留 FAIL。

## Task 12（B12）：固定表示保持与无调试期限

**Files:**
- Modify/Test: `tests/test_warm_reboot.py`

- [ ] **Step 1:** 固定合法表示、不变完整 FV 的普通及 shared/PIC 样本执行 plain run，按候选集合验证可匹配旧 ID 保持；字段顺序不重要，相关值列表内部顺序可能影响精确匹配。
- [ ] **Step 2:** RIB 改号和成员表示重排单列，仍要求语义正确，记录新 ID/未匹配与写事件；不承诺任意等价序列都保持 ID，也不放宽固定表示断言。
- [ ] **Step 3:** 分别记录逻辑调用结束、必要 flush、期望状态可见；性能运行不附加 GDB、全库 MONITOR 或高频全表采样。最后未就绪/首次就绪形成 T_restore 区间，单次查询也有超时。
- [ ] **Step 4:** 仅本 plain run 的有效区间交给 C01 的 `interval_status()`；跨 run、无预算、调试数据或精度跨越预算不得 PASS。实际已超预算则 FAIL，不能因缺更细观察而抹掉已知错误。
- [ ] **Step 5:** 若无可靠写事件流，保持语义与 ID 结果，单独将“过程中无删除重建”标为 BLOCKED；不复用会去重/事后取值的旧订阅计数器来证明零 churn。

## Task 13（B13）：分层回归与关卡报告

**Files:**
- Test: `tests/dvslib/test_warm_helpers.py`
- Test: `tests/test_warm_reboot.py::TestFpmsyncdL2`

- [ ] **Step 1:** 在普通 Python 环境重跑全部纯 helper 和 C01 判据单测，确认实际运行数非零，无意外跳过；这些结果只代表测试工具。
- [ ] **Step 2:** 在已批准的专用 Linux runner 使用下列入口；DVS_IMAGE 是执行输入，不在计划中虚构镜像。权限由环境预先提供，不自动安装、提权或更改容器安全配置。

```bash
set -euo pipefail
: "${DVS_IMAGE:?必须指定已核对的兼容镜像}"
python3 -m pytest -vv --max_cpu=2 --force-recreate-dvs \
  --imgname="$DVS_IMAGE" \
  tests/test_warm_reboot.py::TestFpmsyncdL2
```

- [ ] **Step 3:** 按 C02–C10/L01–L05 子例清单核对实际 collected/executed IDs 和每项结果；skip 对应 BLOCKED/N/A 必须单独记录，pytest 返回 0 不足以通过 G2。
- [ ] **Step 4:** 在相同固定镜像另建专用 DVS 运行原有 fpmsyncd smoke 及受影响小型 warm 测试，检查夹具退出后服务、peer/observer 和子容器资源；原 skip 大例仍保持跳过。
- [ ] **Step 5:** 汇总有效的因果、状态、期限与缺证据，不宣称物理数据面或真实 Zebra 已验证。仅在用户授权提交时提交已审阅的测试改动，生产失败继续按主计划保持未完成。
