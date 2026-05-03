# 读写锁性能提升报告

## 1. 测试目标

本报告评估本次新增读写锁在不同读写比例下的性能变化。

读写锁允许多个读者同时进入临界区，而写者仍保持独占。因此它最适合读密集场景；当写操作比例升高时，读写锁需要频繁退化为写者独占，收益会下降，甚至可能因为额外读写状态维护和写者优先策略而不如单纯互斥。

本报告分两部分评估：第一部分使用 `user/rwbench.c` 微基准隔离测量读写锁自身相对“全部操作都用写锁串行化”的收益和代价；第二部分在吸收 DeepSeek 思路后，通过同一 benchmark 的 `fsread` 场景观察 inode 普通文件读/stat 路径启用共享读锁后的吞吐。

## 2. Benchmark 设计

### 2.1 测试程序

新增用户态程序：`user/rwbench.c`。

程序运行 6 个 worker 进程，每个 worker 在固定 tick 时间内重复执行同一段受锁保护的共享数组访问 workload，并统计完成轮数。

关键参数：

| 参数 | 值 | 含义 |
| --- | ---: | --- |
| `NWORKER` | 6 | 并发 worker 数 |
| `RUN_TICKS` | 100 | 每组 workload 的测量时长 |
| `WORK_ITERS` | 4096 | 每轮临界区中的读循环次数 |
| `DATA_ITEMS` | 256 | 共享数组大小 |

### 2.2 对照方式

每个 workload 都跑两遍：

1. **exclusive**
   - 所有操作都使用 `rwlock_wrlock()` / `rwlock_unlock()`。
   - 等价于用互斥锁保护全部路径，读者之间不能并发。

2. **rw**
   - 读操作使用 `rwlock_rdlock()`。
   - 写操作使用 `rwlock_wrlock()`。
   - 读者可并发进入临界区，写者仍独占。

### 2.3 Workload 覆盖范围

`rwbench` 以 10 轮为一个周期，通过 `writes_per_ten` 控制写操作比例：

| Workload | `writes_per_ten` | 写操作比例 | 场景 |
| --- | ---: | ---: | --- |
| `readonly` | 0 | 0% | 纯读 |
| `readmostly` | 1 | 10% | 读多写少 |
| `balanced` | 5 | 50% | 读写均衡 |
| `writemostly` | 9 | 90% | 读少写多 |
| `writeonly` | 10 | 100% | 纯写 |

输出核心指标：

- `rounds`：固定时间内完成的总轮数，越高越好；
- `ops_per_tick_x100 = rounds * 100 / ticks`：吞吐量，越高越好；
- `speedup_x100 = rw_ops_per_tick_x100 * 100 / exclusive_ops_per_tick_x100`：rw 相对 exclusive 的吞吐倍数。

### 2.4 文件系统读路径场景

吸收 DeepSeek 的思路后，inode 锁从 `sleeplock` 改为 `rwlock`，但原有 `ilock()` 仍表示独占写锁。当前只让普通文件的 `filestat()` 和 `fileread()` 使用 inode 读锁，并新增 `rwbench: fsread` 场景：

- 创建一个固定大小测试文件；
- 6 个 worker 并发打开并循环执行 `fstat()` + `read()`；
- 统计固定 tick 时间内的完成轮数。

该场景用于观察真实文件系统读路径在共享 inode 读锁下的吞吐变化。由于没有在同一程序内保留旧的 exclusive inode lock 对照，它主要用于和改动前同命令结果或其他分支结果比较。

## 3. 测试命令

先构建内核和文件系统：

```sh
make
make fs.img
```

然后分别在不同 CPU 数下运行：

```sh
make qemu CPUS=1
make qemu          # 默认 CPUS=3
make qemu CPUS=8
```

在 xv6 shell 中执行：

```sh
rwbench
```

## 4. 原始测试结果

### 4.1 CPUS=1

```text
rwbench: workers 6 run_ticks 100 work_iters 4096
rwbench: exclusive uses wrlock for every operation
rwbench: rw uses rdlock for read operations and wrlock for write operations
rwbench: writes_per_ten is the number of write operations in each 10-round cycle
rwbench: higher ops_per_tick_x100 is better
rwbench: readonly   exclusive_rounds 53856 exclusive_ticks 100 exclusive_ops_per_tick_x100 53856 rw_rounds 108882 rw_ticks 100 rw_ops_per_tick_x100 108882 speedup_x100 202
rwbench: readmostly exclusive_rounds 54267 exclusive_ticks 100 exclusive_ops_per_tick_x100 54267 rw_rounds 49859 rw_ticks 100 rw_ops_per_tick_x100 49859 speedup_x100 91
rwbench: balanced   exclusive_rounds 53811 exclusive_ticks 100 exclusive_ops_per_tick_x100 53811 rw_rounds 54221 rw_ticks 100 rw_ops_per_tick_x100 54221 speedup_x100 100
rwbench: writemostly exclusive_rounds 54035 exclusive_ticks 100 exclusive_ops_per_tick_x100 54035 rw_rounds 54522 rw_ticks 100 rw_ops_per_tick_x100 54522 speedup_x100 100
rwbench: writeonly  exclusive_rounds 51355 exclusive_ticks 100 exclusive_ops_per_tick_x100 51355 rw_rounds 53747 rw_ticks 100 rw_ops_per_tick_x100 53747 speedup_x100 104
rwbench: PASS
```

### 4.2 CPUS=3

```text
rwbench: workers 6 run_ticks 100 work_iters 4096
rwbench: exclusive uses wrlock for every operation
rwbench: rw uses rdlock for read operations and wrlock for write operations
rwbench: writes_per_ten is the number of write operations in each 10-round cycle
rwbench: higher ops_per_tick_x100 is better
rwbench: readonly   exclusive_rounds 47694 exclusive_ticks 100 exclusive_ops_per_tick_x100 47694 rw_rounds 220681 rw_ticks 100 rw_ops_per_tick_x100 220681 speedup_x100 462
rwbench: readmostly exclusive_rounds 49609 exclusive_ticks 100 exclusive_ops_per_tick_x100 49609 rw_rounds 54808 rw_ticks 100 rw_ops_per_tick_x100 54808 speedup_x100 110
rwbench: balanced   exclusive_rounds 46946 exclusive_ticks 100 exclusive_ops_per_tick_x100 46946 rw_rounds 46245 rw_ticks 100 rw_ops_per_tick_x100 46245 speedup_x100 98
rwbench: writemostly exclusive_rounds 46293 exclusive_ticks 100 exclusive_ops_per_tick_x100 46293 rw_rounds 47062 rw_ticks 100 rw_ops_per_tick_x100 47062 speedup_x100 101
rwbench: writeonly  exclusive_rounds 48947 exclusive_ticks 100 exclusive_ops_per_tick_x100 48947 rw_rounds 46199 rw_ticks 100 rw_ops_per_tick_x100 46199 speedup_x100 94
rwbench: PASS
```

### 4.3 CPUS=8

```text
rwbench: workers 6 run_ticks 100 work_iters 4096
rwbench: exclusive uses wrlock for every operation
rwbench: rw uses rdlock for read operations and wrlock for write operations
rwbench: writes_per_ten is the number of write operations in each 10-round cycle
rwbench: higher ops_per_tick_x100 is better
rwbench: readonly   exclusive_rounds 40119 exclusive_ticks 100 exclusive_ops_per_tick_x100 40119 rw_rounds 173284 rw_ticks 100 rw_ops_per_tick_x100 173284 speedup_x100 431
rwbench: readmostly exclusive_rounds 40794 exclusive_ticks 100 exclusive_ops_per_tick_x100 40794 rw_rounds 56161 rw_ticks 100 rw_ops_per_tick_x100 56161 speedup_x100 137
rwbench: balanced   exclusive_rounds 38075 exclusive_ticks 100 exclusive_ops_per_tick_x100 38075 rw_rounds 38988 rw_ticks 100 rw_ops_per_tick_x100 38988 speedup_x100 102
rwbench: writemostly exclusive_rounds 39501 exclusive_ticks 100 exclusive_ops_per_tick_x100 39501 rw_rounds 38459 rw_ticks 100 rw_ops_per_tick_x100 38459 speedup_x100 97
rwbench: writeonly  exclusive_rounds 36627 exclusive_ticks 100 exclusive_ops_per_tick_x100 36627 rw_rounds 41051 rw_ticks 100 rw_ops_per_tick_x100 41051 speedup_x100 112
rwbench: PASS
```

## 5. 汇总结果

| CPUS | Workload | 写比例 | exclusive ops/tick x100 | rw ops/tick x100 | 提升倍数 | 吞吐变化 |
| ---: | --- | ---: | ---: | ---: | ---: | ---: |
| 1 | readonly | 0% | 53856 | 108882 | 2.02x | +102% |
| 1 | readmostly | 10% | 54267 | 49859 | 0.91x | -9% |
| 1 | balanced | 50% | 53811 | 54221 | 1.00x | +0% |
| 1 | writemostly | 90% | 54035 | 54522 | 1.00x | +0% |
| 1 | writeonly | 100% | 51355 | 53747 | 1.04x | +4% |
| 3 | readonly | 0% | 47694 | 220681 | 4.62x | +362% |
| 3 | readmostly | 10% | 49609 | 54808 | 1.10x | +10% |
| 3 | balanced | 50% | 46946 | 46245 | 0.98x | -2% |
| 3 | writemostly | 90% | 46293 | 47062 | 1.01x | +1% |
| 3 | writeonly | 100% | 48947 | 46199 | 0.94x | -6% |
| 8 | readonly | 0% | 40119 | 173284 | 4.31x | +331% |
| 8 | readmostly | 10% | 40794 | 56161 | 1.37x | +37% |
| 8 | balanced | 50% | 38075 | 38988 | 1.02x | +2% |
| 8 | writemostly | 90% | 39501 | 38459 | 0.97x | -3% |
| 8 | writeonly | 100% | 36627 | 41051 | 1.12x | +12% |

## 6. 结果分析

### 6.1 纯读场景收益最大

`readonly` workload 中，所有临界区操作都是读操作：

- CPUS=1：约 `2.02x`；
- CPUS=3：约 `4.62x`；
- CPUS=8：约 `4.31x`。

这说明当读操作占绝对多数时，读写锁能有效避免“所有读者都被写锁串行化”的瓶颈。多核下多个 reader 可以同时执行共享读临界区，因此收益比单核更明显。

### 6.2 读多写少场景仍可能受益，但收益不稳定

`readmostly` 中写比例为 10%。多核下仍有读者并发窗口：

- CPUS=3：约 `1.10x`；
- CPUS=8：约 `1.37x`。

但 CPUS=1 下结果为 `0.91x`。单核没有真正的 reader 并行，读写锁还需要维护 reader/writer/waiting_writer 状态，因此在有写者参与时可能不如全写锁对照。

### 6.3 读写均衡场景基本持平

`balanced` 中读写比例为 50%/50%。结果集中在 `0.98x~1.02x`。

这说明当写操作已经很频繁时，读写锁的 reader 并发优势被写者独占抵消，整体性能接近互斥锁。此时是否使用读写锁更应看语义需求，而不是期望明显吞吐提升。

### 6.4 读少写多场景没有明显收益

`writemostly` 中写比例为 90%。结果为：

- CPUS=1：`1.00x`；
- CPUS=3：`1.01x`；
- CPUS=8：`0.97x`。

也就是说读少写多时，读写锁基本退化为互斥锁；在多核下还可能因为写者频繁排队、唤醒和状态维护而略慢。

### 6.5 纯写场景应视为互斥锁等价对照

`writeonly` 中所有操作都使用写锁。理论上 rw 模式和 exclusive 模式都走 `rwlock_wrlock()`，差异主要来自运行时调度波动、QEMU tick 粒度和 worker 启停时机，而不是读写锁并发能力本身。

本次结果在 `0.94x~1.12x` 之间波动，说明纯写场景没有结构性收益。实际应用中，如果某个锁保护的数据几乎总是写操作，读写锁不是合适优化方向。

### 6.6 CPUS=8 纯读没有继续线性增长的原因

`rwbench` 固定为 6 个 worker。CPUS=3 时已经能并行调度多个 reader，CPUS=8 虽然提供更多 hart，但 worker 数没有继续增加，且 xv6/QEMU 的调度、系统调用、pipe 汇总和 tick 粒度都会限制扩展性。因此 CPUS=8 的纯读结果与 CPUS=3 同量级，而不是继续线性增长。

## 7. 结论

新增读写锁的收益与读写比例强相关：

- **纯读/读密集**：收益显著，多核纯读吞吐提升约 `4.31x~4.62x`；
- **10% 写的读多写少**：多核仍有收益，约 `1.10x~1.37x`；
- **50% 写的读写均衡**：基本持平；
- **90% 写的读少写多**：没有明显收益，可能小幅变慢；
- **纯写**：没有结构性收益，只是写锁互斥路径的调度波动。

## 8. DeepSeek 思路吸收后的文件系统读路径

本次进一步把 inode 的保护锁改为 `rwlock`，但只在普通文件 `filestat()` 和 `fileread()` 使用共享读锁，写路径和目录/路径遍历仍保持独占 `ilock()`。这样可以保守地让多个 reader 同时读取同一个已加载 inode，同时避免 lazy inode load、目录修改、链接数更新和日志事务路径的锁语义变化。

`rwbench` 已新增输出。默认 `CPUS=3` 下补充运行结果如下：

```text
rwbench: fsread     workers 6 rounds 8096 ticks 100 ops_per_tick_x100 8096
```

该结果可与改动前或其他分支的同场景结果对比。预期收益主要出现在多进程频繁 `fstat()`/`read()` 同一普通文件的读密集场景；如果 workload 主要受磁盘 buffer cache、进程调度或文件 offset 锁影响，提升可能小于纯内存 rwlock 微基准。

## 9. 结论

因此，读写锁适合用于 xv6 中“读路径频繁、写路径较少”的共享数据结构；对于读少写多或写路径占主导的结构，继续使用普通互斥/自旋/睡眠锁通常更简单，也不会损失可观性能。吸收 DeepSeek 的 inode 读锁思路后，当前实现已经开始把这一优势应用到普通文件读/stat 路径，但仍保留了保守边界，避免无差别替换整个文件系统锁协议。
