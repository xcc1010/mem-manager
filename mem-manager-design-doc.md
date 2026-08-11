# mem-manager 设计文档与代码重点实现分析

> 项目地址: https://github.com/xcc1010/mem-manager  
> 分析时间: 2026-08-11  
> 分析分支: main

---

## 1. 项目概述

**mem-manager** 是一个面向企业级平台的共享内存池管理器。其核心动机是：

> OS 共享内存原语（`ShmMap` / `ShmMapOnPg` / `ShmUnmap`）的最小分配粒度为 **2MB**，而业务代码大量分配小块内存，导致严重的空间浪费。

项目采用**两步走**策略：
- **Step 1（已完成）**: `Platform_Shm*` 透传包装器 + Debug-only Profiler，收集真实分配模式（size / frequency / lifetime / sharing semantics）
- **Step 2（计划中）**: 基于 Step-1 数据的可配置池分配器

---

## 2. 系统架构

### 2.1 双平面架构（CP / DP）

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                         mem-manager 系统架构                                 │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  ┌─────────────────────────────┐    ┌─────────────────────────────────────┐ │
│  │    Control Plane (CP)       │    │        Data Plane (DP)              │ │
│  │    单进程，dlopen N 个 .so   │    │    进程组(PG)，绑定 CPU 核心         │ │
│  │    C++ 可用                  │    │    C-only，无 libc                  │ │
│  │    有完整 libc               │    │    单线程/核心，run-to-completion    │ │
│  └─────────────┬───────────────┘    └─────────────┬───────────────────────┘ │
│                │                                  │                         │
│                ▼                                  ▼                         │
│  ┌─────────────────────────────┐    ┌─────────────────────────────────────┐ │
│  │  Platform_ShmMap()          │    │  Platform_ShmMap()                  │ │
│  │  Platform_ShmMapOnPg()      │    │  (ShmMapOnPg 不存在于 DP 链接环境)   │ │
│  │  Platform_ShmUnmap()        │    │  Platform_ShmUnmap()                │ │
│  └─────────────┬───────────────┘    └─────────────┬───────────────────────┘ │
│                │                                  │                         │
│                ▼                                  ▼                         │
│  ┌─────────────────────────────┐    ┌─────────────────────────────────────┐ │
│  │  CP Profiler Backend        │    │  DP Profiler Backend                │ │
│  │  - 写 JSONL 到 per-pid 文件 │    │  - 通过 OS LOG_FILE_INFO 宏输出      │ │
│  │  - open/write/close         │    │  - 无状态，无缓冲区                   │ │
│  │  - 实时解析 lifetime        │    │  - 离线由 analyze_profile.py 关联     │ │
│  │  - atomic_flag 自旋锁        │    │  - atomic reentrancy flag            │ │
│  └─────────────────────────────┘    └─────────────────────────────────────┘ │
│                                                                             │
│  ┌─────────────────────────────────────────────────────────────────────┐   │
│  │                     OS Shared-Memory Primitives                       │   │
│  │  ShmMap / ShmMapOnPg / ShmUnmap (2MB 最小粒度)                       │   │
│  └─────────────────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────────────────┘
```

**关键约束**：
- **DP 是 C-only** → 整个实现必须是 C11
- **DP 无 libc** → profiler 不能使用 malloc/stdio/snprintf/open/write 等标准名
- **CP 和 DP 是不同进程** → profiler 必须写 **per-process 文件**，日志永不交错
- **NUMA 维度**：`ShmMapOnPg` 将内存分配到绑定核心的 NUMA 节点上

---

## 3. 核心数据结构与算法

### 3.1 flags 语义（GetShmFlags 返回值）

`flags` **不是不透明值**，它编码了共享范围和虚拟地址（VA）一致性：

| flags | 共享范围 | VA 类别 |
|------:|----------|---------|
| 0 | CP kernel thread + DP inter-PG shared | **per-proc VA differs** |
| 1 | CP + DP inter-PG shared | **per-proc VA differs** |
| 2 | CP + DP; DP intra-PG shared | **per-proc VA differs** |
| 3 | CP + DP process shared | process-shared |
| 4 | CP + DP inter-PG shared | **per-proc VA SAME** |
| 5 | CP + DP; DP intra-PG shared | **per-proc VA SAME** |

**Step-2 硬约束**：
- **VA-same (4/5)**：可在共享内存中存储裸指针 → 池分配器可用指针元数据
- **VA-differs (0/1/2)**：裸指针在另一进程无效 → 池分配器必须返回 **offset**，元数据也必须基于 offset
- **VA-same 和 VA-differs 绝不能共享同一个池**

实测：~**99% 的分配是 VA-same**，所以 Step-2 v1 可以是一个单一的 VA-same 池，让罕见的 VA-differs 直通到 `ShmMap`。

### 3.2 Live Set — 开放寻址哈希表（CP Backend）

```c
typedef struct {
    const void* key;    // mapping address returned by ShmMap
    char* name;         // owned copy of shmname
    UINT32 size;
    int64_t t_map;      // map 时间戳 (ns)
    int state;          // 0=empty, 1=used, 2=tombstone
} Entry;

typedef struct {
    Entry* slots;
    size_t cap;
    size_t count;       // used slots
    size_t tombs;
} Table;
```

**哈希函数**（64-bit FNV-1a 风格）：
```c
static size_t hash_ptr(const void* p) {
    uintptr_t x = (uintptr_t)p;
    x ^= x >> 33;
    x *= (uintptr_t)0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    return (size_t)x;
}
```

**操作复杂度**：
- `table_put`: O(1) 均摊，load factor > 0.75 时翻倍扩容
- `table_find`: O(1) 平均
- `table_erase`: 标记 tombstone，lazy cleanup

**用途**：
- map 时插入，记录 `shmname` / `size` / `t_map`
- unmap 时查找，计算 `lifetime_ns = t_unmap - t_map`
- atexit 时遍历，输出 `live_at_exit`（泄漏检测）

### 3.3 线程安全 — C11 atomic_flag 自旋锁

```c
#ifdef IS_CP
static atomic_flag g_lock = ATOMIC_FLAG_INIT;

static void mm_lock(void) {
    while (atomic_flag_test_and_set_explicit(&g_lock, memory_order_acquire)) {
        /* spin: contention is rare and each critical section is tiny */
    }
}

static void mm_unlock(void) {
    atomic_flag_clear_explicit(&g_lock, memory_order_release);
}
#endif
```

**设计选择**：
- 不用 pthread mutex → 避免 `-lpthread` 依赖
- 自旋锁适用于 **contention 极低**的场景（shm 分配不是高频热点）
- acquire/release memory order 保证临界区内外可见性

### 3.4 DP Backend — 无 libc JSON 格式化

DP 环境没有标准 libc，所以实现了**零依赖的字符串构建器**：

```c
typedef struct {
    char* b;
    unsigned cap;
    unsigned len;
} Buf;

// 手写实现：
// b_raw()  — 原始字节追加
// b_str()  — 字符串追加（手写 strlen）
// b_u64()  — 无符号十进制（手写除法循环）
// b_i64()  — 有符号十进制
// b_hex()  — 指针十六进制（手写移位）
// b_json() — JSON 字符串转义（\ " \n \r \t）
```

**关键**：所有格式化在 **栈上完成**（`char buf[512]`），零堆分配。

---

## 4. 代码重点实现分析

### 4.1 Wrapper 层（src/platform_shm.c）

```c
INT32 Platform_ShmMap(INT32 flags, char* shmname, UINT32 size, void** shm) {
    INT32 ret = ShmMap(flags, shmname, size, shm);   // 先调用 OS
#ifdef MEM_MANAGER_PROFILE
    shm_profiler_on_map("map", shmname,
                        (ret >= 0 && shm) ? *shm : NULL,
                        size, flags, ret, NULL);      // 后记录（不影响行为）
#endif
    return ret;  // 透传原始返回值
}
```

**设计原则**：
1. **先调用 OS，后记录** — profiler 在 OS 调用之后，使用真实返回值和地址
2. **条件编译隔离** — `#ifdef MEM_MANAGER_PROFILE` 包裹所有 profiler 调用
3. **IS_CP 条件编译** — `Platform_ShmMapOnPg` 只在 CP 编译，避免 DP 链接失败

### 4.2 CP File Backend（shm_profiler.c 上半部分）

#### 初始化与路径构建

```c
static void build_path(char* buf, size_t n, int pid) {
    const char* env = getenv("MEM_PROFILE_PATH");
    if (env && env[0]) {
        const char* pc = strstr(env, "%p");
        if (pc) {
            // 支持 %p 占位符替换为 pid
            snprintf(buf, n, "%.*s%d%s", (int)(pc - env), env, pid, pc + 2);
        } else {
            snprintf(buf, n, "%s", env);
        }
    } else {
        snprintf(buf, n, "shm_profile.%d.jsonl", pid);
    }
}
```

#### Map 事件处理

```c
void shm_profiler_on_map(const char* op, const char* shmname, const void* addr,
                         UINT32 size, INT32 flags, INT32 ret, const char* pgname) {
    mm_lock();
    ensure_init();  // lazy init: open file, init hash table, register atexit

    if (t_tid == 0) {
        t_tid = ++g.tid_counter;  // 稳定可移植的 per-thread ID
    }
    int64_t t = now_ns();

    ++g.total_map_calls;
    if (ret < 0) ++g.total_map_failures;

    if (ret >= 0 && addr) {  // ret >= 0 为成功（DP 返回 0，CP 返回引用计数 >0）
        table_put(&g.live, addr, shmname, size, t);
        g.live_bytes += size;
        if (g.live.count > g.peak_count)  g.peak_count = g.live.count;
        if (g.live_bytes > g.peak_bytes)  g.peak_bytes = g.live_bytes;
    }

    // 格式化为 JSONL 并写入文件
    if (g.fd >= 0) {
        char buf[2048];
        LineBuf lb = { buf, sizeof buf, 0 };
        // ... 构建 JSON ...
        mm_write_all(g.fd, lb.b, lb.len);
    }
    mm_unlock();
}
```

#### Unmap 事件处理 — 实时 lifetime 解析

```c
void shm_profiler_on_unmap(const void* addr, INT32 ret) {
    mm_lock();
    ensure_init();
    int64_t t = now_ns();
    ++g.total_unmaps;

    Entry* e = table_find(&g.live, addr);
    if (g.fd >= 0) {
        char buf[2048];
        LineBuf lb = { buf, sizeof buf, 0 };
        // ...
        if (e) {
            g.live_bytes -= e->size;
            lb_fmt(&lb, "...,\"lifetime_ns\":%lld", (long long)(t - e->t_map));
            table_erase(&g.live, e);
        } else {
            ++g.unmatched_unmaps;
            lb_str(&lb, ",\"matched\":false");
        }
        mm_write_all(g.fd, lb.b, lb.len);
    } else if (e) {
        g.live_bytes -= e->size;
        table_erase(&g.live, e);
    } else {
        ++g.unmatched_unmaps;
    }
    mm_unlock();
}
```

#### atexit 清理 — 泄漏检测 + Summary

```c
static void profiler_atexit(void) {
    mm_lock();
    if (g.fd >= 0) {
        int64_t t = now_ns();
        // 1. 输出所有仍 live 的 region（live_at_exit）
        for (size_t i = 0; i < g.live.cap; ++i) {
            if (g.live.slots[i].state == 1) {
                Entry* e = &g.live.slots[i];
                // 输出 {"event":"live_at_exit", ...}
            }
        }
        // 2. 输出 summary
        // {"event":"summary", "peak_live_count":..., "peak_live_bytes":..., ...}
        mm_close(g.fd);
        g.fd = -1;
    }
    mm_unlock();
}
```

### 4.3 DP Log Backend（shm_profiler.c 下半部分）

```c
#ifndef IS_CP
// 无状态：每次调用格式化一个 JSON 行，通过 LOG_FILE_INFO 输出
static atomic_int g_in_log;

void shm_profiler_on_map(...) {
    // 重入保护：如果日志路径内部调用了 ShmMap，防止递归
    if (atomic_exchange_explicit(&g_in_log, 1, memory_order_acquire)) {
        return;  // 嵌套调用 → no-op（同时自动排除日志自身的 shm 分配）
    }
    char buf[512];
    Buf b = { buf, sizeof buf, 0 };
    // 构建 JSON（无 ts_ns, 无 pid — 日志前缀已携带）
    b_str(&b, "{\"event\":\"");
    b_str(&b, op ? op : "map");
    b_str(&b, "\",\"shmname\":\"");
    b_json(&b, shmname);
    // ... addr, size, flags, ret, pgname ...
    b_str(&b, "}");
    b_finish(&b);
    LOG_FILE_INFO(MEM_PROFILE_LOG_MODULE, "%s", b.b);
    atomic_store_explicit(&g_in_log, 0, memory_order_release);
}
#endif
```

**关键设计**：
- **无状态**：无文件句柄、无哈希表、无 init/atexit
- **重入保护**：`atomic_exchange` 检测嵌套调用（日志系统内部可能分配 shm）
- **时间/身份外包**：依赖日志前缀（`[pg:1][vcpu:1][TSC:...]` 或 `[date+us]`）
- **离线关联**：`analyze_profile.py` 按 address 关联 map↔unmap，重建 lifetime

### 4.4 编译时防绕过机制（platform_shm_api.h）

```c
// 业务代码包含此头文件
#include "platform/platform_shm_api.h"

// 1. 暴露 Platform_Shm* 包装器
#include "platform/platform_shm.h"

// 2. 暴露 OS 常量（来自 api.h）
#ifdef MEM_MANAGER_USE_API_H
#include "api.h"
#endif

// 3. POISON 原始调用 — GCC/Clang 编译时检查
#ifdef __GNUC__
#pragma GCC poison ShmMap ShmMapOnPg ShmUnmap
#endif
```

**效果**：任何业务代码写 `ShmMap(...)` 都会编译失败：
```
error: attempt to use poisoned 'ShmMap'
```

**wrapper 实现**（`src/platform_shm.c`）**不**包含 `platform_shm_api.h`，所以它可以正常调用 `ShmMap`。

---

## 5. 离线分析工具（tools/analyze_profile.py）

### 5.1 核心能力

| 分析维度 | 说明 |
|----------|------|
| **Size distribution** | 分配大小分布 + 分位数 (p50/p90/p99/max) |
| **Waste vs 2MB** | 核心 KPI：累积请求大小 vs OS 实际开销（向上取整到 2MB） |
| **Peak simultaneously-live** | 通过 replay map/unmap 重建峰值 live 数量/字节（池大小下界） |
| **Allocation frequency** | 平均速率 + 峰值速率（最忙 1s 窗口） |
| **Lifetime distribution** | 存活时间分位数 + 分桶直方图 |
| **Size × Lifetime cross-tab** | 识别 "small + short-lived" 的池化目标 |
| **Per-pgname / NUMA** | `ShmMapOnPg` 的 NUMA 维度分析 |
| **Flags / VA class** | sharing semantics 分布 + VA-same/VA-differs 汇总 |
| **Never-freed / Leaks** | 退出时仍 live 的 block（长期驻留部分） |
| **Unmatched unmaps** | 双重释放 / 分析前已释放 / 预捕获的 map |

### 5.2 双 Schema 支持

```python
# Schema 1: Pure JSONL（CP file backend / DP dump）
{"ts_ns":..., "event":"map", "shmname":"...", ...}

# Schema 2: Log-wrapped（DP VIA_LOG backend）
[pg:1][vcpu:1][TSC:0117567917671138][INFO][SHMPROF] {"event":"map", ...}
# analyze_profile.py 通过正则提取前缀中的 pg/vcpu/TSC，解析 JSON payload
```

### 5.3 关键算法 — Map/Unmap Replay

```python
def correlate(records):
    by_pid = defaultdict(list)
    for r in records:
        if r.get("event") in ("map", "map_on_pg", "unmap"):
            by_pid[r.get("pid")].append(r)

    for evs in by_pid.values():
        evs.sort(key=lambda r: r.get("ts_ns", 0))
        live = {}  # addr -> (size, t_map, name, flags)
        for r in evs:
            if r["event"] in ("map", "map_on_pg"):
                if ret >= 0 and addr_is_real(addr):
                    live[addr] = (size, ts, name, flags)
            elif r["event"] == "unmap":
                if addr in live:
                    size, t_map, name, flags = live.pop(addr)
                    lifetimes.append((size, ts - t_map, flags))
                else:
                    unmatched += 1
        # 剩余 live 即为 never-freed
```

**为什么需要 replay**：DP backend 只输出原始事件，不解析 lifetime。离线工具通过 address 关联重建，使两种 backend 的报告统一。

---

## 6. 构建系统

```cmake
# Debug builds (default): enable profiling
# Release builds: pure pass-through, shm_profiler.c compiles to empty TU
target_compile_definitions(mem_manager_lib PUBLIC $<$<CONFIG:Debug>:MEM_MANAGER_PROFILE>)

# Real integration: pull types and OS Shm* from business api.h
option(MEM_MANAGER_USE_API_H "Pull types and OS Shm* from the real api.h" OFF)

# Standalone build: use heap stub + local type aliases
if(NOT MEM_MANAGER_USE_API_H)
    list(APPEND MEM_MANAGER_SOURCES src/os/os_shm_stub.c)
endif()
```

| 构建配置 | MEM_MANAGER_PROFILE | MEM_MANAGER_USE_API_H | 效果 |
|----------|---------------------|----------------------|------|
| Debug standalone | ON | OFF | 启用 profiler，使用 heap stub，本地测试 |
| Release standalone | OFF | OFF | 零开销，使用 heap stub |
| Debug integration | ON | ON | 启用 profiler，链接真实 OS API |
| Release integration | OFF | ON | 零开销，链接真实 OS API |

---

## 7. 关键设计决策与权衡

### 7.1 为什么用 C11 而不是 C++？

- **DP 是 C-only 工具链** — 任何 C++ 代码都无法编译
- `extern "C"` 头文件让 C++ CP 可以无缝调用
- C11 的 `atomic_flag`、`_Thread_local`、`<stdatomic.h>` 提供了足够的并发原语

### 7.2 为什么用 atomic_flag 自旋锁而不是 pthread mutex？

- **避免 pthread 依赖** — Release build 理论上可以零外部依赖
- **Contention 极低** — shm 分配不是高频热点，自旋等待成本可忽略
- **DP 环境可能没有 pthread** — 但 C11 atomic 是语言级特性

### 7.3 为什么 DP backend 是无状态的？

- **DP 无 libc** — 不能 `open`/`write` 文件，不能 `malloc` 哈希表
- **DP 是核心绑定** — 单线程 per context，全局 atomic flag 足够
- **OS log 前缀已携带时间/身份** — 不需要在 payload 中重复

### 7.4 为什么 VA-same 和 VA-differs 不能共享池？

- **VA-same**：所有进程看到相同虚拟地址 → 可以在 shm 中存储裸指针
- **VA-differs**：每个进程虚拟地址不同 → 裸指针在另一进程无效 → 必须使用 offset
- 如果混合：VA-same 区域中存储的指针会被 VA-differs 进程错误解释 → **UB / 崩溃**

### 7.5 为什么先调用 OS 再记录？

```
INT32 ret = ShmMap(...);        // 1. 先调用 OS
shm_profiler_on_map(..., ret);  // 2. 使用真实返回值记录
return ret;
```

- 保证 **profiler 故障不影响业务行为**
- 保证 **记录的是真实的地址和返回值**（而不是预测值）
- 如果顺序颠倒，OS 调用失败但 profiler 已记录了一个假成功 → 数据污染

---

## 8. 文件清单

```
mem-manager/
├── CMakeLists.txt                    # 主构建配置
├── docs/
│   ├── DESIGN.md                     # 架构设计文档
│   └── STEP1.md                      # Step 1 详细文档
├── include/platform/
│   ├── platform_shm.h                # 公共 API (extern "C")
│   ├── platform_types.h              # INT32/UINT32 类型定义
│   └── platform_shm_api.h            # 业务 facade（poison 原始 API）
├── src/
│   ├── main.c                        # Demo 程序
│   ├── platform_shm.c                # 三个 wrapper（透传 + profile）
│   ├── os/
│   │   ├── os_shm.h                  # OS Shm* 声明
│   │   └── os_shm_stub.c             # Heap-based fake（独立构建用）
│   └── profiler/
│       ├── shm_profiler.h            # Profiler 公共接口
│       └── shm_profiler.c            # 双后端实现（CP file / DP log）
├── tools/
│   └── analyze_profile.py            # 离线分析工具（Python 3 stdlib）
└── tests/                            # 单元测试
```

---

## 9. Step-2 方向（基于 DESIGN.md）

根据 Step-1 收集的数据，Step-2 池分配器的设计方向：

1. **单一 VA-same 池** — 覆盖 ~99% 的分配，使用裸指针元数据
2. **Per-NUMA 池** — 按 `pgname` 划分，对应 `ShmMapOnPg` 的 NUMA 节点
3. **池大小配置** — 下界为观测到的 `peak_live_bytes`
4. **`shmname → (pool, offset)` 注册表** — 保留基于名称的共享，无需真实 OS 对象
5. **VA-differs 直通** — ~1% 的罕见情况继续走 `ShmMap`（v1）
6. **保持 C-only** — 兼容 DP 工具链

---

*本文档基于 mem-manager 项目 main 分支的实际源码分析生成。*
