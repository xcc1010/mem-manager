# Step 2 内存池分配器（v1, bump）实现设计文档

> **分支状态**：`br_xcc_bump` @ `abe3356`，领先 `main` 21 个提交（+1779/-169，16 个文件）。
> 本文是该分支**当前实现**的完整设计总结，取代 `STEP2_DESIGN.zh.md` 中已过时的部分
> （flags 默认值 {1}、无 OnPg、无生命周期管理等均为旧版描述）。
> 总览见 `设计文档.md`，架构约束见 `DESIGN.md`。

---

## 1. 背景与目标

业务平台的 OS 共享内存原语（`ShmMap` / `ShmMapOnPg` / `ShmUnmap`）**最小分配粒度为 2MB**，
而业务的实测分配主体集中在 16KB–256KB：~14,467 个不同名的区域、实际申请 ~1.3GB，
OS 实际占用 ~28.8GB，**浪费 ~27.5GB（约 95%）**。

Step 2 的目标：预申请大块共享内存作为池，从池内自管理地切分小块，把 OS 占用压向真实
需求（潜在 ~20 倍下降）。池行为由 JSON 配置文件驱动，可随需求演进调整、支持一键回滚。

**两步走机制**：Step 1（已完成）`Platform_Shm*` 纯透传 + Debug-only profiler 采集真实分配
行为；Step 2（本文）在 `Platform_Shm*` 内部按配置路由到池分配器。

## 2. v1 范围与关键决策

| 决策点 | v1 选择 | 理由 |
|---|---|---|
| 分配策略 | **bump  only**（指针推进、O(1)、槽位不回收） | 业务以 init-once/长生命周期分配为主；slab/freelist 移出 v1 |
| 池化对象 | **VA-differs 为主**，默认可池化 flags = **{0}** | 2026-07 数据修正：业务几乎全是 VA-differs；**api.h 把业务 type-1 #define 为 0U，掩码测的是 flags 值而非类型编号**（43fd749 修正） |
| 元数据位置 | 共享内存 `mmpool/meta`，**只存偏移/索引，绝不存指针** | VA-differs 下裸指针跨进程无意义 |
| 块寻址 | 各进程按块名自行 attach，本地 VA + 偏移 | 同一路径天然兼容 VA-same |
| 并发模型 | 单一共享池 + 跨进程 TrySpinLock 创建锁 + 无锁 attach | 打包最密最省内存；否决了每进程 arena |
| process-shared | 不透池，直接透传 OS | 语义不同 |
| flags 隔离 | **不同 flags 值绝不共用池块**；OnPg 块再按 pgidx 细分 | 共享范围/NUMA 局部性隔离 |
| 配置来源 | **仅 CP 读 JSON 文件**；DP 无文件 I/O，用 meta 快照 | DP 是 C-only 受限工具链（112b499） |

**路由规则**（`poolable()`）：

| 请求 | 处理 |
|---|---|
| `size < threshold` 且 flags 值在掩码内 | **进池**（bump 切槽） |
| `size ≥ threshold` / `size > block_size` | 透传 OS |
| 掩码外 flags | 透传 OS |
| `enable = 0` | 全透传（等价 Step 1，**一键回滚**） |

保留 OS 两个语义不变：**同名共享**（同名 attach 拿同一块）+ **引用计数**（返回值 = refcount）。

## 3. 总体分层与调用路径

```
业务 ──► Platform_ShmMap / Platform_ShmMapOnPg(CP) / Platform_ShmUnmap
            │
            ▼  (MEM_MANAGER_POOL 编译开关)
      mm_pool_try_map / mm_pool_try_map_pg / mm_pool_try_unmap
            │
      ensure_init() ──► 执行上下文看门狗（pgid,vcpu 校验，§7）
            │            └─ 失配：丢弃本地缓存重新 attach
            ▼
      meta_get()（原子读 g_meta_va，§6.1）
            │
     ┌──────┴───────────────┐
  透传 OS                  进池
  (大块/掩码外/降级)          │
                    ┌────────┼─────────────┐
                无锁 attach    创建路径        池块来源
                lookup 命中   (跨进程自旋锁)   ShmMap / ShmMapOnPg
                refcount++    bump 切槽       (64MB 大块,按 flags/pgidx 分)
                             注册表登记
```

`Platform_ShmUnmap` 先问池"这个地址是不是池内槽位"：是则认领（返回成功但**不做任何
回收**——bump 语义），否则透传 OS `ShmUnmap`。

## 4. 数据结构

### 4.1 PoolMeta（共享元数据，固定名 `mmpool/meta`，flags=0）

```c
typedef struct {
    UINT64      ready;                      /* MM_META_MAGIC("MMPOOL05") = 创建者完工屏障 */
    AAASpinLock lock;                       /* 创建路径跨进程自旋锁（平台上 176 字节） */
    int         enable;                     /* ── 配置快照：creator wins ── */
    UINT32      threshold;                  /*    attach 方一律用快照，不读自己的 cfg */
    UINT32      block_size;
    UINT32      poolable_flags_mask;
    INT32       nblocks;                    /* SetRelease 发布 / ReadAcquire 读 */
    INT32       npg;                        /* pgname 表填充数（仅持锁时触碰） */
    char        pgnames[32][32];            /* OnPg NUMA 放置键表 */
    PoolBlock   blocks[1024];               /* 块表：size/next(bump 游标)/flags/pgidx，无指针 */
    NameEntry   names[8192];                /* 名字注册表：开放寻址哈希 */
} PoolMeta;                                 /* _Static_assert ≤ 2MB（一个 OS 区域） */
```

### 4.2 NameEntry（名字 → 槽位）

```c
{ INT32 state;     /* ST_EMPTY → ST_USED，SetRelease 发布 / ReadAcquire 读 */
  INT32 refcount;  /* 创建者置 1，attach 者 AAA_Atomic32IncReturn */
  int block_idx; UINT32 off; UINT32 size; INT32 flags;
  INT32 pgidx;     /* -1 = 普通 ShmMap 槽；>=0 = 经 ShmMapOnPg(pgnames[pgidx]) 创建 */
  char name[64]; }
```

### 4.3 池块命名与寻址

- 块名：`mmpool/blk-<flags>-p<pgidx>-<序号>`（名字是唯一标识；attach 一律用普通
  `ShmMap` 按名 attach，OnPg 创建的块亦然）。
- **共享 meta 只存 (block_idx, off)**；每个进程/上下文用 `g_blk_base[]` 缓存自己 attach
  到的本地 VA：`地址 = g_blk_base[block_idx] + off`。
- 块默认 64MB（`block_size`，2026-07 试用值：尾部搁浅稀释到 ~0.2%，~1.3GB 存活量约 21 个
  OS 段）；平台 OS 段上限 128MB（`MM_MAX_BLOCK_SIZE`）。

### 4.4 进程本地状态

| 状态 | 类型 | 说明 |
|---|---|---|
| `g_meta_va` | INT64（指针宽） | 本地 meta VA，**只经 AAA 64 位原子发布/读取**（§6.1） |
| `g_blk_base[1024]` | INT64 数组 | 各块本地 VA 缓存，按需懒 attach |
| `g_init_state` | INT32 | 0 未初始化 / 1 初始化中 / 2 完成 |
| `g_owner_pid / g_owner_vcpu` | INT32 | 缓存所属的执行上下文（§7） |

## 5. 配置体系（CP-only）

- **默认值**：`enable=1, threshold=0x200000(2MB), block_size=0x4000000(64MB), mask={0}`。
- **JSON 文件**：路径由 `MEM_POOL_CONFIG` 指定——这是**唯一**使用的环境变量，且仅用于
  定位文件（早期版本的 `MEM_POOL_*` 逐项覆盖已删除）。扁平对象，未知键忽略，大小接受
  十进制数字或 `"0x.."` 字符串；手写极简解析器，无第三方依赖。
- **只有 meta 创建者（CP/Simulator）读文件**；所有 attach 方使用 meta 里的快照。
  DP 上 `mm_pool_load_config` 直接返回 -1（无文件 I/O，`<stdio.h>` 在 DP 链接环境会
  与平台头文件的 FILE 定义冲突，故整个 JSON 解析以 `#ifdef IS_CP` 包裹）。
- **回滚**：文件里 `"enable": false`，全量透传。

```json
{ "enable": true, "threshold": "0x200000", "block_size": "0x4000000",
  "poolable_flags": [0] }
```

## 6. 并发与正确性设计

### 6.1 初始化：进程内 CAS 门闩 + 跨进程 ready 屏障

```
mm_pool_init(cfg):
  g_init_state == 2 ? 返回
  CAS(g_init_state, 0→1) 成功?  → 唯一胜者进入 meta_attach()（全进程只发一次 meta 的 ShmMap）
                                  记录 owner (pgid,vcpu)，SetRelease(g_init_state, 2)
  失败者：有界等待 state==2，超时则返回（降级透传，绝不重入 meta_attach）

meta_attach():
  ShmMap("mmpool/meta") → ret 是 OS 引用计数
  ret == 1（创建者）: memset → 读配置(CP 读 JSON) → 写字段 → AAA_InitSpinLock
                     → SetRelease(ready, MAGIC)     【跨进程 ready 屏障】
  ret >  1（attach者）: 有界自旋等 ready==MAGIC（超时→当作无 meta，降级透传）
  最后：meta 快照 sanity 校验（§9）→ meta_put(m) 原子发布
```

两道已修复的 coredump 防线（详见 §10）：

1. **进程内并发 init 双创建者**：原实现让所有并发调用者都进 `meta_attach`，依赖 OS 对
   进程内并发同名 create 的串行化；假设不成立时两个线程同进创建者分支，后者的
   `memset`/`AAA_InitSpinLock` 会清零正在使用中的元数据与锁 → coredump。**CAS 门闩
   保证全进程只有一个线程进入 meta_attach，不再依赖该假设**（308d2b8）。
2. **`g_meta` 普通全局指针的跨线程发布**是 C11 data-race UB，DP 工具链优化器借此把
   NULL 检查重排到解引用之后（实测 DP 启动期崩在 `ldr w0,[x20,#192]` 即 `m->enable`，
   x20=g_meta 未发布）。**g_meta 改为指针宽 INT64 + AAA 64 位原子的不透明访问器
   发布/读取，NULL 检查独立成句**（5bb92bf）。

### 6.2 attach 热路径：无锁

`lookup()`（开放寻址哈希，`state` 原子读）命中 → `entry_ok()` 校验（flags 相同、请求
size ≤ 注册槽 size）→ `local_base()` 查/建本地 VA → `refcount` 原子自增 → 返回
`base + off`。**全程无锁**；表项一经发布永不墓碑化（bump 语义），探针遇首个 ST_EMPTY
即停。

### 6.3 创建路径：跨进程自旋锁 + 有界自旋复查

- 未命中 → 最多 128 轮（`MM_CREATE_ROUNDS`）{ TrySpinLock（每轮 20000 次尝试）→
  锁内复查注册表 }。
- **新池块的 OS ShmMap/ShmMapOnPg 刻意放在锁内**：移出锁外会打开同名重复注册的窗口。
- **split-sharing 禁令**：只要同名创建可能仍在飞行，可池化名字绝不透传 OS——否则同一
  名字会同时以池槽和 OS 私有区域存在（静默数据分裂）。只有预算耗尽（锁被死亡持锁者
  冻结 / 池或表满）才允许透传，此时所有人同样失败，名字一致地走 OS 共享。
- 冲突（同名不同 flags / 请求大于注册槽）**响亮失败**：`ret = -1, *out = NULL`，不静默
  截断。
- 持锁者崩溃 → 锁留在持久 meta 段里跨重启保持置位 → 池"冻结"，全部一致性透传，并打
  日志使其可见（由 boot cleanup 解除，§8）。

### 6.4 生命周期：显式 init/uninit + boot 清理

平台要求的时序（README §Pool configuration）：

```
Simulator(CP) 启动:
    mm_pool_cleanup()     ← 排空上次运行残留的 meta+池块（refcount-drain 兜底）
    mm_pool_init(cfg)     ← 单点预建 meta（在任何 loadPG 之前）
    loadPG(...)

每个 PG/DP 进程:
    入口: mm_pool_init(NULL)     ← 只 attach（若意外成为创建者 → 打 WARN，
    ...                            说明启动时序被违反，仍安全兜底初始化）
    退出: mm_pool_uninit()       ← ShmUnmap 本进程 attach 的所有块 + meta
```

- **`mm_pool_uninit`**：给池段真正的生命周期。此前所有 attach 只增不减 OS refcount，
  段与注册表永生，破坏下次启动的 "ret==1 ⇒ 我是创建者" 协议并残留旧锁/旧配置。
  全系统停止时 refcount 归零、OS 回收段，下次启动是干净注册表。
- **`mm_pool_cleanup`**（兜底，仅 Simulator 在无任何池用户时调用）：进程崩溃、多 vcpu
  attach 扰动使 refcount 计数不可靠，不能依赖 uninit 完美配对。cleanup 只用
  ShmMap/ShmUnmap 实现"drain"：attach 得知当前 refcount，再 unmap 同样次数降到零。
  若残留 meta 带本版 magic，先按其块表精确 drain 所有记录的池块，再 drain meta 本身。
- 懒初始化路径（try_map/try_unmap 里的 ensure_init）保留为安全网。

## 7. 执行上下文看门狗（平台特性，关键）

**平台实测确认（A/B 实验）：shm VA 的作用域是 per-vcpu 的**——在一个 vcpu 上 attach 的
meta，另一个 vcpu 解引用其缓存 VA 会 fault（pgid-only 看门狗曾因此 coredump，
(pgid,vcpu) 看门狗则不崩）。外加风险 S1：平台在池初始化后 fork，子进程继承的 VA 悬垂。

`ensure_init()` 每次比对当前 `(AAA_GetPgId(), AAA_GetVcpuId())` 与缓存的 owner：
**一致即返回（热路径零成本）；失配则丢弃全部本地缓存（meta VA + 块 VA + init 状态），
在当前上下文重新 attach**（DP 线程核绑定，调用中途不会切换上下文）。

- 看门狗仅在 **DP** 绑定（`MEM_MANAGER_USE_API_H && !IS_CP`：这两个 API 只存在于 DP
  链接环境）；CP/独立构建编译期消掉（零成本）。
- 宏 `MM_POOL_GETPID/MM_POOL_GETVCPU` 可覆盖以适配其他环境。

## 8. ShmMapOnPg 池化（CP-only，abe3356）

- `Platform_ShmMapOnPg` 同样进池：**一个名字一个槽**，与用哪个 API 首次映射无关。
- 仅**块创建**不同：`pgidx >= 0` 的块经 `ShmMapOnPg` 钉在 pgname 对应的 NUMA 节点；
  pgname 表容量 32。attach 侧永远用普通 ShmMap 按块名 attach（DP 没有 ShmMapOnPg 符号，
  `mm_pool_try_map_pg` 在 DP 直接返回 0）。
- OnPg 调用者 attach 到钉在**其他** pg 节点的槽时：数据仍然正确（同一槽），仅 NUMA
  局部性不符——限速告警，绝不分裂。

## 9. 防御性硬化与诊断（DP coredump 排查战的产出）

- **meta 快照 sanity 校验**（attach 后、信任前）：`enable ∈ {0,1}`、`threshold/block_size
  非 0`、`block_size ≤ 128MB 且 2MB 对齐`。失败 = 视为无 meta → 干净透传。
  背景：曾观测到旧布局残留 meta（同 magic）导致 `block_size` 读出 0、块 ShmMap 全失败、
  垃圾 nblocks 把 blocks[] 索引带出段外。
- **`nblocks_read()` 钳制** [0, 1024]：损坏 meta 永远不能驱动越界索引。
- **`attach()` 校验 `block_idx` 范围**：损坏表项响亮失败。
- **诊断日志体系**（`mm_pool_log`：CP 走 stderr；DP 走 `LOG_FILE_INFO` 且显式补 `\n`）：
  - `MM_REJECT_LOG`：每类透传原因限速 32 条（含当前配置快照），解释"为什么没进池"；
  - `MM_TRACE(0..5)`：分阶段各限速 64 条——coredump 前最后一条 trace 即故障阶段；
  - meta ShmMap / 块 attach / 上下文切换 / uninit / cleanup 全部留痕（带 pgid、vcpu）。

## 10. Coredump 战史（本分支修复的三个根因）

| # | 现象 | 根因 | 修复 |
|---|---|---|---|
| 1 | 多线程并发 init 偶发 coredump | 进程内并发 `mm_pool_init` 全进 `meta_attach`，OS 若不串行化进程内同名 create → 双创建者，后者的 memset/锁重初始化打烂在用元数据 | CAS 门闩：`g_init_state` 三态，进程内唯一 init 线程（308d2b8） |
| 2 | DP 启动期 `ldr w0,[x20,#192]`（x20=g_meta） | `g_meta` 普通全局跨线程发布 = data-race UB，DP 优化器把 NULL 检查重排到解引用后；启动慢 I/O 下等待方超时读到未发布指针 | `g_meta` 原子化发布/读取 + 独立 NULL 判断（5bb92bf） |
| 3 | 仅按 pgid 校验时 meta 解引用仍崩 | **平台 shm VA 是 per-vcpu 作用域**（A/B 实测确认） | (pgid,vcpu) 执行上下文看门狗，失配即丢弃缓存重 attach（6b5a09c） |

另有两个定位修正：`poolable` 掩码测的是 **flags 值**（业务 type-1 在 api.h 里 #define 为
0U，不是 1）（43fd749）；配置/stdio 全部收口到 CP（112b499）。

## 11. unmap 语义与已知限制

- **bump 不回收槽位**：池内 unmap 只认领（防调用方 OS-unmap 子槽），refcount 不减、槽
  不释放；profiler 视角下池化区域"从不释放"是预期行为。完整回收生命周期随
  slab/freelist 落地。
- **残余风险**：
  1. 跨进程双创建仍依赖 OS 对同名创建的串行化（鸡生蛋问题：锁住在待创建的 meta 里，
     进程间无法用池内原语保护创建本身）；OS 若违背此语义，其自身 refcount 也是错的。
  2. attach 方 ready 屏障是 5000 万次无退让自旋；创建者读 JSON（CP 侧磁盘 I/O）期间，
     低核数 + 大量自旋线程有饿死创建者的理论风险（启动时序改为 Simulator 单点预建后
     已大幅缓解）。
  3. meta 单段 ≤ 2MB：上限 1024 块 / 8192 名字 / 32 pgname；满则一致性透传。
  4. 创建锁冻结（持锁者崩溃）需依赖重启时 `mm_pool_cleanup` 排空恢复。
  5. VA-same flags（4/5）路径共享同一实现但未实测。

## 12. 构建与测试

```sh
cmake -S . -B build -DMEM_MANAGER_POOL=ON          # 启用池（IS_CP/MEM_MANAGER_USE_API_H
cmake --build build                                #  由平台构建环境定义）
ctest --test-dir build --output-on-failure
```

`tests/test_pool.c` 覆盖：功能（相邻打包/同名同槽/阈值透传/掩码外透传/unmap 认领）、
8 线程 × 500 次并发（共享名同槽 + 独占名创建竞争）、跨进程 attach（ready 屏障 +
`mm_pool_reset_for_test` 模拟第二进程）、**8 线程并发 init 竞态回归**、JSON 解析、
冲突响亮失败、uninit/cleanup 生命周期、OnPg 池化。本地构建用 `src/os/os_shm_stub.c`
（堆模拟同名共享 + refcount，AAA_* 映射到 C11 stdatomic）；平台构建由 api.h 提供真实
`AAA_*` 与 `ShmMap/ShmMapOnPg/ShmUnmap`。

## 13. 分支提交历程（main..br_xcc_bump，21 个）

| 阶段 | 提交 |
|---|---|
| v0 框架 | 7d3b4f6 可插拔策略框架上的 bump v0 |
| v1 共享元数据 | ddab32d 共享 meta + 无锁 attach（C11 atomics）；0076f88 同名共享 stub 测跨进程屏障 |
| 正确性加固 | 5cdfcb6 bump-only v1：JSON 配置、VA-differs 寻址、正确性加固 |
| 平台同步原语 | f680cc5/e44c9d5 全部同步统一到平台 AAA_* 接口；3e200f7 profiler 保留 stdatomic |
| 试用调参 | f39f95f 默认 block_size 2MB→64MB |
| **coredump 修复** | 308d2b8 CAS init 门闩；5bb92bf g_meta 原子化 |
| DP 启动攻坚 | 29964f5 执行上下文看门狗 + boot 诊断；90dad58 看门狗仅绑 DP；112b499 配置/stdio 收口 CP；43fd749 掩码测 flags 值（type-1=0U）；3954820 pgid 看门狗 + 诊断；6b5a09c 恢复 (pgid,vcpu) 看门狗 + meta 加固 + 全量诊断 |
| 生命周期 | 9e165b5 要求 Simulator 单点建 meta + PG 创建告警；f88945e mm_pool_uninit；8d39e1d mm_pool_cleanup 兜底 + DP 日志换行修复；d0e714a cleanup 改 refcount-drain（无需 delete API） |
| OnPg | abe3356 ShmMapOnPg 池化：一名一槽、NUMA 钉块 |
