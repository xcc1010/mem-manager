# mem_trace — 把进程内存按「函数」拆开（malloc + mmap 全覆盖）

进程在 `top`/`ps` 里占了比如 ~6G，要回答：**这 6G 是哪些函数 / 业务调用栈分配并长期持有的？是大量小块累积，还是少数大块？**

## 为什么用它（而不是 tcmalloc / valgrind / heaptrack）

| 你的处境 | 别的工具 | mem_trace |
|---|---|---|
| 程序自带/混合分配器 | tcmalloc `free invalid pointer` 直接崩 | **不替换**分配器，只包装并转交真 malloc/free → **不可能崩** |
| valgrind 太慢，跑不完初始化 | 20–50x 减速 | **按字节采样**，几个百分点开销，能跑到稳态 |
| 不知是「小块累积」还是「大块」 | 硬阈值会漏掉小块累积 | **字节采样对两种都正确归因** |
| 运行机老（glibc 2.34 / libstdc++ 6.0.24） | 新工具常需新库 | **纯 C、不依赖 libstdc++**，C++ `new` 走 malloc 自动覆盖 |
| 没有 root / eBPF | 多数内核侧工具要 | 只需能 `LD_PRELOAD` |

## 一、编译

在 **aarch64 目标机**上编译最稳：
```sh
gcc -shared -fPIC -O2 -o mem_trace.so mem_trace.c -ldl -lpthread
```
若只能在**新编译机**上编，编完务必自检符号版本不超过运行机的 glibc 2.34：
```sh
objdump -T mem_trace.so | grep -oE 'GLIBC_[0-9.]+' | sort -V | tail -1   # 应 <= 2.34
```
> 想让调用栈翻成**函数名**，被测程序最好带符号（`-g` 或有独立 debug 符号），
> 否则只能给到 `模块+偏移`（仍可后续用 `addr2line` 翻）。

## 二、采集

```sh
MEM_TRACE_OUT=/tmp/memtrace.dump \
MEM_TRACE_SAMPLE=0x80000 \              # 字节采样间隔 512KiB；调小=更准但更慢
LD_PRELOAD=$PWD/mem_trace.so ./your_program ...
```
让程序跑到内存稳态（`top` 到那 ~6G）。**dump 是收到信号 / 退出时才写**，不产生海量日志。

在稳态对**同一时刻**抓快照（不用停程序）：
```sh
PID=$(pidof your_program)
kill -USR1 $PID                         # 触发一次 dump
cp /tmp/memtrace.dump /tmp/snap.dump
cat /proc/$PID/smaps > /tmp/smaps.txt   # 给 mmap 侧做真实 RSS 对接
```

## 三、分析

```sh
# 1) 哪些函数占内存最多（malloc 用采样估计，mmap 精确）
python3 analyze_mem_trace.py /tmp/snap.dump --resolve

# 2) 到底是「小块累积」还是「大块」——看 live malloc 的大小分布
python3 analyze_mem_trace.py /tmp/snap.dump --hist

# 3) 把 top 看到的真实 RSS（mmap 侧）按函数拆开
python3 analyze_mem_trace.py /tmp/snap.dump --smaps /tmp/smaps.txt --resolve
```
输出形如：
```
## malloc/new live by function (sampled estimate)
   2310.0 MiB  x4512 samples
    dma_pool_alloc   (libsim.so)
    device_init      (libsim.so)
    ...
## RSS breakdown by function (smaps RSS x mmap stacks)
# total RSS 6021.4 | attributed 5380.2 | UNATTRIBUTED 641.2 MiB
   3120.5 MiB (52%)
    guest_ram_map    (sim)
    ...
```
`UNATTRIBUTED` = 没对接上的 RSS（glibc 小对象 arena / brk / 库 / 栈），让你看出「6G 解释了多少」。

## 环境变量

| 变量 | 默认 | 说明 |
|---|---|---|
| `MEM_TRACE_OUT` | `/tmp/mem_trace.dump` | dump 输出（第 N 次信号写 `.N`） |
| `MEM_TRACE_SAMPLE` | `0x80000` (512K) | malloc 字节采样间隔；≥此值的分配必记且精确 |
| `MEM_TRACE_MMAP_MIN` | `0x1000` (4K) | 忽略小于此的 mmap |
| `MEM_TRACE_SIG` | `10` (SIGUSR1) | dump 触发信号；若程序自己用了 USR1，改成别的 |

## 原理（一句话版）

LD_PRELOAD 抢在真 libc 前面接管 `malloc/free/mmap/...`，每次分配**按字节采样**抓一次调用栈、记进进程内活跃表，`free` 时移除——所以表里始终是**当前还活着的内存**；收到信号时把活跃表按调用栈 dump 出来，`addr2line` 翻成函数名。malloc 侧用采样保证低开销且对「小块累积/大块」都正确；mmap 侧全记并可用 smaps 对接真实 RSS。详见 `mem_trace.c` 顶部注释。

## 喂回 mem-manager Step 2

- 长期持有的大块 → 适合 `bump`（最省、可无锁）；反复 map/unmap → `slab`（复用槽）。
- 各调用栈的**峰值同时存活字节** = 把「28.8G → 多少 G」算成确定值所缺的那个数。
