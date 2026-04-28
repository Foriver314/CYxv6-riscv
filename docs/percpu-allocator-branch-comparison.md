# percpu-GPT5 与 percpu-deepseek 分支修改方案对比

## 1. 结论摘要

`percpu-GPT5` 的方案更完整，也更适合当前这个已经包含 buddy allocator、superpage 和 COW 的 xv6-riscv 分支。它把 allocator 从单一全局锁重构为 buddy 锁、引用计数分片锁和 per-CPU cache 锁，并把 `kalloc_order(0)` / `kfree_order(0)` 与 `kalloc()` / `kfree()` 统一纳入 order-0 cache 路径。

`percpu-deepseek` 的方案更小、更容易阅读，也提供了方便的 `NO_PERCACHE` 编译开关和 benchmark 脚本。但它只优化 `kalloc()` / `kfree()`，没有覆盖当前 VM 层大量使用的 `kalloc_order(0)` / `kfree_order(0)` 路径；同时它直接在中断可开启的上下文中调用 `mycpu()` 并访问 `struct cpu` 内的 cache，存在 CPU 迁移导致的并发安全风险。

如果要合并一个正确、完整的 per-CPU allocator 优化，建议以 `percpu-GPT5` 为主体；如果需要更好的 A/B 性能对比，可以吸收 `percpu-deepseek` 的 `NO_PERCACHE` 开关和自动化 benchmark 思路。

## 2. 对比对象

两个分支都基于 `riscv` 分支之后的一个提交：

- `percpu-GPT5`: `c82e064 vm: reduce buddy allocator multicore contention`
- `percpu-deepseek`: `a0d704d kalloc: add per-CPU page cache to reduce multi-core lock contention`
- 基线分支: `riscv` / `c133d7d docs of buddy-superpage-cow`

相对 `riscv` 的改动规模：

| 分支 | 文件数 | 插入 | 删除 | 主要新增内容 |
|---|---:|---:|---:|---|
| `percpu-GPT5` | 5 | 1037 | 43 | allocator 重构、`kallocbench`、设计文档、性能报告 |
| `percpu-deepseek` | 8 | 771 | 0 | 简单 per-CPU cache、`NO_PERCACHE`、`kmembench`、benchmark 脚本、设计文档 |

## 3. 方案概览

### 3.1 `percpu-GPT5`

`percpu-GPT5` 把原本由单个 `kmem.lock` 保护的 allocator 状态拆成三类同步对象：

1. `buddy_lock`: 保护全局 buddy freelist、split 和 merge。
2. `ref_lock[REF_NLOCK]`: 按物理页索引分片保护引用计数和页 metadata 检查。
3. `cache[NCPU]`: 每个 CPU 一个 order-0 页缓存，每个 cache 有自己的锁。

该方案还引入了更明确的页状态：

- `PAGE_ALLOCATED`: 已分配。
- `PAGE_FREE`: 在全局 buddy freelist 中。
- `PAGE_FREEING`: 正在释放，尚未进入 buddy/cache。
- `PAGE_CACHE`: 暂存在某个 CPU 的 order-0 cache 中。

核心设计目标是把高频 order-0 分配释放从全局 buddy 锁中移出，同时保持 high-order/superpage 分配的正确性和 COW 引用计数的一致性。

### 3.2 `percpu-deepseek`

`percpu-deepseek` 保留原有 `kmem.lock` 和 buddy 结构，在 `struct cpu` 里新增一个简单数组缓存：

```c
struct {
  void *free[PAGECACHE_SIZE];
  int n;
} pagecache;
```

它重写 `kalloc()` 和 `kfree()`：

- `kalloc()` 优先从当前 CPU 的 `pagecache` 弹出页；为空时批量从全局 buddy refill。
- `kfree()` 在引用计数降到 0 后优先把页放回当前 CPU 的 `pagecache`；cache 满时批量 flush 回 buddy。
- `NO_PERCACHE` 宏可以关闭该优化，便于同一分支下做 A/B 对比。

该方案的优点是实现短、侵入性低、便于教学说明；缺点是只覆盖 `kalloc()` / `kfree()`，没有统一 allocator 的其他 order-0 入口。

## 4. 关键差异

| 维度 | `percpu-GPT5` | `percpu-deepseek` |
|---|---|---|
| allocator 结构 | 拆分 buddy 锁、引用计数锁、cache 锁 | 保留单个全局 `kmem.lock` |
| per-CPU cache 位置 | `kernel/kalloc.c` 内部 `kmem.cache[NCPU]` | `kernel/proc.h` 的 `struct cpu.pagecache` |
| cache 同步 | 每个 cache 有 spinlock，定位 CPU 时使用 `push_off()` | 假设每个 CPU 只访问自己的 cache，未加锁 |
| `kalloc()` / `kfree()` | 通过 `kalloc_order(0)` / `kfree_order(0)` 统一实现 | 单独重写 fast path |
| `kalloc_order(0)` / `kfree_order(0)` | 走 per-CPU cache | 仍走原始全局锁路径 |
| high-order 分配 | 失败时 drain 所有 CPU cache 后重试 | 没有 drain 机制 |
| COW 引用计数 | 引用计数锁分片，减少 COW 路径串行化 | 仍用全局 `kmem.lock` |
| A/B 测试便利性 | 没有编译期开关 | 有 `NO_PERCACHE` 开关 |
| 复杂度 | 高 | 低 |

## 5. 正确性分析

### 5.1 `mycpu()` 使用约束

xv6 中 `mycpu()` 要求调用时中断关闭，否则当前进程可能在读取 CPU id 后被抢占并迁移到另一个 CPU。迁移后继续访问旧 CPU 的 per-CPU 数据，就可能和旧 CPU 上正在运行的新代码并发冲突。

`percpu-GPT5` 的 `lock_current_cache()` 在选择当前 CPU cache 时执行：

1. `push_off()` 关闭中断；
2. 用 `cpuid()` 选择 `kmem.cache[cpuid()]`；
3. 获取该 cache 的锁；
4. `pop_off()` 恢复中断状态。

由于 cache lock 已经持有，即使之后发生调度，其他 CPU 也不会同时修改同一个 cache。

`percpu-deepseek` 在 `kalloc()` / `kfree()` 中直接调用 `mycpu()` 后访问 `c->pagecache`，没有 `push_off()` 保护，也没有 cache lock。因此该实现依赖一个未满足的隐含条件：调用期间不会迁移 CPU。这在 xv6 的普通内核路径中不能作为安全假设。

### 5.2 `kalloc_order(0)` / `kfree_order(0)` 覆盖问题

当前 VM 层并不只调用 `kalloc()` / `kfree()`：

- COW copy 会调用 `kalloc_order(order)`。
- `uvmunmap()` 释放用户页时调用 `kfree_order((void*)pa, page_order_for_level(level))`。
- superpage 优先分配也调用 `kalloc_order(order)`，失败后才回退到 `kalloc()`。

这意味着 `percpu-deepseek` 中大量实际用户页分配/释放仍会走原来的全局 `kmem.lock`，优化覆盖面不足。`percpu-GPT5` 则把 order-0 cache 放在 `kalloc_order()` / `kfree_order()` 层，`kalloc()` / `kfree()` 只是薄封装，因此覆盖面更完整。

### 5.3 high-order / superpage 分配

per-CPU cache 会暂时持有 order-0 空闲页。对普通页分配来说这是好事，但对 buddy allocator 来说，缓存页不在全局 freelist 中，可能阻止相邻页合并成 high-order block，进而影响 superpage 分配。

`percpu-GPT5` 在 high-order 分配失败时 drain 所有 CPU cache，再重试全局 buddy 分配。这不能保证一定成功，但能避免 cache 长期囤积 order-0 页导致的伪碎片化。

`percpu-deepseek` 没有全局 drain 机制。闲置 CPU 的 cache 可能长期持有页，使 buddy 无法合并出较大块。这在当前包含 superpage 支持的分支中是一个明显缺口。

### 5.4 引用计数与 COW

当前分支已经有 COW 和 superpage-aware user mappings。分配器 metadata 不只服务普通页，还承担引用计数检查、COW 共享页释放、superpage split 等职责。

`percpu-GPT5` 将引用计数和 buddy freelist 的锁职责拆开，使不同物理块的 COW 引用计数操作可以并发进行。它仍然保持块内每个 base page 的 `ref/order/is_free` 一致性检查，维护成本较高，但更贴合现有 VM 代码的假设。

`percpu-deepseek` 继续用全局 `kmem.lock` 保护引用计数，因此 COW 路径的锁竞争没有被实质拆分。

## 6. 性能与测试对比

### 6.1 `percpu-GPT5`

`percpu-GPT5` 新增 `user/kallocbench.c`，包含两个 workload：

1. `small`: 多 worker 循环执行 `sbrk(16 * PGSIZE)`、逐页触碰、再 `sbrk(-16 * PGSIZE)`，用固定 tick 预算统计吞吐。
2. `cow`: 多 worker 触发 COW dirty 和复制路径，观察是否退化。

分支报告中显示，`small` workload 在单核和 8 核下都有约 26%–29% 的吞吐提升；COW workload 变化较小，主要说明没有明显退化。

### 6.2 `percpu-deepseek`

`percpu-deepseek` 新增 `user/kmembench.c` 和 `bench-kmembench.py`，并通过 `NO_PERCACHE` 在同一分支内对比开启/关闭缓存的表现。这是该分支比较实用的一点：对比方式更公平，也更容易重复。

其报告结论是：在 xv6 当前规模下，per-CPU cache 对总吞吐没有统计显著提升，只在 4–8 CPU 时略微改善扩展效率。

### 6.3 性能结论

两边 benchmark 的目标不完全一样，不能直接横向比较数值。更合理的判断是：

- `percpu-GPT5` 的实现更像生产方向的正确性优先方案，并展示了较明显的小页分配吞吐提升。
- `percpu-deepseek` 的 benchmark 基础设施更适合做同分支 A/B 测试，但它测到的收益有限，也可能与优化覆盖面不足有关。

## 7. 风险评估

### 7.1 `percpu-GPT5` 风险

1. **实现复杂度高**：新增页状态机、多类锁和 cache drain 路径，未来维护者需要理解更多不变量。
2. **锁顺序需要持续保持一致**：cache lock、ref lock、buddy lock 的组合必须避免死锁。
3. **cache drain 成本**：high-order 分配失败时 drain 所有 CPU cache，会引入一次较重的慢路径操作。
4. **需要更多压力测试**：尤其是 COW、superpage split、并发 `sbrk`、fork/exit 混合 workload。

这些风险主要是复杂度风险，不是明显的设计错误。

### 7.2 `percpu-deepseek` 风险

1. **`mycpu()` 竞态**：未关闭中断、未锁住 cache，存在 CPU 迁移后访问旧 CPU cache 的风险。
2. **优化覆盖面不足**：`kalloc_order(0)` / `kfree_order(0)` 未纳入 cache，VM 热路径仍可能争用全局锁。
3. **high-order 分配受影响**：没有 cache drain，可能降低 buddy 合并出 superpage 的能力。
4. **metadata 状态不够明确**：cached 页仍主要复用 `is_free` 的旧语义，不如显式 `PAGE_CACHE` 状态清晰。
5. **锁竞争改善有限**：COW/refcount 路径仍被全局 `kmem.lock` 串行化。

这些风险包含实际正确性问题，合并前必须修复。

## 8. 合并建议

建议采用组合方案：

1. **以 `percpu-GPT5` 的 allocator 实现为主体**。
   - 保留 `buddy_lock`、`ref_lock[]`、per-CPU cache lock 和显式页状态。
   - 保留 `kalloc_order(0)` / `kfree_order(0)` 的统一 cache 路径。
   - 保留 high-order 分配失败时 drain cache 的机制。

2. **吸收 `percpu-deepseek` 的可测试性设计**。
   - 增加类似 `NO_PERCACHE` 的编译开关，便于同一代码基线上做 A/B 测试。
   - 保留或改造 `bench-kmembench.py` 的自动化思路，避免手动切分支比较。

3. **补充验证**。
   - 运行 quick usertests。
   - 单独跑 COW benchmark、superpage 分配/释放测试、并发 `sbrk` 压测。
   - 在 `CPUS=1/2/4/8` 下分别跑 allocator benchmark，至少取多次运行中位数。

## 9. 最终判断

如果目标是教学演示“per-CPU cache 可以减少全局锁竞争”，`percpu-deepseek` 更短、更直观，但必须先修复 `mycpu()` 使用和 API 覆盖问题。

如果目标是给当前这个带 buddy、superpage、COW 的 xv6 分支合入一个相对完整的优化，`percpu-GPT5` 是更合适的基础方案。它虽然复杂，但设计上覆盖了现有 VM 层的真实调用路径，并显式处理了 per-CPU cache 与 buddy high-order 分配之间的冲突。
