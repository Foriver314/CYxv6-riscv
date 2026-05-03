# XV6 读写锁实现总结报告

## 1. 改动背景

本次改动为当前 xv6-riscv 系统新增读写锁机制。原系统主要提供两类锁：

- `spinlock`：适合短临界区，忙等获取；
- `sleeplock`：适合可能睡眠的长临界区，但同一时间只允许一个持有者。

在读多写少的共享数据结构中，普通互斥锁会把所有读操作串行化，无法利用多个 CPU 同时执行只读临界区的能力。读写锁的目标是：

- 多个 reader 可以同时进入临界区；
- writer 仍然独占；
- 等待时通过 `sleep/wakeup` 阻塞，而不是忙等；
- 对用户态提供可测试、可复用的 syscall 接口。

## 2. 核心实现

### 2.1 内核读写锁 primitive

新增文件：

- `kernel/rwlock.h`
- `kernel/rwlock.c`

`struct rwlock` 的核心状态包括：

| 字段 | 作用 |
| --- | --- |
| `struct spinlock lk` | 保护 rwlock 内部状态 |
| `readers` | 当前持有读锁的 reader 数量 |
| `writer` | 当前是否有 writer 持有写锁 |
| `waiting_readers` | 正在等待的 reader 数量 |
| `waiting_writers` | 正在等待的 writer 数量 |
| `writer_pid` | 当前写锁持有者 pid，用于调试/检查 |

提供的内核 API：

```c
void initrwlock(struct rwlock *rw, char *name);
int  acquireread(struct rwlock *rw);
void releaseread(struct rwlock *rw);
int  acquirewrite(struct rwlock *rw);
void releasewrite(struct rwlock *rw);
int  holdingread(struct rwlock *rw);
int  holdingwrite(struct rwlock *rw);
int  rwlock_isidle(struct rwlock *rw);
```

### 2.2 锁语义

#### 读锁获取

读锁在以下条件下可以进入：

- 当前没有 writer；
- 当前没有等待中的 writer。

第二个条件用于避免 writer 饥饿。只要有 writer 已经排队，后续新 reader 就不再插队，等当前已有 reader 全部释放后，writer 优先获得锁。

#### 写锁获取

写锁在以下条件下可以进入：

- 当前没有 writer；
- 当前没有 reader。

写者等待时会增加 `waiting_writers`，从而阻止新的 reader 继续进入。

#### 睡眠等待

读写锁内部复用 xv6 的 `sleep(chan, lock)` / `wakeup(chan)` 机制：

- 等待者睡眠在 rwlock 对象地址上；
- 睡眠时释放内部 spinlock；
- 被唤醒后重新检查条件；
- `kill()` 可打断等待中的用户进程，避免被杀进程永久卡在 syscall 中。

## 3. 用户态 syscall 接口

新增文件：

- `kernel/sysrwlock.c`

用户态不直接暴露内核地址，而是通过固定大小的全局锁表分配整数 handle。当前表大小为：

```c
#define NRWLOCK 32
```

用户态 API：

```c
int rwlock_alloc(void);
int rwlock_rdlock(int id);
int rwlock_wrlock(int id);
int rwlock_unlock(int id);
int rwlock_free(int id);
```

对应 syscall 编号新增在 `kernel/syscall.h`：

| syscall | 编号 |
| --- | ---: |
| `SYS_rwlock_alloc` | 23 |
| `SYS_rwlock_rdlock` | 24 |
| `SYS_rwlock_wrlock` | 25 |
| `SYS_rwlock_unlock` | 26 |
| `SYS_rwlock_free` | 27 |

并同步接入：

- `kernel/syscall.c`
- `user/user.h`
- `user/usys.pl`

## 4. 用户锁表设计

`kernel/sysrwlock.c` 中维护全局 `rwtable`。每个用户锁槽位包含：

| 字段 | 作用 |
| --- | --- |
| `used` | handle 是否有效 |
| `refs` | 当前正在使用该槽位的 syscall 引用数，用于避免释放/复用竞态 |
| `struct rwlock lock` | 实际内核读写锁 |
| `struct spinlock meta` | 保护 owner bookkeeping |
| `writer` | 当前写锁持有进程 |
| `readers[NPROC]` | 当前读锁持有进程列表 |
| `read_counts[NPROC]` | 每个 reader 的读锁计数 |

### 4.1 ownership 检查

syscall 层记录锁持有者，用于实现：

- `rwlock_unlock(id)` 自动判断当前进程持有读锁还是写锁；
- 非持有者调用 `unlock` 返回 `-1`；
- 同一进程重复加锁或读锁升级写锁返回 `-1`，避免重入和升级死锁；
- `rwlock_free(id)` 仅允许在锁空闲且没有等待/引用时成功。

### 4.2 进程退出清理

新增：

```c
void rwlock_proc_cleanup(struct proc *p);
```

并在 `kernel/proc.c` 的 `kexit()` 中调用。

如果进程退出时仍持有用户读写锁，内核会自动释放它持有的读锁或写锁，避免其他等待者永久睡眠。

## 5. 初始化与构建接入

### 5.1 初始化

在 `kernel/main.c` 中加入：

```c
rwlockinit();
```

初始化全局用户读写锁表。

### 5.2 构建接入

`Makefile` 新增内核对象：

```make
$K/sysrwlock.o \
$K/rwlock.o \
```

同时新增用户 benchmark 程序：

```make
$U/_rwbench\
```

## 6. 正确性测试

在 `user/usertests.c` 中新增 5 个 quicktests：

| 测试 | 覆盖内容 |
| --- | --- |
| `rwlockreaders` | 多个 reader 可并发进入 |
| `rwlockwriter` | writer 持锁时 reader 被阻塞 |
| `rwlockfair` | 有 writer 排队时后续 reader 不插队 |
| `rwlockexit` | 进程持锁退出后内核自动清理 |
| `rwlockbad` | 非法 handle、非 owner unlock、递归加锁、持锁 free 等错误路径 |

已执行并通过：

```sh
./test-xv6.py rwlockreaders
./test-xv6.py rwlockwriter
./test-xv6.py rwlockfair
./test-xv6.py rwlockexit
./test-xv6.py rwlockbad
./test-xv6.py -q usertests
```

## 7. 性能测试

新增 benchmark：

- `user/rwbench.c`
- `benchmark/rwlock-performance-report.md`

`rwbench` 对比两种模式：

1. **exclusive**：所有操作都使用写锁，相当于互斥锁串行化；
2. **rw**：读操作使用读锁，写操作使用写锁。

测试覆盖不同写比例：

| Workload | 写比例 | 场景 |
| --- | ---: | --- |
| `readonly` | 0% | 纯读 |
| `readmostly` | 10% | 读多写少 |
| `balanced` | 50% | 读写均衡 |
| `writemostly` | 90% | 读少写多 |
| `writeonly` | 100% | 纯写 |

分别在 `CPUS=1`、默认 `CPUS=3`、`CPUS=8` 下运行。

### 7.1 性能结论

| 场景 | 结论 |
| --- | --- |
| 纯读/读密集 | 收益显著，多核纯读吞吐提升约 `4.31x~4.62x` |
| 10% 写 | 多核仍有收益，约 `1.10x~1.37x` |
| 50% 写 | 基本持平 |
| 90% 写 | 没有明显收益，可能小幅变慢 |
| 100% 写 | 没有结构性收益，只体现调度波动 |

因此，读写锁适合用于读路径频繁、写路径较少的共享数据结构；对写路径占主导的数据结构，继续使用普通互斥/自旋/睡眠锁通常更合适。

## 8. 主要设计取舍

### 8.1 未替换文件系统 sleeplock

本次没有把 inode/buffer 的 `sleeplock` 直接替换成读写锁。原因是文件系统路径中存在大量隐含写语义，例如：

- inode lazy load 会修改 `ip->valid` 和 inode cache 内容；
- `iput()` / `itrunc()` / `iupdate()` 等路径需要独占；
- 目录读写、链接数、日志事务等需要重新审计锁语义。

直接替换风险较高，且不一定能马上带来收益。更合理的后续方向是先基于本次 primitive 增加明确的 `ilock_read()` / `ilock_write()`，再逐条审计读路径和写路径。

### 8.2 writer 优先策略

本实现使用 `waiting_writers` 阻止新 reader 插队，避免 writer 饥饿。

代价是：在读多写少但 writer 频繁出现时，新 reader 可能被更早阻塞，吞吐不一定总是最大化。性能报告中的 10% 写和 50% 写场景已经体现了这一点。

### 8.3 不支持重入和升级

当前用户态接口不支持：

- 同一进程重复加读锁；
- 同一进程重复加写锁；
- 持读锁升级为写锁；
- 持写锁降级为读锁。

这些操作容易引入自死锁或复杂 owner 状态。当前实现统一返回 `-1`，保持语义简单可控。

## 9. 文件清单

### 新增文件

- `kernel/rwlock.h`
- `kernel/rwlock.c`
- `kernel/sysrwlock.c`
- `user/rwbench.c`
- `benchmark/rwlock-performance-report.md`
- `docs/rwlock-implementation-summary.md`

### 修改文件

- `Makefile`
- `kernel/defs.h`
- `kernel/main.c`
- `kernel/proc.c`
- `kernel/syscall.c`
- `kernel/syscall.h`
- `user/user.h`
- `user/usertests.c`
- `user/usys.pl`

## 10. 后续可改进方向

1. **文件系统读路径改造**
   - 在 inode 层引入 `ilock_read()` / `ilock_write()`；
   - 审计 `readi()`、`stati()`、目录查找等是否能安全使用读锁；
   - 保留 `iput()`、`writei()`、`itrunc()` 等写路径独占。

2. **更细粒度的性能测试**
   - 增加 worker 数可配置项；
   - 增加多次运行取平均值；
   - 分离 syscall 开销、临界区计算开销和调度开销。

3. **更多调度策略对比**
   - 当前实现偏 writer 优先；
   - 后续可对比 reader 优先、公平队列或阶段公平策略，观察不同读写比例下的吞吐和延迟。

## 11. 总结

本次改动为 xv6-riscv 增加了完整的读写锁能力：

- 内核提供可复用的 sleep-based rwlock primitive；
- 用户态通过 syscall handle 使用读写锁；
- 进程退出时自动清理持有锁，避免永久阻塞；
- usertests 覆盖并发读、写互斥、公平性、退出清理和错误路径；
- rwbench 覆盖从纯读到纯写的多种读写比例，并形成性能报告。

测试结果表明：读写锁在读密集多核场景下收益明显，但在写密集场景中不会带来结构性提升。因此后续使用时应根据实际读写比例选择锁类型，而不是无差别替换现有锁。
