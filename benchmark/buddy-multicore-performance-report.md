# Buddy Allocator 多核优化性能报告

## 1. 测试目标

本报告比较 buddy allocator 多核优化前后的性能差异。

本次优化主要面向两类场景：

1. **多核 order-0 小页分配/释放竞争**
   - 通过 per-CPU order-0 cache 减少全局 buddy 锁争用。

2. **COW 引用计数竞争**
   - 通过 `ref_lock[]` 分片锁，把 `kaddref_order()`、`kgetref()`、`ksplit_order()` 和 `kfree_order()` 的引用计数路径从全局 buddy 锁中拆出。

需要注意：本次优化不是为了降低所有单线程路径的指令数。它增加了状态检查、分片锁和 cache 管理逻辑，因此在低竞争或单核场景下可能出现小幅额外开销。

## 2. 测试环境与方法

### 2.1 测试对象

- **Baseline**：当前分支优化前的 HEAD 版本 buddy allocator。
- **Optimized**：加入本次多核优化后的版本。

为了让 `kallocbench` 可在两边运行，Baseline 测试副本中仅额外加入了同一份 `user/kallocbench.c` 和对应 `Makefile` 条目；allocator 实现保持优化前版本。

### 2.2 测试命令

每组都先构建 `fs.img`，再用 QEMU 运行用户态 benchmark：

```sh
make fs.img
make qemu CPUS=1
make qemu CPUS=8
```

在 xv6 shell 中分别运行：

```sh
kallocbench
cowbench
```

### 2.3 测试程序

#### `kallocbench`

`kallocbench` 是本次新增的 allocator 压力测试，包含两段 workload：

1. **small**
   - fork 8 个 worker；
   - 每个 worker 在至少 40 ticks 内循环执行 `sbrk(16 * PGSIZE)`、逐页触碰、再 `sbrk(-16 * PGSIZE)`；
   - 子进程通过 pipe 回传完成轮数，父进程统一统计 `total_rounds`，避免多核串口输出交错；
   - 核心指标是 `ops_per_tick_x100 = total_rounds * 100 / ticks`，数值越高越好。

2. **cow**
   - fork 8 个 worker；
   - 每个 worker 分配并触碰 2MiB 区域；
   - 反复 fork 子进程并写整段区域，触发 COW fault 和物理页释放。

`small` 输出吞吐，越高越好；`cow` 输出总 tick，越低越好。

#### `cowbench`

`cowbench` 是已有 COW benchmark。它比较三种工作集大小下的两类 fork workload：

- **share**：子进程不写共享页，直接退出；
- **dirty**：子进程写每个页，强制触发 COW 复制。

核心指标：

- `rounds`：固定 tick 时间内完成的轮数，越高越好；
- `tpf_x1000`：每轮 fork 的 tick 开销乘以 1000，越低越好。

`cowbench` 更适合观察 COW 语义和大页 dirty 成本，不完全等价于 allocator 锁竞争压力测试，因为它的 share 路径大多是单 parent/child 顺序执行。

## 3. 原始测试结果

### 3.1 `kallocbench`

| Version | CPUS | small total rounds | small ticks | small ops/tick x100 | cow ticks |
| --- | ---: | ---: | ---: | ---: | ---: |
| Baseline | 1 | 6202 | 47 | 13195 | 56 |
| Optimized | 1 | 7983 | 47 | 16985 | 63 |
| Baseline | 8 | 16428 | 41 | 40068 | 25 |
| Optimized | 8 | 20747 | 41 | 50602 | 24 |

### 3.2 `cowbench` CPUS=1

| Version | size KiB | share rounds | share tpf x1000 | dirty rounds | dirty tpf x1000 |
| --- | ---: | ---: | ---: | ---: | ---: |
| Baseline | 64 | 1641 | 18 | 1043 | 28 |
| Optimized | 64 | 1505 | 19 | 1027 | 29 |
| Baseline | 2048 | 1641 | 18 | 30 | 1000 |
| Optimized | 2048 | 1465 | 20 | 30 | 1000 |
| Baseline | 8192 | 1634 | 18 | 13 | 2384 |
| Optimized | 8192 | 1573 | 19 | 12 | 2583 |

### 3.3 `cowbench` CPUS=8

| Version | size KiB | share rounds | share tpf x1000 | dirty rounds | dirty tpf x1000 |
| --- | ---: | ---: | ---: | ---: | ---: |
| Baseline | 64 | 1519 | 19 | 1034 | 29 |
| Optimized | 64 | 1514 | 19 | 1081 | 27 |
| Baseline | 2048 | 1545 | 19 | 23 | 1304 |
| Optimized | 2048 | 1421 | 21 | 30 | 1000 |
| Baseline | 8192 | 1580 | 18 | 9 | 3333 |
| Optimized | 8192 | 1436 | 20 | 9 | 3333 |

## 4. 性能差异分析

### 4.1 `kallocbench small`：用长时间吞吐替代 1-2 tick 计时

| CPUS | Baseline ops/tick x100 | Optimized ops/tick x100 | 吞吐变化 |
| ---: | ---: | ---: | ---: |
| 1 | 13195 | 16985 | +28.7% |
| 8 | 40068 | 50602 | +26.3% |

新版 `small` workload 不再报告 `2 ticks -> 1 tick` 这种过短计时，而是让每个 worker 至少运行 40 ticks，并统计总完成轮数。这样单次测试会累计上万次 `sbrk` 增长/触页/收缩操作，tick 粒度误差显著降低。

结果显示：

- 单核吞吐从 `131.95 ops/tick` 提升到 `169.85 ops/tick`，约 +28.7%；
- 8 核吞吐从 `400.68 ops/tick` 提升到 `506.02 ops/tick`，约 +26.3%。

这更精确地说明 per-CPU order-0 cache 降低了小页分配/释放路径的平均成本。单核也有提升，主要因为 order-0 释放后能直接从本 CPU cache 复用，不必每次进入 buddy split/merge 路径。

### 4.2 `kallocbench cow`：COW 压力下没有改善，略有回退

| CPUS | Baseline ticks | Optimized ticks | 变化 |
| ---: | ---: | ---: | ---: |
| 1 | 56 | 63 | +12.5% |
| 8 | 25 | 24 | -4.0% |

`cow` workload 中，每个 worker 会反复 fork 并写 2MiB 区域。它涉及：

- COW fault；
- 大块 `kalloc_order(SUPERPGORDER)`；
- `memmove()` 复制 2MiB；
- `kfree_order()` 递减旧块引用。

这里的主要成本并不只是 allocator 锁，还包括大块复制和页表操作。本次优化新增了更细的状态检查和分片锁逻辑，因此单核下仍有小幅回退。

8 核下该 workload 从 25 ticks 降到 24 ticks，约改善 4.0%。由于 COW dirty 路径受 2MiB 复制和 tick 粒度影响较大，这个结果只能说明多核下没有出现明显退化；更细粒度的结论需要更多轮次和锁统计支持。

### 4.3 `cowbench share`：顺序 COW fork 场景略慢

`cowbench share` 中子进程不写共享页，主要测试 fork 共享映射和 `kaddref_order()`。

CPUS=8 下：

| size KiB | Baseline share rounds | Optimized share rounds | 变化 |
| ---: | ---: | ---: | ---: |
| 64 | 1519 | 1514 | -0.3% |
| 2048 | 1545 | 1421 | -8.0% |
| 8192 | 1580 | 1436 | -9.1% |

这个测试并不是高度并行的 allocator 竞争场景。它更多体现单个进程顺序 fork/wait 的成本。在这种情况下，优化版的额外状态和锁拆分开销不能被并发收益抵消，因此 2MiB 与 8MiB 档位有约 8%~9% 回退。

### 4.4 `cowbench dirty`：2MiB 多核 dirty 场景改善

CPUS=8 下：

| size KiB | Baseline dirty tpf x1000 | Optimized dirty tpf x1000 | 变化 |
| ---: | ---: | ---: | ---: |
| 64 | 29 | 27 | -6.9% |
| 2048 | 1304 | 1000 | -23.3% |
| 8192 | 3333 | 3333 | 0% |

dirty 模式会触发真实 COW fault 和复制。结果显示：

- 64KiB 有小幅改善；
- 2MiB 档位从 `1304` 降到 `1000`，改善约 23.3%；
- 8MiB 档位无变化。

2MiB 档位改善较明显，可能来自释放路径缩短和 refcount 操作与 buddy 操作解耦；但 8MiB 档位主要受大块复制和 tick 粒度影响，未体现进一步收益。

## 5. 总体结论

### 5.1 明确改善的场景

本次优化最明显改善的是 **order-0 小页分配/释放 churn 的吞吐**：

```text
CPUS=8 kallocbench small: 400.68 ops/tick -> 506.02 ops/tick (+26.3%)
```

这与设计目标一致：per-CPU order-0 cache 减少了普通页分配释放进入全局 buddy 锁的次数。相比原先 `2 ticks -> 1 tick` 的短测试，新结果基于 40 tick 预算内累计 2 万次左右操作，更适合量化差异。

### 5.2 部分 COW dirty 场景改善

CPUS=8 的 `cowbench dirty` 在 2MiB 档位有明显改善：

```text
2048 KiB dirty_tpf_x1000: 1304 -> 1000
```

这说明在实际 COW fault 和释放路径中，把引用计数和释放填充分离出全局锁有一定收益。

### 5.3 低竞争顺序路径存在开销

在单核和顺序 `cowbench share` 场景中，优化版略慢。这是本次设计的主要代价：

- 多状态 `is_free` 检查更严格；
- ref 分片锁替代单一锁后，路径更长；
- per-CPU cache 需要额外的状态迁移；
- 高阶分配失败时需要 drain cache 重试。

因此，本次优化不是“所有场景无条件更快”，而是针对多核下高频小页分配和部分 COW dirty 压力降低锁竞争。

## 6. 正确性验证

性能测试之外，本次改动还通过了以下正确性测试：

```text
make kernel/kernel
make fs.img
./test-xv6.py -q usertests
CPUS=8 ./test-xv6.py cowiso
```

其中 quick usertests 和 8 核 `cowiso` 均输出：

```text
ALL TESTS PASSED
```

同时 `CPUS=8` 下运行新版 `kallocbench` 输出：

```text
kallocbench: workers 8
kallocbench: small_pages 16 small_ticks 40
kallocbench: small total_rounds 20747 ticks 41 ops_per_tick_x100 50602
kallocbench: cow ticks 24
kallocbench: PASS
```

## 7. 测试局限

1. **tick 粒度仍然较粗**
   - 新版 `small` 测试通过 40 tick 长时间吞吐缓解了 `1/2 tick` 问题，但 `cow` 和 `cowbench` 仍受 tick 粒度影响。

2. **单次运行存在波动**
   - QEMU 调度、宿主机负载和 tick 边界都会影响结果。更严格的报告应多次运行取中位数。

3. **`cowbench share` 不是锁竞争压力测试**
   - 它主要测试顺序 fork/wait 下的 COW 成本，无法充分体现多核 allocator 锁拆分收益。

4. **per-CPU cache 可能影响高阶块可用性**
   - 实现中通过高阶分配失败时 drain 所有 cache 缓解，但这仍是设计取舍。

## 8. 后续可改进方向

1. 多次运行 benchmark 并取 median / p90，降低 tick 和 QEMU 波动影响。
2. 增加内核锁统计，记录 `buddy_lock`、`ref_lock[]`、`cache[].lock` 的 spin 次数。
3. 为 `kallocbench` 增加更长时间、更大 worker 数的 order-0 churn 模式，使多核差异更稳定。
4. 评估是否只在 `NCPU > 1` 或高压力路径启用 per-CPU cache，减少单核开销。
5. 进一步研究高阶块只在 head page 记录 `ref/order` 的可行性，以降低 superpage COW metadata 循环成本。

## 9. 相关文件

- 优化实现：`kernel/kalloc.c`
- 新增 benchmark：`user/kallocbench.c`
- benchmark 注册：`Makefile`
- 原理报告：`docs/buddy-multicore-optimization.md`
