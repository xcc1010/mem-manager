# Step 2 详细设计：配置驱动的共享内存池分配器

> 配套总览见 `设计文档.md`（两步走机制）。本文是 Step 2 的**详细实现设计**，
> 供逐条分析与审阅。标注 **【待拍板】** 的是需要你确认的设计决策。

> **2026-07 范围更新（已定）**：v1 只实现 **bump** 策略（业务以 init-once/长生命周期
> 分配为主，slab/freelist 移出 v1 范围）；配置文件格式定为 **JSON**（扁平对象，
> 手写极简解析器，见 `config/pool.json` 与 `mm_pool_load_config`）。
>
> **2026-07 数据修正（推翻早期"~99% VA-same"结论）**：实际业务**几乎全是 VA-differs**
> （flags 0/1/2）。v1 因此**直接池化 VA-differs**：共享元数据只存偏移/索引、不存指针，
> 各进程按块名自行 attach 池块、用本地 VA + 偏移寻址（同一路径天然兼容 VA-same）。
> 默认可池化 flags = **{1}**（CP+DP 进程组间共享）。当前代码（`src/pool/mm_pool.c`）
> 即按此实现并已进入内部项目试用。

---

## 0. 范围（v1）

| 请求 | 处理 |
|---|---|
| `size < threshold` 且 `flags` 可池化（**默认 {1}** = VA-differs，CP+DP 进程组间共享） | **进池**（bump 切槽） |
| `size ≥ threshold`（大块） | **透传 OS** |
| 掩码外的 flags（含 process-shared 3、未启用的 0/2/4/5） | **透传 OS** |
| `enable=false` | **全透传**（等价 Step 1，一键回滚） |

保留 OS 两个语义：**名字共享**（同名 attach 拿同一块）+ **引用计数**（返回值=refcount）。

### 已确认决策（2026-07 修正后）
- **直接池化 VA-differs**：早期"~99% VA-same、VA-differs 仅占 ~1% 故透传"的数据有误，
  实际业务几乎全是 VA-differs（0/1/2），它们才是浪费大头。池化方案：共享元数据
  **只存偏移/索引**（VA-differs 下裸指针跨进程无意义），各进程按池块名自行
  `ShmMap` attach、缓存本地 VA，地址 = 本地基址 + 偏移。同一路径兼容 VA-same。
- **process-shared（flags 3）：透传 OS**，不池化（语义不同，不进掩码）。
- **不同 flags 值绝不共用池块**（共享范围隔离），池块按 flags 严格分开。

---

## 1. 总体分层

```
业务  ──►  Platform_ShmMap  ──►  路由决策
                                   │
                 ┌─────────────────┴───────────────┐
              透传 OS                            进池分配器
        (大块/掩码外 flags)                         │
                                    ┌──────────────┼───────────────┐
                              名字注册表        bump 切槽          池块来源
                            name→(块,偏移)   (v1, 无大小类)     OS ShmMap
                                                          (大块,按 flags 分块)
        元数据(注册表/池块表/锁/配置) ── 放在"元数据共享内存"(只存偏移) ── CP建、各进程attach
        配置文件(JSON) ── CP 启动读取 ── 解析后写入元数据
        各进程本地: block_idx → 本进程 VA (按块名 attach 一次, 之后纯查表)
```

---

## 2. 关键设计决策（先讲清，影响一切）

### 2.1 池按什么 key 分？
**pool key = (flags 值, [NUMA/pgname — 仅 OnPg], size_class)**

- **按 flags 分**：不同 flags 值**共享范围不同**（如 inter-PG 与 intra-PG），绝不能放进同一个池块——否则一个 intra-PG 的数据落进 inter-PG 的块，隔离被破坏。所以**每个可池化的 flags 值各自一套池块**。
- **按 NUMA/pgname 分**：`ShmMapOnPg` 要求内存落在指定 NUMA 节点，所以 OnPg 的池块要按节点分别申请（v1 未实现，OnPg 透传）。
- **按 size_class 分**：slab 要求同一池块内槽大小固定（v1 为 bump，无大小类）。

### 2.2 元数据放共享内存、跨进程访问
- 注册表 / 池块表 都在一块**元数据 shm**（固定名字 `mmpool/meta`，flags=1，**只存偏移/索引、不存指针**）里。
- CP 和各 DP 进程都 attach 到**同一份** → 这样"同名 attach 拿同一槽"才成立。
- **【已定 A】CP 和 DP 都能申请新区域**（DP 的小块占浪费大头，必须也能池化）。
- 并发模型见 §2.5：**单一共享池**（打包最密、最省内存）+ **`TrySpinLock`（拿不到就兜底透传，永不死锁）** + **原子引用计数**。
  （否决了"每进程 arena"：它会牺牲打包密度，违背省内存的初衷。）

### 2.3 `unmap` 只给地址 → 地址反查槽
池块是大的对齐区域。`Platform_ShmUnmap(addr)`：
1. 在"池块表"里按**地址区间**找到包含 addr 的池块（块基址有序→二分，或按高位建索引）；
2. 槽号 = `(addr − 块基址) / 槽大小`；
3. 槽 → 所属注册项（name, refcount）。
4. 若不落在任何池块 → 是透传出去的 → 调 OS `ShmUnmap`。

### 2.4 VA-differs 寻址：元数据只存偏移（2026-07 修正）
早期方案是"只池化 VA-same、发裸指针"，根因是 VA-differs 内存里裸指针跨进程无意义。
数据修正后（业务几乎全是 VA-differs），v1 改为**直接池化 VA-differs**：

- 共享元数据（注册表/块表/锁/配置）**只存偏移与索引**（`block_idx, off, size, flags`），
  不存任何指针 → VA 是否相同对元数据不再重要；
- 每个进程对用到的池块**按块名自行 `ShmMap` attach**，拿到本进程的 VA 并缓存
  （进程本地表 `block_idx → base`）；
- 对外地址 = 本地基址 + 偏移；同一条路径对 VA-same 的块也正确（同名 attach 各进程同 VA）。
- 代价：每进程每块一次性 attach（稳态后纯查表，attach 热路径仍无锁）。

### 2.5 并发与多进程模型（已定，核心）
**模型：单一共享池**（打包最密、最省内存）。CP/DP 都能申请。用 DP 现成的**自旋锁 + 原子**协调。

**(1) 创建路径**（切槽 / 建新池块 / 名字表插入·删除）—— 跨进程**自旋锁**保护，但用 **`TrySpinLock` + 有限重试**：
- 拿到 → 做结构性修改 → `SpinUnlock`；
- 拿不到（持锁者卡住/崩溃）→ **兜底透传 OS `ShmMap`**。**永不死锁**，最坏退化为"这次不池化"，系统照跑。
- ⇒ **锁是否崩溃健壮不影响安全性**（不依赖它）。

> **v1 修正（防同名双映射）**：拿不到锁时**不能立刻透传**——若同名的并发创建正在进行，
> 透传会让该名字同时存在于池和 OS（共享被撕裂）。v1 实现为**有界自旋 + 反复重查注册表**，
> 预算（~100ms）远覆盖一次锁内 OS ShmMap；只有锁被死亡持有者冻结 / 池满时才透传，
> 此时所有进程同样失败，同名经 OS 一致共享，路由不错乱。

**(2) 临界区极小**：新池块的 OS `ShmMap`（慢、可能阻塞）在**锁外**做好，锁内只做几行赋值/链表操作（纳秒级）→ 崩溃窗口极小。且创建属 init-once，稳态几乎不触发。

> **v1 实际取舍**：OS `ShmMap` **保留在锁内**。挪到锁外会让"慢速创建"失去串行化，
> 同名可能被注册两次，需引入 CREATING 状态机才能闭合——v1 不值得。代价：持锁者若在
> ShmMap 中崩溃，锁冻结（见 (1) 修正后的行为：新名字一致透传，系统仍正确）。

**(3) attach 路径无锁（性能关键）**：业务频繁 map 一块**已存在**的共享区域（attach），不应争全局锁：
- 名字表项**插入**在锁内完成，**最后一步**用 release 原子写置 `state=USED`；
- **查表**用 acquire 原子读 `state`，读到 `USED` 才用（保证看到完整项）；
- **引用计数**用原子自加/自减。
- ⇒ **attach = 无锁查表 + 原子 refcount，不碰全局锁** → 对"性能严格"友好。
- 注：这只是"表项就绪位的原子发布"，池仍是**单一共享池、打包密**，与被否的"每进程 arena"无关。
  若更想要极简，也可把查表放进锁内（代价：attach 也争锁）。

**(4) NUMA**：部署固定、消费者大多同节点 → 池块放在**该公共节点**；OnPg 路径**按节点分池**。均由配置静态指定。

**(5) 释放**：detach 用原子自减；减到 0 时置 `state=TOMBSTONE`（不立刻动 free list）；
切槽方持锁时顺带回收墓碑槽。init-once/shutdown-free 下几乎只在收尾回收。

**用 DP 原语实现**：

| 角色 | 原语 | 内存序 |
|---|---|---|
| 创建路径互斥（切槽/建块/名字增删） | `TrySpinLock`/`SpinUnlock`（拿不到→透传兜底） | — |
| refcount 增减（attach/detach） | 原子自加/自减 | acq_rel |
| 发布表项（插入最后一步写 state） | `Atomic32Set`（release） | release |
| 查表读 state（attach 无锁） | `Atomic32ReadAcquire` | acquire |

**崩溃**：TrySpinLock+兜底 → 降级不死锁；半截创建最多泄漏一个槽（不影响别人）。无全系统死锁。

---

## 3. 数据结构（伪 C）

```c
/* ---- 配置（运行时，解析自配置文件）---- */
typedef struct {
    UINT32 slot_size;                 /* 该大小类的槽大小（字节） */
    UINT32 init_blocks, grow_blocks, max_blocks;
} SizeClassCfg;

typedef struct {
    int    enable;
    UINT32 threshold;                 /* ≥ 此值透传 OS（默认 0x200000=2MB） */
    UINT32 block_size;                /* 池块大小（OS 申请粒度，默认 2MB） */
    UINT32 poolable_flags_mask;       /* 位掩码：哪些 flags 入池（默认 1<<1） */
    int    nclasses;
    SizeClassCfg classes[MAX_CLASSES];
} PoolCfg;

/* ---- 元数据共享区 ---- */
typedef struct {                      /* 一个池块（单一共享池，所有进程共用） */
    /* 2026-07 修正：不存 base 指针（VA-differs 下各进程 VA 不同）；
     * 各进程用 shmname 自行 attach，本地缓存 block_idx → 本进程 VA */
    char   shmname[MM_NAME_MAX];      /* 池块自己的 OS 名字 */
    INT32  flags;                     /* 该块所属 flags */
    int    numa;                      /* -1=非OnPg；否则节点号 */
    UINT32 block_size;
    union {                           /* 策略私有状态（见 §3.5） */
        struct { UINT32 slot_size, nslots, nfree; int free_head; } slab;
        struct { UINT32 next; }                                    bump;
        struct { UINT32 free_head; }                               flist; /* 首个空闲chunk偏移 */
    } u;
} PoolBlock;

typedef struct {                      /* 注册项：一个被池化的业务区域（按 name 唯一） */
    char   name[MM_NAME_MAX];
    int    block_idx;
    UINT32 off;                       /* 块内字节偏移（slab: 槽号=off/slot_size） */
    UINT32 size;                      /* 业务原始申请大小 */
    INT32  flags;
    INT32  refcount;                  /* 原子自加/自减（attach/detach，不持 g.lock） */
    INT32  state;                     /* 插入时持锁写、最后一步 release 置 USED；查表 acquire 读 */
} NameEntry;                          /*   0空 1用(USED) 2墓碑(TOMBSTONE) */

typedef struct {
    mm_spinlock_t lock;               /* 跨进程自旋锁：保护"创建路径"（切槽/建块/名字增删）；
                                         用 TrySpinLock + 兜底透传，不依赖它崩溃健壮 */
    PoolCfg   cfg;                    /* 配置快照（CP 启动时写入） */
    INT32     nblocks;                /* 池块数（持锁修改） */
    PoolBlock blocks[MM_MAX_BLOCKS];
    NameEntry names[MM_NAME_TABLE_CAP];   /* name→槽 开放寻址哈希：插入持锁+release发布，查表 acquire 无锁 */
} PoolMeta;
```

> 备注：v1 用**定长数组 + 上限**（`MM_MAX_BLOCKS` 等），简单、无动态内存、契合无 libc 的 DP。
> 上限值通过配置/编译期常量给，超出则兜底透传。

### 3.5 可插拔分配策略（slab / freelist / bump，三选，配置决定）

> **2026-07 更新：v1 只实现 bump**，本节保留作为后续演进的设计参考（配置中已无
> `strategy` 项）。

把"框架"与"怎么从块里切一块"解耦：**名字表、引用计数、路由、锁、NUMA、地址反查全部与策略无关**，
每个"逻辑池"选一种策略，**只有 `carve`/`release` 两个函数不同**。

```c
typedef enum { ALLOC_SLAB, ALLOC_FREELIST, ALLOC_BUMP } AllocKind;

typedef struct {                 /* 一个逻辑池：服务某 (flags,node) 下某 size 范围 */
    AllocKind kind;
    UINT32 min_size, max_size;   /* 覆盖的请求大小区间 */
    UINT32 block_size;
    INT32  flags; int numa;      /* (flags,node) 维度由系统按需实例化，不必在配置里展开 */
    /* 它的 PoolBlock 列表索引；每块的策略私有状态见 §3 的 union */
} Pool;

/* 持锁调用：从池切 size 字节 → (块,偏移)，不够则建新块；返回 FAIL=池满 */
int  pool_carve  (Pool* p, UINT32 size, int* blk_out, UINT32* off_out);
/* 释放（按策略不同行为） */
void pool_release(Pool* p, int blk, UINT32 off, UINT32 size);
```

三种策略的 `carve`/`release`：

| 策略 | carve | release | 复杂度/特性 |
|---|---|---|---|
| **slab** | 该大小类块里弹 `u.slab.free_head` | 槽压回 free_head | O(1)、零外碎片、有档间内碎片；**支持复用槽** |
| **freelist** | 遍历空闲 chunk 找够大的(first/best fit)，按需 split | chunk 插回 + 合并相邻 | O(n)、精确无内碎片、**有外碎片**、临界区长 |
| **bump** | 对齐 `u.bump.next` 后推进（可做成**原子 fetch-add → 无锁**） | **空操作**（整块收尾回收） | O(1)、精确、无外碎片、最省最快；**不单独复用槽** |

共同点：`addr = block.base + off`；unmap 的"地址反查 (块,偏移) → 注册项"也与策略无关。
⇒ **新增/切换策略，改动只局限在 `carve`/`release`。**

选型建议：**会运行期复用槽 → slab**；**任意大小+频繁复用(像通用 malloc) → freelist**；
**init-once/长生命周期不复用 → bump（最优）**。可全局选一种，也可按 size 范围分多个逻辑池各用各的。

---

## 4. 核心流程（伪代码）

### 4.1 初始化（CP / 平台 init）
```
读配置文件 → PoolCfg
meta = OS ShmMap("mmpool/meta", sizeof(PoolMeta), flags=1)   // 固定名元数据区，只存偏移
若首次创建: 置零; spin_init(meta.lock); meta.cfg = PoolCfg
(可选) 按各类 init_blocks 预留池块
DP 启动: attach "mmpool/meta"（同名）→ 拿到同一份 meta，不读文件
```

### 4.2 Platform_ShmMap
```
INT32 Platform_ShmMap(flags, name, size, void** out):
  if !cfg.enable or size >= cfg.threshold or !poolable(flags, cfg):
      return OS ShmMap(flags, name, size, out)           // 透传

  e = name_lookup_acquire(name)                          // 无锁查表（acquire 读 state）
  if e != NULL:                                          // 同名 attach —— 全程无锁
      *out = slot_addr(e)
      return Atomic_Inc(&e.refcount)                     // 原子自增，返回引用数

  node = resolve_node(flags, name, caller)               // OnPg→pgname节点；非OnPg→调用方节点
  pool = pick_pool(flags, node, size)                    // 选覆盖该 size 的逻辑池(及其策略)
  if pool == NULL: return OS ShmMap(...)                 // 无合适池 → 透传

  if !TrySpinLock(&meta.lock, RETRY_LIMIT):              // 拿不到锁（持锁者卡住/崩溃）
      return OS ShmMap(...)                              // → 兜底透传，永不死锁
  e = name_lookup(name)                                  // 双检：拿锁期间别人可能已创建
  if e == NULL:
      if !pool_carve(pool, size, &blk, &off):            // 按策略切（slab/freelist/bump，见 3.5/4.3）
          SpinUnlock(&meta.lock); return OS ShmMap(...)  // 池满 → 透传
      e = name_insert_publish(name, blk, off, size, flags)  // 写好字段，最后一步 release 置 state=USED
  rc = Atomic_Inc(&e.refcount)
  *out = addr_of(e)                                      // = blocks[e.block_idx].base + e.off
  SpinUnlock(&meta.lock)
  return rc                                              // 返回值=引用数（与 OS 一致）
```

### 4.3 pool_carve（按策略派发，含池块增长）
```
pool_carve(pool, size, &blk, &off):                      // 持 meta.lock
  blk = find_block_in_pool_with_room(pool, size)         // slab:有空槽；bump:next+size≤块；flist:有够大chunk
  if blk == NULL:
      if pool_block_count(pool) >= pool.max_blocks (且非0): return FAIL
      name_blk = make_pool_block_name(pool)
      // 慢的 OS 申请尽量放锁外（reserve 模式）；这里简化为同步
      base = (pool.numa<0) ? OS ShmMap   (pool.flags, name_blk, pool.block_size, ...)
                           : OS ShmMapOnPg(pool.flags, pg_of(pool.numa), name_blk, pool.block_size, ...)
      if base == NULL: return FAIL
      blk = register_block(pool, base, name_blk)          // 按 pool.kind 初始化 union（slab切槽/bump置next=0/flist整块为一空闲chunk）
  // 按策略切：
  switch pool.kind:
    SLAB:     off = pop_free_slot(blk)                    // O(1)
    BUMP:     off = align(blk.u.bump.next); blk.u.bump.next = off+size   // O(1)（可改原子fetch-add）
    FREELIST: off = first_fit_split(blk, size)            // O(n)+split
  return true
```

`pool_release(pool, blk, off, size)`：SLAB→压回 free_head；BUMP→空操作；FREELIST→插回+合并相邻。

### 4.4 Platform_ShmUnmap
```
INT32 Platform_ShmUnmap(void* addr):
  blk = find_block_containing(addr)                      // 地址区间查（无锁读块表，块表只在创建时增）
  if blk == NULL: return OS ShmUnmap(addr)               // 非池内存 → 透传

  off = addr - blk.base                                  // 块内偏移（与策略无关）
  e = entry_of(blk, off)                                 // 反查注册项（块内 offset→entry 索引）
  if Atomic_Dec(&e.refcount) == 0:                       // 原子自减，无锁
      Atomic_Set_Release(&e.state, TOMBSTONE)            // 标记可回收，不立刻动 free list
      // 槽的实际回收：由后续"创建路径"持锁时顺带扫墓碑归还，或收尾统一回收
  return 0
```

### 4.5 Platform_ShmMapOnPg
同 `Platform_ShmMap`，但：`numa = resolve(pgname)`；池块用 `ShmMapOnPg` 申请；pool key 带上 numa。

---

## 5. 配置文件（JSON，v1 已实现）

- **格式：扁平 JSON 对象**（2026-07 定）。手写极简解析器（`mm_pool_load_config`，
  无第三方库），只认扁平键值 + 一个整数数组，未知键忽略。
- **解析放 CP 端 / 元数据创建方**：读文件后把结果填 `Mm_PoolCfg` 存进元数据 shm。
- **DP 不读文件**：attach 元数据 shm 即拿到同一份 `cfg` 快照。
- 路径由 `MEM_POOL_CONFIG` 环境变量给；优先级：**内置缺省 < JSON 文件 < `MEM_POOL_*` 环境变量**
  （`MEM_POOL_ENABLE=0` 依旧是一键回滚）。

v1 配置项（见 `config/pool.json`）：

```json
{
  "enable": true,                  // false = 全透传（等价 Step 1，一键回滚）
  "threshold": "0x200000",         // ≥ 此值透传 OS（大块不进池）；数字或 "0x.." 字符串
  "block_size": "0x200000",        // 池块大小（OS 申请粒度）
  "poolable_flags": [1]            // 只池化 flags=1（VA-differs，CP+DP 进程组间）
}
```

> 最简起步即上例：`range = 0:threshold` 的全部小块都进 bump 池。
> 后续若需要大小类/多策略，在 JSON 里扩展键即可，不改加载机制。

### 5.1 演进预留：多逻辑池 + 策略选择（v1 未启用）

原设计每个逻辑池声明策略（slab/freelist/bump）+ 覆盖的 size 范围 + 块大小，路由按
`size` 落到覆盖它的逻辑池；`(flags, node)` 维度由系统自动实例化。v1 收敛为单一 bump
池后，这些键（如 `pools` 数组、`classes`）**留待后续按真实数据再引入**。

---

## 6. 边界与异常（v1 实现行为）

| 情况 | 处理 |
|---|---|
| 池满（达 `MM_MAX_BLOCKS`） / 块放不下 | **兜底透传 OS**（所有进程一致失败，同名仍经 OS 共享，路由不错乱） |
| 名字哈希表满 | 透传 OS（同上，一致性成立） |
| 创建路径拿不到锁 | **有界自旋 + 反复重查注册表**（~100ms 预算 ≫ 一次 OS ShmMap）；等不到且表项仍未出现才透传 → **同名不会同时进池和进 OS** |
| 持锁进程崩溃 | 锁冻结：已池化名字照常 attach（无锁），新名字一致透传 OS → 池冻结但**系统行为正确**，无死锁 |
| attach 与注册项不一致（flags 不同 / size 超登记值） | **明确报错**（返回 <0，`*shm=NULL`），不静默截断/分裂 |
| `name` 超长（≥ `MM_NAME_MAX`=64） / 非 NUL 结尾 | 透传 OS（有界扫描，不越界读） |
| 元数据创建者中途崩溃 | attacher **有界等待** ready（超时按无 meta 处理 → 透传），不永久卡死 |
| refcount 泄漏（进程崩溃未 unmap） | v1 不回收（bump 无槽位回收；profiler 会把池化区域报为 never-freed，分析时注意口径） |
| `enable=false` | 全透传，等价 Step 1。**前提：由 meta 创建者生效**——配置快照在共享 meta 里，须所有进程退出、meta 区域销毁后重建才换配置 |

---

## 7. 可观测 / 复用 Step 1

池上线后要能验证效果，复用 Step 1 的采集与工具：
- 统计：池命中率、各大小类利用率（已用槽/总槽）、池块数、透传比例、内部碎片（已用 size 之和 / 占用槽之和）。
- 用 `analyze_profile.py` / `crosscheck_os_log.py` 持续核对：**池化后 OS 实际占用应从 ~28.8GB 降到接近真实 ~1.3GB**。

---

## 8. 内部碎片估算（用实测分布初选大小类）

实测主体在 16K–256K。大小类分得越细，内部碎片越小，但池块利用率/管理项越多。
建议初值（**【待拍板 C】**）：`16K, 32K, 64K, 128K, 256K, 512K, 1M`（threshold=2MB）。
- 例：90KB 请求 → 128K 类 → 内部碎片 38KB（比独占 2MB 省 ~16 倍）。
- 上线后据真实 size 直方图，在**配置文件**里调档（无需改代码）。

---

## 9. 决策状态（审阅重点）

**已确认：**
- **【A】CP/DP 都能申请** → **单一共享池** + `TrySpinLock`（拿不到→兜底透传）+ 原子 refcount（§2.5）。否决"每进程 arena"（牺牲打包密度）。
- **【B】崩溃**：`TrySpinLock`+兜底 → 最坏**降级透传、绝不死锁**；**不依赖锁崩溃健壮**。
- **NUMA**：部署固定、消费者大多同节点 → 池块放**公共节点**；OnPg 按节点分池；配置静态指定。
- **范围（2026-07 修正）**：VA-differs（0/1/2）**进池**（默认仅 {1}）；process-shared（3）/大块/掩码外 flags → 透传（§0）。
- **不同 flags 值分池**（共享范围不同，隔离不能破）。
- **元数据只存偏移**，各进程本地解析 VA（§2.4）。
- **attach 无锁**（查表 acquire + 原子 refcount），创建才持锁 → 性能严格友好。
- **【2026-07】v1 只做 bump**：slab/freelist 移出范围；`Mm_PoolCfg` 无 `strategy` 项。
- **【2026-07】配置文件格式定为 JSON**（扁平对象；`MEM_POOL_CONFIG` 给路径，缺省 < 文件 < 环境变量）。

**待定（不阻塞主体设计）：**
1. **OS 对 shm "段数量"有无上限 / 每段固定开销？**（影响是否把部分大块也纳入池、阈值大小）
2. **大小类初值 + 池块大小**（建议 16/32/64/128/256K/512K/1M，块 2MB；上线后据数据在配置里调）。
3. **`MM_NAME_MAX` / 名字表·块表上限**（定长数组上界取值）。
4. **DP 创建 vs attach 比例**（决定锁争用强度；可用"创建者归属"统计量化，不阻塞）。

---

## 10. 实施顺序（v1 现状）

1. ~~先实现 **enable=false 透传 + 配置解析 + 元数据 shm 框架**~~（已完成，JSON 配置）。
2. **bump 分配 + 名字注册表 + 引用计数**（已完成；v1 收敛为 bump-only，跳过 slab）。
3. **地址反查 + unmap 认领**（已完成；bump 不回收槽，回收留待后续策略）。
4. **跨进程锁 + 池块增长 + 兜底透传**（已完成；attach 热路径无锁）。
5. 接 **OnPg/NUMA**（未做，`Platform_ShmMapOnPg` 仍透传）。
6. 内部项目试用中：复用 Step 1 工具**验证内存占用下降**，据数据在 JSON 配置里调参。
