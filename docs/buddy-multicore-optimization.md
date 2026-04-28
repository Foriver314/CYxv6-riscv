# xv6 伙伴系统多核优化报告

## 1. 背景

当前分支已经把 xv6 的物理页分配器改造成 binary buddy allocator，并在此基础上支持：

- 4KiB 普通页分配；
- 2MiB superpage 分配与映射；
- COW 所需的物理页引用计数；
- superpage 拆分时的 allocator metadata 降级。

优化前，`kernel/kalloc.c` 使用单个 `kmem.lock` 同时保护所有 allocator 状态：

- buddy freelist 的查找、拆分、合并；
- `pages[]` 中的 `ref`、`order`、`is_free` 元数据；
- COW 的引用计数增加、减少、查询；
- `kfree_order()` 中释放页时的 junk fill。

这在单核环境下实现简单，但在多核环境中会形成明显锁竞争：一个 CPU 正在做 COW 引用计数操作时，会阻塞另一个 CPU 的普通 `kalloc()`；一个 CPU 释放 2MiB superpage 并执行 `memset()` 时，也会长时间持有同一把全局锁。

本次优化目标是：在不改变对外 allocator API、不破坏 buddy 合并能力、不削弱 COW/superpage 语义的前提下，降低多核热点路径上的锁竞争。

## 2. 优化前的主要瓶颈

### 2.1 全局锁串行化所有路径

优化前，`kmem.lock` 是唯一 allocator 锁。以下路径都会争用它：

- `kalloc()` / `kfree()` 普通页分配与释放；
- `kalloc_order()` / `kfree_order()` 高阶块分配与释放；
- `kaddref_order()` fork/COW 共享时增加引用计数；
- `kgetref()` COW fault 时判断是否可以原地转 writable；
- `ksplit_order()` superpage 拆分 metadata；
- buddy split/merge 结构操作。

因此多核下即使不同 CPU 操作完全无关的物理页，也会因为同一把锁互相等待。

### 2.2 COW 引用计数与 buddy 结构操作耦合

COW 的常见路径并不一定需要修改 buddy freelist。例如：

- `uvmcopy()` 只需要对共享物理块执行 `kaddref_order()`；
- `cowcopy()` 先通过 `kgetref()` 读取引用计数；
- 释放共享页时，如果原引用数大于 1，`kfree_order()` 只需要递减引用计数，不需要回收到 freelist。

优化前这些操作仍然持有全局 buddy 锁，导致 COW 密集 workload 与普通物理页分配互相干扰。

### 2.3 释放大块时在锁内执行 `memset()`

优化前 `kfree_order()` 在持有全局锁时执行：

```c
memset(pa, 1, PGSIZE << order);
```

对 order-0 页，这只是 4KiB；但对 `SUPERPGORDER == 9` 的 superpage，这是 2MiB 写入。多核下，一个 CPU 在锁内填充 2MiB 内存期间，其他 CPU 的所有 allocator 操作都无法推进。

### 2.4 order-0 分配/释放频率最高

xv6 中许多路径都会频繁申请 4KiB 页：

- 页表页分配；
- trapframe 分配；
- pipe buffer；
- lazy allocation page fault；
- exec 参数页；
- `freewalk()` 释放页表页。

这些都是 order-0 热点。即使保留全局 buddy 结构锁，也可以通过本地缓存减少进入全局 buddy 的次数。

## 3. 本次改动概览

本次改动集中在 `kernel/kalloc.c`，同时新增用户态 benchmark `user/kallocbench.c`，并在 `Makefile` 中加入 `_kallocbench`。

核心思路有三点：

1. **锁职责拆分**：把原单锁拆成 buddy 结构锁、引用计数分片锁、每 CPU cache 锁。
2. **order-0 per-CPU cache**：普通 4KiB 页优先在本 CPU cache 中分配/释放。
3. **缩短释放路径临界区**：最终释放时先标记状态，再在锁外执行 `memset()`。

## 4. 数据结构变化

### 4.1 新的锁与缓存结构

`kernel/kalloc.c` 中新增：

```c
#define REF_NLOCK 64
#define KCACHE_MAX 32
#define KCACHE_REFILL 16

struct kcache {
  struct spinlock lock;
  struct run *freelist;
  int count;
};

struct {
  struct spinlock buddy_lock;
  struct spinlock ref_lock[REF_NLOCK];
  struct kcache cache[NCPU];
  struct run *freelist[BUDDY_MAX_ORDER + 1];
} kmem;
```

其中：

- `buddy_lock` 只保护全局 buddy freelist 以及 split/merge；
- `ref_lock[]` 按物理页索引分片，保护已分配块的引用计数和 metadata 检查；
- `cache[NCPU]` 是每 CPU 的 order-0 页缓存。

### 4.2 明确页状态

原先 `pages[].is_free` 是布尔值。本次改为多状态：

```c
#define PAGE_ALLOCATED 0
#define PAGE_FREE 1
#define PAGE_FREEING 2
#define PAGE_CACHE 3
```

含义如下：

| 状态 | 含义 |
| --- | --- |
| `PAGE_ALLOCATED` | 已分配，可被有效引用 |
| `PAGE_FREE` | 在全局 buddy freelist 上 |
| `PAGE_FREEING` | 最后引用已释放，正在填充或准备回收 |
| `PAGE_CACHE` | 在某个 CPU 的 order-0 cache 中 |

`free_block()` 只会与 `PAGE_FREE` 的 buddy 合并。这样可以避免把仍在填充、或暂存在 per-CPU cache 中的页错误合并进全局 buddy。

## 5. 锁拆分原理

### 5.1 Buddy 结构锁

全局 buddy freelist 仍然由一把 `buddy_lock` 保护。原因是 buddy allocator 的 split/merge 需要维护全局结构一致性：

- 分配时可能从高阶 freelist 取块并拆成多个低阶 buddy；
- 释放时可能递归查找并合并 buddy；
- 高阶块，尤其 2MiB superpage，仍需要全局对齐和连续性保证。

因此，本次没有把 buddy 本身完全分区化，而是减少进入 buddy 锁的次数，并缩短持锁时间。

### 5.2 引用计数分片锁

COW 引用计数不需要修改 buddy freelist。现在：

- `kaddref_order()` 使用 `page_ref_lock(idx)`；
- `kgetref()` 使用 `page_ref_lock(idx)`；
- `ksplit_order()` 使用 `page_ref_lock(idx)`；
- `kfree_order()` 的引用计数递减阶段使用 `page_ref_lock(idx)`。

`page_ref_lock(idx)` 通过 `idx % REF_NLOCK` 选择锁。这样不同物理块的 COW 操作可以并发执行，不再被单个 `kmem.lock` 串行化。

### 5.3 Per-CPU cache 锁

order-0 分配先尝试当前 CPU cache：

1. `kalloc_order(0)` 调用 `cache_pop()`；
2. cache 命中时只需要拿当前 CPU cache 锁和对应 ref 分片锁；
3. cache 未命中时才从全局 buddy 批量 refill。

释放 order-0 页时：

1. `kfree_order()` 在 ref 分片锁下把引用计数降到 0；
2. 在锁外执行 junk fill；
3. 若当前 CPU cache 未满，则放入 cache；
4. cache 满时才进入全局 buddy 合并。

这使常见小页 churn 大部分停留在本 CPU 的短链表上，减少全局 buddy 锁争用。

## 6. 关键路径变化

### 6.1 `kalloc_order()`

优化后：

- order-0：先从 per-CPU cache 取页；cache 空时从 buddy 批量补充；仍失败则 drain 所有 CPU cache 后重试全局 buddy。
- 高阶 order：直接走全局 buddy；失败时 drain 所有 CPU cache 后重试，以减少 cache 囤积 order-0 页导致高阶分配失败的风险。

内存 junk fill 仍然在 allocator 锁外执行。

### 6.2 `kfree_order()`

优化后释放流程：

1. 参数与范围检查；
2. 获取对应 ref 分片锁；
3. 检查块内 base page 的 `ref/order/is_free` 一致性；
4. 递减引用计数；
5. 若原 `ref > 1`，直接释放 ref 锁返回，不触碰 buddy；
6. 若是最后引用，把块标记为 `PAGE_FREEING`；
7. 释放 ref 锁；
8. 在锁外执行 `memset(pa, 1, size)`；
9. order-0 优先进入 CPU cache，高阶块回到全局 buddy。

这个流程把共享页释放路径从“全局锁 + 可能大块填充”缩短为“只拿分片 ref 锁递减引用计数”。

### 6.3 `kaddref_order()` / `kgetref()` / `ksplit_order()`

这些函数不再获取 `buddy_lock`。它们只操作已分配块的 metadata，因此使用 ref 分片锁即可。

这对 fork/COW 路径尤其重要：`uvmcopy()` 中的 `kaddref_order()` 不再阻塞其他 CPU 的 buddy 分配；COW fault 中的 `kgetref()` 也不再与全局分配器互斥。

## 7. 正确性约束

### 7.1 Buddy 只合并全局 free-list 页

`free_block()` 只接受 `PAGE_FREE` buddy。`PAGE_CACHE` 页虽然物理上空闲，但仍属于某个 CPU cache，不能被全局 buddy 合并。`PAGE_FREEING` 页也不能合并，因为它可能还在执行 junk fill。

### 7.2 高阶分配失败时 drain cache

Per-CPU cache 会暂时持有 order-0 页，可能降低全局 buddy 合并出高阶块的机会。为减轻这个问题，高阶分配失败时会 drain 所有 CPU cache，再重试全局 buddy。

这保留了 per-CPU cache 的性能收益，同时避免长期牺牲 superpage 分配能力。

### 7.3 COW 仍要求块内 metadata 一致

对高阶块，`kaddref_order()`、`kfree_order()`、`ksplit_order()` 仍会检查覆盖范围内所有 base page 的：

- `usable`；
- `is_free`；
- `order`；
- `ref`。

这延续了原实现的严格校验，避免 superpage/COW metadata 悄悄失真。

### 7.4 `ksplit_order()` 仍要求私有块

`ksplit_order()` 仍要求 `ref == 1`。共享 superpage 必须先经 COW 变成私有物理块，才能拆分为 order-0 metadata。

## 8. 与 VM 层的关系

本次没有修改 allocator 对外 API，因此 `kernel/vm.c` 的调用逻辑保持不变：

- `uvmalloc()` 仍通过 `kalloc_order(SUPERPGORDER)` 尝试分配 2MiB superpage；
- `uvmunmap()` 仍通过 `kfree_order()` 释放普通页或 superpage；
- `uvmcopy()` 仍通过 `kaddref_order()` 增加 COW 引用；
- `cowcopy()` 仍通过 `kgetref()` 判断是否可以原地恢复写权限；
- `split_superpage()` 仍通过 `ksplit_order()` 降级 allocator metadata。

也就是说，本次优化只改变 allocator 内部同步策略，不改变 VM 层语义。

## 9. 本次实现的取舍

### 9.1 为什么不做完整 per-CPU buddy

完整 per-CPU buddy allocator 可以进一步降低锁竞争，但会引入更复杂的问题：

- 不同 CPU 的空闲页如何跨 CPU 合并；
- 2MiB superpage 所需连续物理块如何保证；
- 内存碎片如何控制；
- COW 引用计数如何与多个局部 buddy 协作。

对 xv6 教学内核而言，这会显著增加复杂度。本次选择只缓存最热的 order-0 页，高阶块仍由全局 buddy 管理。

### 9.2 为什么保留块内逐页 metadata

可以进一步优化为只在块头记录 `ref/order`，减少 superpage 引用计数操作时的循环。但当前 VM 和调试校验依赖每个 base page 都有一致 metadata，保留它能降低正确性风险。

因此，本次优化优先降低锁竞争，而不改变 metadata 表达方式。

## 10. 验证结果

本次改动后已通过：

```text
make kernel/kernel
make fs.img
./test-xv6.py -q usertests
CPUS=8 ./test-xv6.py cowiso
```

并在 `CPUS=8` 下运行：

```text
kallocbench
cowbench
```

具体性能数据见 `benchmark/buddy-multicore-performance-report.md`。

## 11. 相关文件

- 主要实现：`kernel/kalloc.c`
- VM 调用方：`kernel/vm.c`
- 新增 benchmark：`user/kallocbench.c`
- 用户程序列表：`Makefile`
- 性能报告：`benchmark/buddy-multicore-performance-report.md`
