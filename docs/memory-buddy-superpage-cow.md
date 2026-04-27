# xv6 内存管理改造总结：伙伴系统、大页与 COW

## 1. 背景与设计目标

当前分支对 xv6 的用户态内存管理做了三项彼此关联的改造：

1. 用 **binary buddy allocator** 取代只能按 4KiB 单页分配的简单接口；
2. 在用户页表中支持 **2MiB superpage** 映射；
3. 在 `fork()` 路径上引入 **copy-on-write (COW)**，并要求它与伙伴系统和大页机制兼容。

这三项改造不是彼此独立的功能堆叠，而是一条逐层展开的实现链：

- superpage 需要底层分配器能一次提供按 2MiB 对齐的连续物理块；
- COW 需要底层分配器能追踪共享物理页的引用计数；
- 当共享对象是 superpage 时，COW 又必须处理“大页共享、按需私有化、必要时拆分”的问题。

因此，这个分支的核心目标不是单纯“把页变大”或“把 `fork()` 变快”，而是构建一套能够同时支持：

- 普通 4KiB 页分配；
- 2MiB 用户态映射；
- `fork()` 后共享物理页；
- 写时复制与内核态 `copyout()` 私有化；
- lazy `sbrk` 缺页分配；

并且保证这些路径之间不会互相破坏。

## 2. 伙伴系统：为大页与共享提供底座

### 2.1 为什么要从单页分配器升级

原始 xv6 的物理页分配以 4KiB 为粒度，适合普通页表和内核对象分配，但不适合下面两类需求：

- **大页映射**：要建立 2MiB superpage，必须拿到一个 2MiB 对齐、连续的物理块；
- **COW**：父子进程共享同一物理页后，必须直到最后一个映射释放时才能真正回收物理内存。

所以这个分支把物理分配器改造成了 binary buddy allocator，并在每个 base page 上记录元数据。

### 2.2 核心数据结构与接口

`kernel/kalloc.c:24` 定义了 freelist 节点 `struct run`，`kernel/kalloc.c:28` 定义了页面元数据 `struct page`。其中：

- `ref`：该物理块当前引用计数；
- `order`：当前块大小对应的阶；
- `is_free`：块是否在 freelist 中；
- `usable`：该物理页是否可被分配器管理。

分配器主体在 `kernel/kalloc.c:35`，为每个 order 维护一条 freelist。核心接口包括：

- `kalloc_order()` / `kfree_order()`：按阶分配和释放物理块，见 `kernel/kalloc.c:179`、`kernel/kalloc.c:205`；
- `kaddref_order()` / `kgetref()`：对共享块增加引用计数、查询引用计数，见 `kernel/kalloc.c:245`、`kernel/kalloc.c:322`；
- `ksplit_order()`：把一个私有高阶块的 allocator metadata 降级成更小块，见 `kernel/kalloc.c:279`；
- `kalloc()` / `kfree()`：保留传统 4KiB 单页接口，内部只是对 order-0 的包装，见 `kernel/kalloc.c:337`、`kernel/kalloc.c:346`。

### 2.3 分裂、合并与引用计数

伙伴系统的基本策略是：

- 申请时，若目标 order 没有空闲块，就从更高阶 freelist 取块并逐层拆分，逻辑在 `alloc_block()`，见 `kernel/kalloc.c:121`；
- 释放时，若 buddy 块同样空闲且同阶，就递归合并，逻辑在 `free_block()`，见 `kernel/kalloc.c:101`。

与普通 buddy allocator 不同，这里还引入了 **引用计数语义**：

- `kalloc_order()` 分配成功后，会把整个块覆盖到的 base page 的 `ref` 初始化为 1，见 `kernel/kalloc.c:186-201`；
- `kfree_order()` 并不是直接回收，而是先将块内所有 base page 的 `ref` 递减；只有原 `ref == 1` 时才真正进入 freelist，见 `kernel/kalloc.c:222-239`；
- `kaddref_order()` 用于在 COW 场景下把一整块共享页的引用数整体增加，见 `kernel/kalloc.c:262-272`。

这意味着分配器不只是“给页”和“收页”，还承担了 **共享对象生命周期管理** 的职责。

### 2.4 为什么 `ksplit_order()` 很关键

`ksplit_order()` 的存在说明：**物理块的共享状态与映射粒度并不总是一致**。

当一个 2MiB superpage 映射需要降级成 512 个 4KiB 映射时，页表层面只要重建下一级页表即可；但 allocator metadata 也必须同步从 `SUPERPGORDER` 改成 `0`，否则后续对其中任意 4KiB 页的引用计数和回收都会失真。

同时，`ksplit_order()` 明确要求 `ref == 1`，否则直接 panic，见 `kernel/kalloc.c:301-303`。这条约束非常重要：

- **共享的 superpage 不能直接拆成小页继续共用 allocator metadata**；
- 如果想拆分共享大页，必须先通过 COW 把它变成当前进程私有的物理块，再做 metadata 降级。

这条规则正是后面 superpage 与 COW 能安全协作的基础。

## 3. superpage：在用户空间引入 2MiB 映射

### 3.1 关键常量与基本思路

`kernel/riscv.h:354-355` 定义了：

- `SUPERPGSIZE = 1 << 21`，即 2MiB；
- `SUPERPGORDER = 9`，表示一个 superpage 覆盖 `2^9` 个 4KiB base page。

由于 Sv39 的 level-1 leaf PTE 正好对应 2MiB 映射，这个分支选择：

- 在分配阶段尽可能申请 order-9 物理块；
- 在建图阶段直接把该物理块映射成 level-1 leaf；
- 仅在必须做细粒度操作时才拆回 4KiB 页映射。

### 3.2 页表遍历为何要识别不同 leaf level

原始 xv6 主要围绕 4KiB leaf 工作，而 superpage 使“叶子节点不一定在 level-0”。

因此 `kernel/vm.c:148` 增加了 `walk_leaf()`：

- 它沿页表向下走，遇到任一有效 leaf 就返回；
- 同时把 leaf 所在 level 返回给调用者。

对应地，`walkaddr()` 也不再默认页大小为 4KiB，而是根据 level 计算实际映射粒度，见 `kernel/vm.c:174-194`。这使得：

- 普通页和 superpage 可以共用同一套地址解析逻辑；
- 后续 `copyout()`、`uvmcopy()`、`uvmunmap()`、`vmfault()` 都可以在一个接口上识别当前映射是 4KiB 还是 2MiB。

### 3.3 `mappages_order()`：把“映射粒度”显式化

`kernel/vm.c:208` 的 `mappages_order()` 是 superpage 支持的核心接口。它把“映射一个区间”进一步参数化为：

- 虚拟地址 `va`；
- 物理地址 `pa`；
- 权限位 `perm`；
- 分配阶 `order`。

对 order-0，它行为等价于原先的普通页映射；对 `SUPERPGORDER`，它会：

- 要求 `va`、`pa`、`size` 都按 2MiB 对齐，见 `kernel/vm.c:216-229`；
- 直接在 level-1 写入 leaf PTE，见 `kernel/vm.c:236-247`。

这一步把“页表映射粒度”和“物理块大小”统一起来，使 superpage 不再是普通页的特殊 case patch，而是 VM 接口的一等能力。

### 3.4 `uvmalloc()` 如何优先选择大页

`kernel/vm.c:401` 的 `uvmalloc()` 在扩展用户空间时，会优先检查当前虚拟地址是否满足：

- `a` 按 `SUPERPGSIZE` 对齐；
- 剩余待分配区间至少还有一个 `SUPERPGSIZE`。

满足时，它会先尝试 `kalloc_order(SUPERPGORDER)`，见 `kernel/vm.c:413-417`；若失败，再回退到普通 4KiB 页，见 `kernel/vm.c:417-425`。随后通过 `mappages_order()` 建图，见 `kernel/vm.c:432-433`。

这样设计的好处是：

- 大范围、对齐良好的用户地址空间会自然形成 superpage；
- 物理内存碎片或高阶块不足时，不会阻塞进程继续增长，只是退回小页映射；
- 上层 `growproc()` 和 `exec()` 不需要知道底层到底用了普通页还是 superpage。

### 3.5 为什么需要 `split_superpage()`

superpage 提高了映射效率，但会让某些“只改动一部分页”的操作变得困难。例如：

- 只取消 superpage 中一段子区间的映射；
- 为用户栈 guard page 清除一个 4KiB 页的 `PTE_U`；
- 后续若要对 superpage 中单个 4KiB 区域做更细粒度管理。

`kernel/vm.c:251` 的 `split_superpage()` 就负责把一个 level-1 leaf 变成下一级页表：

1. 为新的 level-0 页表分配一页页表内存，见 `kernel/vm.c:265-268`；
2. 把原 superpage 覆盖的 512 个 4KiB 子页逐个写成 child PTE，见 `kernel/vm.c:270-273`；
3. 调用 `ksplit_order()` 把 allocator metadata 从大块降级成 order-0，见 `kernel/vm.c:275`；
4. 用指向 child pagetable 的非 leaf PTE 替换原 level-1 leaf，见 `kernel/vm.c:276-277`。

因此，superpage 的设计不是“一旦建成就永不拆分”，而是：

- **能整块映射时尽量整块映射；**
- **需要细粒度操作时再精确降级。**

## 4. COW：把 `fork()` 从立即复制改成写时复制

### 4.1 目标

传统 `fork()` 的主要代价是把父进程全部用户页立即复制到子进程。对于很多 `fork()+exec()` 或“子进程几乎不写继承内存”的场景，这部分复制是浪费的。

COW 的目标是：

- `fork()` 时不复制物理页，只复制页表映射；
- 父子进程共享同一批只读映射；
- 当某一方第一次写入共享页时，再单独为该页分配新物理页并复制内容。

### 4.2 页表标志与 `uvmcopy()`

`kernel/riscv.h:367` 定义了 `PTE_COW`。它表示该映射当前：

- 不是普通可写页；
- 而是“共享只读，但写入时可以私有化”的页。

`kernel/proc.c:260-277` 中的 `kfork()` 仍然调用 `uvmcopy()` 完成地址空间复制，但 `kernel/vm.c:496` 的 `uvmcopy()` 已经改变语义：

- 它遍历父进程用户地址空间中的 leaf；
- 若当前映射可写，则去掉 `PTE_W`，加上 `PTE_COW`，并回写父进程自己的 PTE，见 `kernel/vm.c:521-525`；
- 再把同一物理页以相同 flags 映射到子进程，见 `kernel/vm.c:526-528`；
- 同时通过 `kaddref_order()` 增加共享块的引用计数，见 `kernel/vm.c:528`。

因此，`fork()` 完成后父子进程并不各自持有独立副本，而是共享同一批物理页，只是这些页现在变成了 COW 映射。

值得注意的是，`uvmcopy()` 并不假设所有映射都是 4KiB 页，而是通过 `walk_leaf()` 和 `page_size_for_level()` 逐 leaf 前进，见 `kernel/vm.c:505-516`。这就是它能同时支持普通页和 superpage COW 的关键。

### 4.3 `cowcopy()`：真正的写时复制动作

`kernel/vm.c:282` 的 `cowcopy()` 负责在“写共享页”时把 COW 映射变成私有可写映射。

它的处理分为两种情况：

1. **引用计数已经是 1**
   - 说明当前虽然是 COW 映射，但实际已无其他共享者；
   - 这时无需重新分配物理页，只要恢复 `PTE_W` 并清掉 `PTE_COW`，见 `kernel/vm.c:298-301`。

2. **引用计数大于 1**
   - 说明仍有其他进程共享这块物理页；
   - 于是按当前 leaf 的 order 重新分配同尺寸物理块，见 `kernel/vm.c:304-307`；
   - 把旧内容整块复制过去，见 `kernel/vm.c:308`；
   - 用新块替换当前 PTE，并恢复可写，见 `kernel/vm.c:310-312`；
   - 对旧共享块执行一次 `kfree_order()`，本质上是把当前映射从共享集合中退出，见 `kernel/vm.c:313`。

这里的一个关键点是：**COW 拷贝的粒度跟当前 leaf 粒度一致**。如果当前映射还是一个 superpage leaf，那么复制的就是整个 2MiB 物理块，而不是默认退化成 4KiB 页复制。

### 4.4 `vmfault()`：统一的页错误入口

`kernel/trap.c:71-73` 显示，用户态读/写缺页都会进入 `vmfault()`。`kernel/vm.c:668` 的 `vmfault()` 因此同时承载两种语义：

- **lazy allocation**：地址合法但尚未建立物理页时，分配一个新的 4KiB 页并映射，见 `kernel/vm.c:686-694`；
- **COW write fault**：地址已经映射，但 leaf 含 `PTE_COW` 且当前是写 fault 时，调用 `cowcopy()`，见 `kernel/vm.c:679-683`。

这意味着 lazy `sbrk` 与 COW 走的是同一 trap 入口，但处理原因不同：

- lazy `sbrk` 是“本来承诺了这段虚拟地址，但物理页还没兑现”；
- COW 是“物理页已经存在，但当前写者需要独占副本”。

### 4.5 `copyout()` 也必须尊重 COW

COW 不只影响用户态指令写内存，也影响内核向用户态写数据的路径。

`kernel/vm.c:563` 的 `copyout()` 在找到目标物理地址后，并不会直接 `memmove()`，而是先调用 `cowensure()`，见 `kernel/vm.c:578`。`cowensure()` 的作用是：

- 如果目标页已经可写，直接返回；
- 如果目标页是 COW 映射，则先执行 `cowcopy()`，确保当前页表看到的是私有可写页，见 `kernel/vm.c:318-336`。

这一步非常重要，因为很多内核操作都会向用户空间写数据，例如：

- 系统调用返回字符串或结构体；
- `exec()` 向新用户栈拷贝参数；
- pipe/read 等把数据写回用户缓冲区。

如果 `copyout()` 绕过 COW，那么内核一次写回就会偷偷改掉多个进程共享的物理页，直接破坏隔离性。

## 5. 三者如何协同工作

这一节是整个实现的关键：伙伴系统、superpage 和 COW 不是三条平行改动，而是共享一组约束。

### 5.1 superpage 建立在高阶块分配之上

superpage 不是单独的物理内存来源，它完全依赖 buddy allocator：

- `uvmalloc()` 先决定是否值得尝试 2MiB 映射，见 `kernel/vm.c:413-417`；
- 真正的物理块来自 `kalloc_order(SUPERPGORDER)`；
- 页表层用 `mappages_order()` 把这个高阶块映射成 level-1 leaf。

所以“大页支持”本质上是：**把 allocator 的高阶能力投射到了页表映射层**。

### 5.2 COW 建立在 block 级引用计数之上

COW 的核心不是“页表改只读”本身，而是“共享物理对象的生命周期能被正确维护”。这正是 `kaddref_order()` / `kfree_order()` 提供的能力：

- `fork()` 后，父子对同一物理块共享引用；
- 任一方退出共享时，引用计数递减；
- 直到最后一个映射离开，该物理块才会真正回到 freelist。

由于引用计数是按 block 语义维护的，COW 天然支持：

- 普通 4KiB 页共享；
- 2MiB superpage 共享。

### 5.3 共享 superpage 不能随意拆分

共享 superpage 是三者交互最敏感的场景之一。

如果一个 level-1 leaf 既是 superpage，又处于 COW 共享状态，那么不能简单地执行 `split_superpage()`，因为 `ksplit_order()` 要求 `ref == 1`。这也是为什么 `uvmunmap()` 在处理“部分 unmap 一个 superpage”时，会先检查当前是否是 `PTE_COW`：

- 如果是共享 COW superpage，先 `cowcopy()`，把当前进程得到一个私有副本，见 `kernel/vm.c:379-385`；
- 然后再 `split_superpage()`，把私有的 2MiB 块拆成 512 个 4KiB 页映射。

这个顺序体现了一个核心原则：

> 共享对象的语义必须先由 COW 解开，再把映射粒度从“大块”降到“小块”。

### 5.4 局部操作会触发 superpage 降级

有些操作本质上只针对 superpage 内的一小部分内容，因此必须从大页降级：

- `uvmclear()` 只想清掉某个 4KiB guard page 的 `PTE_U`；若当前落在 superpage 上，就先拆分，见 `kernel/vm.c:541-556`；
- `uvmunmap()` 若只解除 superpage 的一部分映射，同样需要拆分后再逐页处理，见 `kernel/vm.c:377-387`。

这说明 superpage 的存在是一个“尽量使用的大粒度优化”，但 VM 子系统仍然保留了在必要时回退到普通页级管理的能力。

### 5.5 lazy `sbrk` 与 COW 共用 fault 路径

`kernel/sysproc.c:40-64` 的 `sys_sbrk()` 表明，这个分支同时保留了 eager 和 lazy 两种扩容方式：

- eager：直接走 `growproc()`，最终由 `uvmalloc()` 建立映射；
- lazy：只增加 `sz`，先不分配物理页，等访问时再 fault。

于是 `vmfault()` 必须同时处理两类 fault：

- 对未映射但合法的延迟分配地址，补一个新页；
- 对已经映射但不可写的 `PTE_COW` 地址，执行写时复制。

这再次体现出：**页错误入口是多个内存管理策略的汇合点**。

## 6. 正确性验证与性能收益

### 6.1 最小功能验证：`cowiso`

这个分支为 COW 增加了一个最小用户态验证程序 `user/cowiso.c`。它会分别测试：

- 一段普通 4KiB 页区域；
- 一段按 `SUPERPGSIZE` 对齐、可被 superpage 映射的候选区域。

核心测试函数 `run_region()` 位于 `user/cowiso.c:9`，思路是：

1. 父进程先记录多个关键偏移的原值；
2. `fork()` 后子进程修改这些偏移；
3. 子进程通过 pipe 把自己读到的新值回传给父进程；
4. 父进程 `wait()` 后检查：
   - 子进程确实看到了写入结果；
   - 父进程看到的原内容没有变化。

如果父进程数据被 child 写坏，说明共享页没有被正确私有化；如果 child 自己读不到修改结果，说明写时复制后的映射更新有问题。

### 6.2 自动回归：`usertests` 中的 `cowiso`

除了独立程序外，同一套思路也被裁剪进了 `user/usertests.c:2817` 的 `cowiso()` 子测试，并注册进 quicktests，见 `user/usertests.c:2929`。这意味着：

- `cowiso` 可以单独在 shell 中手工演示；
- 也可以通过 `./test-xv6.py cowiso` 进入自动回归流程。

这保证了 COW 隔离性不是一次性实验，而是后续修改后可重复验证的行为约束。

### 6.3 性能验证：`cowbench`

`user/cowbench.c` 则从性能角度验证 COW 的价值。它对同一工作集比较两类 `fork()+wait()` 场景：

- **share 模式**：子进程不写继承内存，直接退出；
- **dirty 模式**：子进程按页写 1 字节，强制触发 COW 复制。

工作集包含 64KiB、2MiB 和 8MiB 三档，其中 2MiB/8MiB 更适合观察大页存在时的趋势，见 `user/cowbench.c:8-12`。完整实验整理见 `benchmark/cow-performance-report.md:1-134`。

报告中的一次样例结果显示：

- share 模式下，每次 `fork()` 的 tick 开销基本不随工作集增长而增加；
- dirty 模式下，随着工作集扩大，写时复制成本显著上升；
- 对 2MiB 与 8MiB 这类较大地址空间，只要子进程不主动写共享页，COW 能把 `fork()` 吞吐提升到几十倍甚至上百倍量级。

这与 COW 的设计目标一致：

- 它不是消除复制成本；
- 而是把复制成本从 `fork()` 时前移改成首次写入时按需支付。

## 7. 总结

这条分支中的三项内存管理改造可以概括为一套从底到上的协同设计：

- **伙伴系统** 提供了按阶分配、合并以及 block 级引用计数；
- **superpage** 把这种高阶分配能力转化成 2MiB 用户态映射；
- **COW** 则在普通页和 superpage 之上实现共享只读、写时私有化，并通过 `vmfault()` 与 `copyout()` 覆盖用户态和内核态写路径。

真正让这套设计成立的，不是单个函数技巧，而是几条一致的约束：

1. 物理块大小与映射粒度要能对应；
2. 共享物理块必须有正确的引用计数；
3. 共享 superpage 不能直接拆分，必须先解除共享再降级；
4. 内核写用户页与用户自身写页一样，都必须遵守 COW 语义；
5. lazy allocation 与 COW 可以共用 fault 入口，但必须在语义上严格区分。

从结果看，这套实现既保留了 xv6 原有代码路径的可理解性，又让它具备了更接近现代操作系统的内存管理特征。

## 8. 相关文件

- 伙伴系统：`kernel/kalloc.c`
- superpage / COW 核心：`kernel/vm.c`
- 常量与 PTE 标志：`kernel/riscv.h`
- `fork()` 入口：`kernel/proc.c`
- page fault 入口：`kernel/trap.c`
- `sbrk` 入口：`kernel/sysproc.c`
- 独立功能测试：`user/cowiso.c`
- 自动回归测试：`user/usertests.c`
- 性能 benchmark：`user/cowbench.c`
- 性能报告：`benchmark/cow-performance-report.md`
