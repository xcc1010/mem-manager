# 定位「内存每小时稳定增长」的实战手册

现象:板卡上某业务的内存(`top` 的 RES)每小时稳定 +160MB。本手册把「从看到涨 → 定位到具体函数」的整条路径固化下来。核心两步:**先分类(增长住在哪类内存)→ 再归因(哪个函数在涨)**。

> 用到的工具都在本目录:`mem_trace.c`(LD_PRELOAD 探针)、`analyze_mem_trace.py`(分析器,含 `--baseline` 增长 diff)、`leaktest.c`(自测样本)。原理细节见 `MEM_TRACE.md`。

---

## 为什么不能直接上 mem_trace

`top` 的 RES 只是「当前驻留的物理页总和」,**不区分来源**:堆、匿名 mmap、共享内存被摸过的页、文件映射、线程栈都算。所以先别选工具,**先把这 160MB 落在哪类页上分出来**——不同类根因和修法完全不同,工具也不同。

---

## Step 0 — 分类:增长住在哪类内存?(便宜、无插桩)

取**两个时间点**(间隔 1~2h),对比下面几个数。`PID=$(pidof your_program)`。

```sh
# 1) 匿名 / 文件 / 共享 的 RSS 拆分(内核已分好)
grep -E 'VmRSS|RssAnon|RssFile|RssShmem|VmSize|Threads' /proc/$PID/status

# 2) 整进程汇总(Linux 4.14+),Anonymous 就是匿名驻留总量
grep -E 'Rss|Anon' /proc/$PID/smaps_rollup

# 3) 只看 brk 堆那段的 Rss
grep -A25 '\[heap\]' /proc/$PID/smaps | grep -m1 '^Rss:'
```

按**哪个数在涨**判定方向:

| 观察 | 结论 | 走哪条路 |
|---|---|---|
| `RssShmem` 涨 | 共享内存 | OS shm 日志 / Step-1 profiler(本手册不覆盖) |
| `RssFile` 涨 | 文件映射 / page cache | 查 mmap 的文件、cache 释放策略 |
| `Threads` 涨 | 线程栈累积 | 查线程只建不回收 |
| **`RssAnon` 涨 + `heap Rss` 也涨** | **brk 小块累积** | → Step 1,采样用 **64K** |
| **`RssAnon` 涨 + `heap Rss` 基本平** | **匿名 mmap 大块**(≥128KB) | → Step 1,采样 **64K/512K 均可** |

**先排除「涨 ≠ 泄漏」的假象**:
- `VmRSS` 涨但 **`VmSize` 平** → 更像 glibc 碎片(逻辑已 free、页没还)或懒 fault-in 摸旧页,**不是新申请**,mem_trace 会误导;
- `VmSize` 也涨 → 是真的在新申请不还 → 继续 Step 1;
- `Threads` 不变 → 顺带排除了 glibc per-thread arena 增殖(新 arena 伴随新线程)。

> 判定示例(本手册的真实案例):`RssAnon↑ + VmSize↑ + Threads 平 + heap 平 + Anonymous(mmap)↑` = **匿名 mmap 大块泄漏**,与共享内存无关。

---

## Step 1 — 归因:哪个函数在涨?(mem_trace + 两快照差值)

### 1. 编译(在 aarch64 目标机上最稳)

```sh
gcc -shared -fPIC -O2 -o mem_trace.so mem_trace.c -ldl -lpthread
objdump -T mem_trace.so | grep -oE 'GLIBC_[0-9.]+' | sort -V | tail -1   # 应 <= 运行机 glibc
```

### 2. 采集(带上你的启动方式)

```sh
MEM_TRACE_OUT=/tmp/mt.dump \
MEM_TRACE_SAMPLE=0x10000 \        # 64K:大块必精确、小块累积也能采到;慢泄漏开销无所谓
  LD_PRELOAD=$PWD/mem_trace.so ./your_program ...
```

### 3. 两快照(推荐,排除静态基线干扰)

慢泄漏的关键:业务可能已经有一大坨**稳态**内存,直接看总量会被它淹没。两快照只看**增长**:

```sh
PID=$(pidof your_program)
kill -USR1 $PID                   # 第 1 次信号 -> /tmp/mt.dump.1  (baseline,记下时刻)
# ...等 1~2 小时...
kill -USR1 $PID                   # 第 2 次信号 -> /tmp/mt.dump.2
```

> dump 是收到信号才写,不产生海量日志,程序不用停。第 N 次信号写 `.N`。

### 4. 分析(`--baseline` = 增长 diff)

```sh
python3 analyze_mem_trace.py /tmp/mt.dump.2 \
    --baseline /tmp/mt.dump.1 \
    --hours 1.5 \                 # 两次信号的实际间隔(小时),用来外推 MiB/h
    --resolve --depth 15
```

输出直接把增长摊到函数:

```
# GROWTH by RESIDENT  (dump - baseline)
# total live grew +240.0 MiB  = +160.0 MiB/h   (baseline ... -> ... MiB)

## malloc/new growth by function (ranked by RESIDENT delta)
     +240.0 MiB  = +160.0 MiB/h   | 100.0 -> 340.0 MiB  x1234 samples
    <你的泄漏函数>  (libbiz.so)    <- 就是它

## mmap growth by function (ranked by RESIDENT delta)
    ...
```

**怎么读:**
- 榜首那个持续大涨的栈 = 泄漏点;稳态基线和有借有还的栈会被约掉(delta≈0)自动过滤;
- 走 `malloc(≥128KB→mmap)` 的落在 **malloc/new growth**,直接 `mmap()` 的落在 **mmap growth**,看哪段有大增长;
- `--depth 15`:防止分配都挤在统一入口(自定义 alloc/封装),看到上面真实的业务调用者;
- 排名按 **RESIDENT**(mincore 实测驻留),大块场景精确对得上 `top` 涨的量。

### (备选)单张快照

懒得抓两张:让它跑 2~3h,泄漏无界增长会自己涨到榜首:

```sh
kill -USR1 $PID
python3 analyze_mem_trace.py /tmp/mt.dump --resolve --depth 15
```

---

## 符号解析:把「模块+偏移」翻成函数名

dump 里的帧是 `模块+偏移`。翻成函数名**不需要 `-g`**:

> `-g` 加的是 **DWARF 调试信息**(`file:line` + 内联帧);**函数名来自 ELF 符号表**。
> 只要库/程序**没被 strip**(符号表在),就能翻——只是没行号、内联函数看不到,定位泄漏点足够。
> 先确认:`file ./your_program` 显示 `not stripped`,或 `nm -C ./your_program | head` 有输出。

### 路线一(推荐):运行机上翻,产出 symmap 拷回来

运行机上库就在 dump 记的路径上,直接翻,产出 `frame<TAB>function` 表:

```sh
# ① addr2line 版(addr2line -f 无 -g 时回退符号表拿函数名,多数工具链够用)
sh resolve_dump.sh    /tmp/mt.dump.2 [aarch64-linux-gnu-addr2line] > /tmp/symmap.txt

# ② 纯 nm 版(万一 addr2line 不回退、吐 ??,用它兜底:只查 .symtab/.dynsym,
#    不碰 addr2line/DWARF,busybox awk 也能跑)
sh resolve_dump_nm.sh /tmp/mt.dump.2 [aarch64-linux-gnu-nm aarch64-linux-gnu-readelf] > /tmp/symmap.txt
```

拷回来喂分析器(不再需要库/工具):

```sh
python3 analyze_mem_trace.py /tmp/mt.dump.2 --baseline /tmp/mt.dump.1 \
    --hours 1.5 --symmap /tmp/symmap.txt --depth 15
```

### 路线二:编译机上翻(库被 strip,或想要行号)

用同一次构建的**未 strip / 带 `-g`** 的库翻:

```sh
python3 analyze_mem_trace.py mt.dump.2 --baseline mt.dump.1 --resolve --depth 15 \
    --bin-dir /编译机/biz/lib --bin-dir /编译机/abseil/lib \
    --addr2line aarch64-linux-gnu-addr2line
```

- `--bin-dir DIR`(可多次):按文件名到这些目录找库;`--map OLD=NEW`:路径前缀替换;
- 找不到库会打印 `WARN: cannot locate ...`。

> **无 `-g` 的坑**:被内联进调用者的函数不会单独成帧,你看到的是它的**外层函数名**。若定位太粗,
> 对那个 `.so` 单独带 `-g` 重编一版放编译机,走路线二精确翻(含行号)。

详见 `MEM_TRACE.md` 的「符号解析」。

---

## 先自测一遍(强烈建议)

上真业务前,用 `leaktest.c`(已知泄漏)把整条链在受控场景验通,提前排掉信号/mincore/符号/diff 的坑:

```sh
gcc -shared -fPIC -O2 -g -o mem_trace.so mem_trace.c -ldl -lpthread
gcc -O0 -g -o leaktest leaktest.c

MEM_TRACE_OUT=/tmp/mt.dump MEM_TRACE_SAMPLE=0x10000 \
  LD_PRELOAD=$PWD/mem_trace.so ./leaktest &
PID=$!
sleep 15; kill -USR1 $PID     # -> /tmp/mt.dump.1
sleep 30; kill -USR1 $PID     # -> /tmp/mt.dump.2
kill $PID

python3 analyze_mem_trace.py /tmp/mt.dump.2 --baseline /tmp/mt.dump.1 --resolve --depth 15
```

**通过标准**(泄漏 ~12.8 MiB/s,30s ≈ +380MiB):
- `leak_here` 在 malloc/new growth 榜首,增量 ≈ 12.8×30 MiB;
- `churn` **不出现**(alloc+free 抵消);
- 64MB 的 stable 基线**不出现在 growth**(两张相等被约掉)。

三条都对 → 工具链可信,再上真业务。

---

## 定位到函数之后:怎么修

| 根因 | 判断 | 修法 |
|---|---|---|
| 真泄漏(申请了不 free) | 该函数的分配从无对应释放 | 补 `free`/`ShmUnmap`;查异常路径提前 return 漏了释放 |
| 无界 cache/队列 | 内存还被引用,只是只进不出 | 加容量上限 / LRU 淘汰 / TTL |
| 共享内存 2MB 粒度浪费 | 是 `Platform_ShmMap` 的小块 | 走本项目 Step 2 池分配器(见 `docs/STEP2_DESIGN.zh.md`) |
| glibc 碎片 | VmSize 平、RSS 下不来 | 调 `M_MMAP_THRESHOLD` / `malloc_trim` / 换分配器 |
