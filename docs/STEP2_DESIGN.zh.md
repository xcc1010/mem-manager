# Step 2 详细设计：配置驱动的共享内存池分配器

> 配套总览见 `设计文档.md`（两步走机制）。本文是 Step 2 的**详细实现设计**，
> 供逐条分析与审阅。标注 **【待拍板】** 的是需要你确认的设计决策。

---

## 0. 范围（v1）

| 请求 | 处理 |
|---|---|
| `size < threshold` 且 `flags` 可池化（默认 VA-same = 4/5） | **进池**（slab 大小类） |
| `size ≥ threshold`（大块） | **透传 OS** |
| VA-differs（flags 0/1/2）、process-shared（3） | **透传 OS**（v1，占比 ~1%） |
| `enable=false` | **全透传**（等价 Step 1，一键回滚） |

保留 OS 两个语义：**名字共享**（同名 attach 拿同一块）+ **引用计数**（返回值=refcount）。

### 已确认决策
- **VA-differs（flags 0/1/2）与 process-shared（flags 3）：一律透传 OS，暂不池化、不考虑。**
  池子只处理 VA-same（flags 4/5）。`poolable_flags_mask` 默认仅含 4、5。这把 v1 的范围限定在
  ~99% 的 VA-same 上，且无需为偏移寻址（VA-differs 必须用 offset）设计任何东西。

---

## 1. 总体分层

```
业务  ──►  Platform_ShmMap  ──►  路由决策
                                   │
                 ┌─────────────────┴───────────────┐
              透传 OS                            进池分配器
            (大块/VA-differs)                       │
                                    ┌──────────────┼───────────────┐
                              名字注册表        大小类 slab        池块来源
                            name→(块,槽)    每类:池块[]+freelist   OS ShmMap
                                                                 (大块,VA-same,按flags/NUMA)
        元数据(注册表/freelist/池块表/锁/配置) ── 放在"元数据共享内存" ── CP建、各进程attach
        配置文件 ── CP 启动读取 ── 解析后写入元数据
```

---

## 2. 关键设计决策（先讲清，影响一切）

### 2.1 池按什么 key 分？
**pool key = (flags 值, [NUMA/pgname — 仅 OnPg], size_class)**

- **按 flags 分**：flags 4（inter-PG 共享）与 5（intra-PG 共享）**共享范围不同**，绝不能放进同一个池块——否则一个 intra-PG 的数据落进 inter-PG 的块，隔离被破坏。所以**每个可池化的 flags 值各自一套池块**。
- **按 NUMA/pgname 分**：`ShmMapOnPg` 要求内存落在指定 NUMA 节点，所以 OnPg 的池块要按节点分别申请。
- **按 size_class 分**：slab 要求同一池块内槽大小固定。

### 2.2 元数据放共享内存、**跨进程**访问 【待拍板】
- 注册表 / free list / 池块表 都在一块**元数据 shm**（固定名字、VA-same）里。
- CP 和各 DP 进程都 attach 到**同一份** → 这样"同名 attach 拿同一槽"才成立。
- ⇒ 并发是**跨进程**的：锁是放在这块共享内存里的**自旋锁**（对共享内存做原子操作跨进程有效）。
- ⚠️ **最大风险点**：持锁进程崩溃 → 死锁 / 状态不一致；refcount 可能泄漏。
  - **【待拍板 A】** 你们是否 **CP 和 DP 都会调用 `ShmMap` 申请新区域**？
    - 若**都会** → 必须跨进程共享分配器（本设计，复杂、有崩溃风险）；
    - 若**只有 CP 申请、DP 只 attach** → 可大幅简化为"单写者"（DP 仅查表+refcount），强烈推荐。

### 2.3 `unmap` 只给地址 → 地址反查槽
池块是大的对齐区域。`Platform_ShmUnmap(addr)`：
1. 在"池块表"里按**地址区间**找到包含 addr 的池块（块基址有序→二分，或按高位建索引）；
2. 槽号 = `(addr − 块基址) / 槽大小`；
3. 槽 → 所属注册项（name, refcount）。
4. 若不落在任何池块 → 是透传出去的 → 调 OS `ShmUnmap`。

### 2.4 只池化 VA-same 才能发裸指针
池块用 **VA-same flags** 申请 → 所有进程看到同一 VA → 块内槽地址跨进程稳定 → 可直接发裸指针、可在共享内存里存指针。这正是"只池化 VA-same"的根因。VA-differs 要用偏移，v1 不池化、直接透传。

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
    UINT32 poolable_flags_mask;       /* 位掩码：哪些 flags 入池（默认 (1<<4)|(1<<5)） */
    int    nclasses;
    SizeClassCfg classes[MAX_CLASSES];
} PoolCfg;

/* ---- 元数据共享区 ---- */
typedef struct {                      /* 一个池块 */
    void*  base;                      /* 本进程内的(VA-same)基址 */
    char   shmname[MM_NAME_MAX];      /* 池块自己的 OS 名字（如 "mmpool/f4/n0/c64k/3"） */
    INT32  flags;                     /* 该块所属 flags */
    int    numa;                      /* -1=非OnPg；否则节点号 */
    UINT32 slot_size, nslots, nfree;
    int    free_head;                 /* 空闲槽链表头（空闲槽内存放下一个空闲槽的索引） */
} PoolBlock;

typedef struct {                      /* 注册项：一个被池化的业务区域（按 name 唯一） */
    char   name[MM_NAME_MAX];
    int    block_idx, slot_idx;
    UINT32 size;                      /* 业务原始申请大小 */
    INT32  flags;
    int    refcount;
    int    state;                     /* 0空 1用 2墓碑（开放寻址用） */
} NameEntry;

typedef struct {
    mm_spinlock_t lock;               /* 跨进程自旋锁 */
    PoolCfg cfg;                      /* 配置快照 */
    int     nblocks;
    PoolBlock blocks[MM_MAX_BLOCKS];
    NameEntry names[MM_NAME_TABLE_CAP];   /* name→槽 的开放寻址哈希 */
    /* per-(flags,numa,class) 的"当前可分配块"索引，v1 可先线性扫 blocks[] */
} PoolMeta;
```

> 备注：v1 用**定长数组 + 上限**（`MM_MAX_BLOCKS` 等），简单、无动态内存、契合无 libc 的 DP。
> 上限值通过配置/编译期常量给，超出则兜底透传。

---

## 4. 核心流程（伪代码）

### 4.1 初始化（CP / 平台 init）
```
读配置文件 → PoolCfg
meta = OS ShmMap("mmpool/meta", sizeof(PoolMeta), VA-same)   // 固定名元数据区
若首次创建: 置零; spin_init(meta.lock); meta.cfg = PoolCfg
(可选) 按各类 init_blocks 预留池块
DP 启动: attach "mmpool/meta"（同名）→ 拿到同一份 meta，不读文件
```

### 4.2 Platform_ShmMap
```
INT32 Platform_ShmMap(flags, name, size, void** out):
  if !cfg.enable or size >= cfg.threshold or !poolable(flags, cfg):
      return OS ShmMap(flags, name, size, out)           // 透传

  lock(meta)
  e = name_lookup(name)
  if e != NULL:                                          // 同名 attach
      e.refcount += 1
      *out = slot_addr(e)
      rc = e.refcount
  else:
      cls = pick_class(size, cfg)                        // 最小满足 size 的大小类
      if cls < 0: unlock; return OS ShmMap(...)          // 没有合适类 → 透传
      slot = alloc_slot(flags, numa=-1, cls)             // 不够则增长池块
      if slot == FAIL: unlock; return OS ShmMap(...)     // 池满兜底 → 透传
      e = name_insert(name, slot, size, flags)           // 登记
      e.refcount = 1
      *out = slot_addr(e)
      rc = 1
  unlock(meta)
  return rc                                              // 返回值=引用数（与 OS 一致）
```

### 4.3 alloc_slot（含池块增长）
```
alloc_slot(flags, numa, cls):
  blk = find_block_with_free(flags, numa, cls.slot_size)
  if blk == NULL:
      if class_block_count(flags,numa,cls) >= cls.max_blocks (且非0): return FAIL
      name_blk = make_pool_block_name(flags, numa, cls)
      base = (numa<0) ? OS ShmMap(flags, name_blk, cfg.block_size, ...)
                      : OS ShmMapOnPg(flags, pg_of(numa), name_blk, cfg.block_size, ...)
      if base == NULL: return FAIL
      blk = register_block(base, name_blk, flags, numa, cls.slot_size)
      build_free_list(blk)                               // 切 nslots 个槽串成 free list
  slot = blk.free_head; blk.free_head = next_of(slot); blk.nfree--
  return (blk, slot)
```

### 4.4 Platform_ShmUnmap
```
INT32 Platform_ShmUnmap(void* addr):
  blk = find_block_containing(addr)                      // 地址区间查（块表二分）
  if blk == NULL: return OS ShmUnmap(addr)               // 非池内存 → 透传

  lock(meta)
  slot = (addr - blk.base) / blk.slot_size
  e = entry_of(blk, slot)                                // 反查注册项
  e.refcount -= 1
  if e.refcount <= 0:
      free_slot(blk, slot)                               // 槽压回 free list, nfree++
      name_remove(e)                                     // 注册项置墓碑
  unlock(meta)
  return 0
```

### 4.5 Platform_ShmMapOnPg
同 `Platform_ShmMap`，但：`numa = resolve(pgname)`；池块用 `ShmMapOnPg` 申请；pool key 带上 numa。

---

## 5. 配置文件

格式与字段详见 `设计文档.md §5`（INI 风格、十六进制字节、可增删大小类、增长策略、`enable` 一键回滚）。补充：

- **解析放 CP 端**：手写极简 INI 解析（几十行，不引第三方库）；结果填 `PoolCfg` 存进元数据 shm。
- **DP 不读文件**：attach 元数据 shm 即拿到 `cfg`。
- 路径由 `MEM_POOL_CONFIG` 环境变量或平台既有配置入口给。

---

## 6. 边界与异常

| 情况 | 处理 |
|---|---|
| 池满（达 `max_blocks`） | **兜底透传 OS**（退化但不失败） |
| `size` > 最大大小类但 < `threshold` | 透传 OS（或配置新增更大类） |
| 名字哈希表满 | 透传 OS（或扩容，v1 先透传 + 告警） |
| `name` 超长 | 截断或拒绝（**【待拍板】** `MM_NAME_MAX` 取值） |
| 持锁进程崩溃 | **v1 风险点**：死锁/不一致（**【待拍板 B】** 健壮锁/恢复要求） |
| refcount 泄漏（进程崩溃未 unmap） | v1 不回收（同上） |
| `enable=false` | 全透传，等价 Step 1 |

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

## 9. 需要你拍板的点（审阅重点）

1. **【A】谁会调 `ShmMap` 申请新区域？** CP/DP 都会 → 跨进程共享分配器（复杂）；只 CP 申请、DP 仅 attach → 可大幅简化为单写者。**这条最关键，决定整体复杂度与风险。**
2. **【B】崩溃恢复要求多高？** v1 能否接受"持锁崩溃/refcount 泄漏"的弱一致（靠重启恢复）？
3. **OS 对 shm "段数量"有无上限 / 每段固定开销？** 影响是否值得把部分大块也纳入池、阈值定多大。
4. **flags 4 与 5 必须分池**（我认为是，隔离不能破）—— 确认。
5. **大小类初值 + 池块大小**（16/64/128/256K…，块 2MB？）。
6. **`MM_NAME_MAX` / 表与块的上限**（定长数组上界）。
7. ~~VA-differs（~1%）v1 透传是否可接受？~~ → **已确认：透传，暂不考虑**（见 §0）。

---

## 10. 实施顺序（建议）

1. 先实现 **enable=false 透传 + 配置解析 + 元数据 shm 框架**（零行为变化，先打通基础设施）。
2. 实现 **slab 分配/释放 + 名字注册表 + 引用计数**（单进程/单线程先跑通正确性）。
3. 接 **地址反查 + unmap 回收**。
4. 加 **跨进程锁 + 池块增长 + 兜底透传**。
5. 接 **OnPg/NUMA**。
6. 复用 Step 1 工具**验证内存占用下降**，据数据在配置里调大小类。
