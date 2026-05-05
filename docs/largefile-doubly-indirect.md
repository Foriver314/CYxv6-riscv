# XV6 大文件支持：二级间接块实现

## 1. 改动背景

原 xv6-riscv 文件系统单个文件的最大容量受限于 inode 的块地址数组：

- `NDIRECT = 12`：12 个直接块
- `NINDIRECT = BSIZE / sizeof(uint) = 256`：1 个一级间接块（包含 256 个块指针）
- `MAXFILE = 12 + 256 = 268` 块 ≈ 268 KB

这对于稍大的工作负载（日志文件、数据库等）远远不够。本次改动通过引入**二级间接块**，将单文件最大块数提升至 **65803 块（约 67 MB）**，同时保持 inode 大小不变（64 字节），完全兼容现有磁盘格式。

## 2. 设计方案

### 2.1 核心思路

将 `NDIRECT` 从 12 缩减为 11，腾出 `addrs[]` 数组中的一个槽位，用于存放二级间接块指针：

```
旧布局：addrs[13] = 12 个直接块 + 1 个一级间接块
新布局：addrs[13] = 11 个直接块 + 1 个一级间接块 + 1 个二级间接块
```

`addrs[NDIRECT+2] = addrs[13]`，与旧布局 `addrs[NDIRECT+1] = addrs[13]` 条目数完全相同，`sizeof(struct dinode)` 保持 64 字节不变。

### 2.2 寻址能力

| 层级 | 块数 | 容量 |
| --- | --- | --- |
| 直接块 (`addrs[0..10]`) | 11 | 11 KB |
| 一级间接块 (`addrs[11]`) | 256 | 256 KB |
| 二级间接块 (`addrs[12]`) | 256 × 256 = 65536 | 64 MB |
| **合计 (MAXFILE)** | **65803** | **~67 MB** |

二级间接块的寻址路径：

```
ip->addrs[12] → 二级间接块 (256 个指针)
                   ├─ [0] → 一级间接块 (256 个数据块指针)
                   │          ├─ [0] → data block
                   │          ├─ [1] → data block
                   │          └─ ...
                   ├─ [1] → 一级间接块
                   │          └─ ...
                   └─ [255] → ...
```

### 2.3 块号映射

给定文件逻辑块号 `bn`，三级判断：

- `bn < 11`：直接块，`addrs[bn]`
- `11 ≤ bn < 267`：一级间接，`bn -= 11`，在 `addrs[11]` 指向的块中索引
- `267 ≤ bn < 65803`：二级间接，`bn -= 267`，外层索引 = `bn / 256`，内层索引 = `bn % 256`

### 2.4 FSSIZE 与 MAXFILE 的关系

`MAXFILE = 65803` 为理论上限，实际单文件大小受 `FSSIZE`（文件系统总块数）约束。本次将 `FSSIZE` 从 2000 提升至 **10000**（9952 个数据块，约 10 MB），使二级间接块外层可实际使用约 38 个指针（共 256 个），远超仅触发门槛的水平。

如需更大文件，可继续增大 `kernel/param.h` 中的 `FSSIZE`，上限为 `MAXFILE`。

## 3. 常量定义变更

### `kernel/fs.h`

```c
// 旧
#define NDIRECT 12
#define NINDIRECT (BSIZE / sizeof(uint))
#define MAXFILE (NDIRECT + NINDIRECT)

// 新
#define NDIRECT 11
#define NINDIRECT (BSIZE / sizeof(uint))
#define NDOUBLYINDIRECT (NINDIRECT * NINDIRECT)   // 65536
#define MAXFILE (NDIRECT + NINDIRECT + NDOUBLYINDIRECT)  // 65803
```

`struct dinode` 中 `addrs` 数组：

```c
// 旧
uint addrs[NDIRECT+1];   // 13 项
// 新
uint addrs[NDIRECT+2];   // 13 项（11+2=13，项数不变）
```

### `kernel/file.h`

内存 inode 同步变更：

```c
// 旧
uint addrs[NDIRECT+1];
// 新
uint addrs[NDIRECT+2];
```

`sizeof(ip->addrs)` 保持 52 字节不变，因此 `iupdate()` 和 `ilock()` 中的 `memmove` 调用无需任何修改。

### `kernel/param.h`

```c
// 旧
#define FSSIZE  2000
// 新
#define FSSIZE  10000  // 9952 个数据块，约 10 MB
```

## 4. 核心函数变更（`kernel/fs.c`）

### 4.1 `bmap()` —— 块映射（分配）

在原有直接块和一级间接块分支之后，新增二级间接块分支：

```c
// NOTE: on allocation failure, intermediate indirect blocks already
// allocated are not rolled back (consistent with existing xv6 behavior).

bn -= NINDIRECT;                    // 此时 bn 已是减去 NDIRECT 之后的值

if(bn < NDOUBLYINDIRECT){
    // 1. 加载/分配二级间接块
    if((addr = ip->addrs[NDIRECT+1]) == 0){
        addr = balloc(ip->dev);
        ip->addrs[NDIRECT+1] = addr;
    }
    bp = bread(ip->dev, addr);
    a = (uint*)bp->data;

    // 2. 加载/分配一级间接块
    uint outer = bn / NINDIRECT;
    if((addr = a[outer]) == 0){
        addr = balloc(ip->dev);
        a[outer] = addr;
        log_write(bp);              // 记录二级间接块的修改
    }
    uint diblock = addr;
    brelse(bp);                     // 及时释放，避免占用过多缓冲区

    // 3. 加载/分配数据块
    bp = bread(ip->dev, diblock);
    a = (uint*)bp->data;
    uint inner = bn % NINDIRECT;
    if((addr = a[inner]) == 0){
        addr = balloc(ip->dev);
        a[inner] = addr;
        log_write(bp);              // 记录一级间接块的修改
    }
    brelse(bp);
    return addr;
}
```

关键设计点：
- 二级间接块读取后立即释放（`brelse`），再读取一级间接块，避免同时占用两个间接块缓冲区
- 每层分配新块后立即 `log_write`，确保修改被当前事务捕获
- `balloc` 内部的 `bzero` 已对同一块调用过 `log_write`，重复调用会被日志层的吸收机制优化掉
- 若 `balloc` 失败，此前已分配的中间间接块不会回滚——这是与原始 xv6 一级间接路径一致的行为，已在函数头注释说明

### 4.2 `bmap_read()` —— 块映射（只读）

与 `bmap()` 结构对称，但不分配新块，遇到空洞（`addr == 0`）直接返回 0：

```c
bn -= NINDIRECT;

if(bn < NDOUBLYINDIRECT){
    if((addr = ip->addrs[NDIRECT+1]) == 0)
        return 0;                   // 二级间接块不存在
    bp = bread(ip->dev, addr);
    a = (uint*)bp->data;
    uint outer = bn / NINDIRECT;
    if((addr = a[outer]) == 0){
        brelse(bp);
        return 0;                   // 一级间接块不存在
    }
    uint diblock = addr;
    brelse(bp);

    bp = bread(ip->dev, diblock);
    a = (uint*)bp->data;
    uint inner = bn % NINDIRECT;
    addr = a[inner];                // 可能为 0（空洞）
    brelse(bp);
    return addr;
}
```

### 4.3 `itrunc()` —— 截断释放

在一级间接块释放之后，新增二级间接块释放循环：

```c
if(ip->addrs[NDIRECT+1]){
    bp = bread(ip->dev, ip->addrs[NDIRECT+1]);
    a = (uint*)bp->data;
    for(j = 0; j < NINDIRECT; j++){
        if(a[j]){
            // 读取每个一级间接块，释放其 256 个数据块
            struct buf *bp2 = bread(ip->dev, a[j]);
            uint *a2 = (uint*)bp2->data;
            for(k = 0; k < NINDIRECT; k++){
                if(a2[k])
                    bfree(ip->dev, a2[k]);
            }
            brelse(bp2);
            bfree(ip->dev, a[j]);   // 释放一级间接块本身
        }
    }
    brelse(bp);
    bfree(ip->dev, ip->addrs[NDIRECT+1]);  // 释放二级间接块本身
    ip->addrs[NDIRECT+1] = 0;
}
```

释放顺序：数据块 → 一级间接块 → 二级间接块。每层独立 `bread/brelse`，缓冲区缓存（NBUF=30）足以满足嵌套循环的工作集。

### 4.4 注释更新

```c
// Inode content
//
// The content (data) associated with each inode is stored
// in blocks on the disk. The first NDIRECT block numbers
// are listed in ip->addrs[].  The next NINDIRECT blocks are
// listed in block ip->addrs[NDIRECT].  The next NDOUBLYINDIRECT
// blocks are reached through ip->addrs[NDIRECT+1], which points
// to NINDIRECT singly-indirect block pointers.
```

## 5. `mkfs/mkfs.c` 变更

### 5.1 `iappend()` 三级分支

原有 `if/else` 二分支改为 `if/else if/else` 三分支：

```c
if(fbn < NDIRECT){
    // 直接块
} else if(fbn < NDIRECT + NINDIRECT){
    // 一级间接块
} else {
    // 二级间接块
    uint dindirect[NINDIRECT];          // 独立缓冲区，避免覆盖间接块数组
    uint dfbn = fbn - (NDIRECT + NINDIRECT);
    uint outer = dfbn / NINDIRECT;
    uint inner = dfbn % NINDIRECT;

    // 分配二级间接块
    if(xint(din.addrs[NDIRECT+1]) == 0)
        din.addrs[NDIRECT+1] = xint(freeblock++);
    rsect(xint(din.addrs[NDIRECT+1]), (char*)dindirect);

    // 分配一级间接块
    if(dindirect[outer] == 0){
        dindirect[outer] = xint(freeblock++);
        wsect(xint(din.addrs[NDIRECT+1]), (char*)dindirect);
    }
    uint siblock = xint(dindirect[outer]);

    // 分配数据块（复用 indirect 缓冲区读取内层）
    rsect(siblock, (char*)indirect);
    if(indirect[inner] == 0){
        indirect[inner] = xint(freeblock++);
        wsect(siblock, (char*)indirect);
    }
    x = xint(indirect[inner]);
}
```

**注意**：`fbn` 不可修改（不同于 `bmap` 中的 `bn`），因为后续代码仍需使用 `fbn` 计算写入偏移 `(fbn + 1) * BSIZE - off`。

### 5.2 缓冲区拆分

二级间接路径使用独立的 `dindirect[NINDIRECT]` 局部数组，与通用的 `indirect[NINDIRECT]` 分离，消除覆盖风险。即使后续调整代码顺序，也不会出现外层数据被内层读取覆盖的问题。

## 6. 日志安全性分析

单次 `writei()` 调用在最坏情况（分配全新的二级间接路径 + 跨块写入）下的日志写入：

| 操作 | 写入的日志块 |
| --- | --- |
| `balloc` 分配数据块 | bitmap 块（1） |
| `balloc` 分配一级间接块 | bitmap 块（可能同上） |
| `balloc` 分配二级间接块 | bitmap 块（可能同上） |
| `log_write` 一级间接块 | 1 |
| `log_write` 二级间接块 | 1 |
| `iupdate` 写入 inode | 1 |
| 第二个数据块（跨块写入） | bitmap（吸收）+ data（吸收） |
| **最坏合计** | **≤ 9 个不同块** |

`MAXOPBLOCKS = 10`，留有安全余量。日志吸收机制确保对同一块的多次写入只计一次。

## 7. 测试

### 7.1 `writebig` —— 三级寻址验证

写入 `NDIRECT + NINDIRECT + 200 = 467` 块，覆盖全部三条路径：

- 块 0-10：直接块
- 块 11-266：一级间接块
- 块 267-466：二级间接块（外层索引 0 内的 200 个块）

写入带序号的块内容后回读校验，确保 `bmap` 和 `bmap_read` 在所有层级正确工作。

### 7.2 `truncbig` —— 二级间接块释放验证（新增）

写入 `NDIRECT + NINDIRECT + 50 = 317` 块（跨越二级间接区域），`unlink` 触发 `iput → itrunc` 释放所有块，随后创建新文件并写入相同数量的块，验证旧块确实被回收：

```
创建 truncbig（317 块，含二级间接路径）
  → unlink（触发 itrunc 释放三步走）
  → 创建 truncbig2（317 块）
  → 写入成功 = 旧块已正确回收
```

### 7.3 `diskfull` —— 填满磁盘测试

从 `for(i = 0; i < MAXFILE; i++)` 改为 `for(i = 0; i < 5000; i++)`，每文件最多写 5000 块，通过多文件交叉填充磁盘（避免退化为单文件撑爆），之后验证 `dirlink` 在磁盘满时优雅失败。

### 7.4 测试结果

```bash
$ ./test-xv6.py writebig
test writebig: OK

$ ./test-xv6.py truncbig
test truncbig: OK

$ ./test-xv6.py diskfull
test diskfull: OK
```

## 8. 改动文件清单

| 文件 | 改动内容 |
| --- | --- |
| `kernel/fs.h:27-39` | NDIRECT 12→11，新增 NDOUBLYINDIRECT，更新 MAXFILE，addrs 数组 NDIRECT+2 |
| `kernel/file.h:30` | inode.addrs 同步变更为 NDIRECT+2 |
| `kernel/fs.c:420-604` | 注释更新 + bmap/bmap_read/itrunc 新增二级间接块分支 + bmap 部分分配注释 |
| `kernel/param.h:12` | FSSIZE 2000→10000 |
| `mkfs/mkfs.c:270-310` | iappend 新增二级间接块分支，独立 dindirect 缓冲区 |
| `user/usertests.c:586-690` | writebig 改用 NDIRECT+NINDIRECT+200；新增 truncbig 测试；diskfull 改用每文件 5000 块上限 |

## 9. 兼容性

- `sizeof(struct dinode)` 保持 64 字节，`IPB` 保持 16
- `sizeof(ip->addrs)` 保持 52 字节
- 超级块布局完全不变
- 旧 `fs.img` 可直接挂载使用（inode 大小一致）
- 如需更大的单文件，可继续增大 `FSSIZE`（`kernel/param.h`），理论上限 `MAXFILE = 65803` 块

## 10. 实施过程与问题解决

本节汇总在实现二级间接块过程中遇到的问题及其解决方案。

### 10.1 FSSIZE 与 MAXFILE 脱节

**问题**：初始实现保持了 `FSSIZE = 2000`（约 1953 个数据块），但 `MAXFILE` 已增至 65803。扣除直接块（11）和一级间接块（256），二级间接块理论上可容纳 65536 个数据块，而实际磁盘仅剩 ~1900 个数据块。这意味着二级间接块的外层 256 个指针中最多只能用到约 7 个，**大文件能力形同虚设**。

**解决**：将 `FSSIZE` 从 2000 提升至 10000（约 9952 个数据块，~10 MB）。二级间接块外层可用约 38 个指针，是对 88% 磁盘空间的实际利用。`FSSIZE` 可根据需要继续增大至 `MAXFILE`。

```c
// kernel/param.h
#define FSSIZE  10000  // 9952 个数据块，约 10 MB
```

### 10.2 `diskfull` 测试退化

**问题**：`diskfull` 测试的原逻辑是为每个文件写入 `MAXFILE` 块以填满磁盘。`MAXFILE` 从 268 变为 65803 后，第一个文件就会试写 65803 块——由于磁盘仅有 ~1953 个可用块，写入在第 ~1950 块失败，**磁盘被单个文件撑满**。测试从"多文件交叉填充"退化为"单文件撑爆"，对 `dirlink` 无法扩展目录时的降级行为覆盖变弱。

**解决**：将内层循环从 `for(i = 0; i < MAXFILE; i++)` 改为 `for(i = 0; i < 5000; i++)`，不再依赖 `MAXFILE`。每文件最多写 5000 块，磁盘通过多个文件交叉填充，`dirlink` 测试覆盖度恢复。

```c
// user/usertests.c — diskfull()
for(int i = 0; i < 5000; i++){   // 不再使用 MAXFILE
    char buf[BSIZE];
    if(write(fd, buf, BSIZE) != BSIZE){
        done = 1;
        close(fd);
        break;
    }
}
```

### 10.3 `diskfull` 测试超时

**问题**：增大 `FSSIZE` 后，`diskfull` 需要在 QEMU 中写入近 20000 个磁盘块。每块写入涉及 `write` 系统调用 → `bmap` 分配 → `log_write` 日志 → virtio 磁盘 I/O，在 QEMU 模拟环境下耗时严重。尝试 `FSSIZE=20000` 时 300 秒超时。

**解决**：在 `FSSIZE` 与测试时间之间取折中——`FSSIZE=10000` 兼顾了二级间接块的实际利用率和测试可运行性。同时将每文件块数从 1000 提高到 5000，减少文件数量以降低目录操作开销。最终 `diskfull` 在 300 秒内完成。

**权衡**：若需更大的 `FSSIZE`，可适当放宽 `diskfull` 超时，或将 `diskfull` 单独运行而非作为快速测试套件的一部分。

### 10.4 `writebig` 测试语义割裂

**问题**：初版将循环上限从 `MAXFILE` 改为硬编码 `BIGBLOCKS = 400`。测试名暗示验证"大文件/最大文件"，但实际仅写到 400 块，与 `MAXFILE` 宏失去关联，后续若修改 `NDIRECT` 等常量不会自动反映到测试中。

**解决**：使用 `NDIRECT + NINDIRECT + 200 = 467` 作为上限，与文件系统常量保持语义关联：

```c
// user/usertests.c — writebig()
enum { BIGBLOCKS = NDIRECT + NINDIRECT + 200 };
// 块 0-10: 直接, 块 11-266: 一级间接, 块 267-466: 二级间接
```

### 10.5 `mkfs.c` 缓冲区隐蔽重用

**问题**：`iappend()` 仅声明了一个 `uint indirect[NINDIRECT]` 数组。在二级间接路径中，先用该数组读取外层间接块，提取 `siblock` 值后，再用**同一数组**读取内层间接块——覆盖了外层数据。虽然代码因提前缓存了 `siblock` 而正确，但这是**维护陷阱**：若后续有人调整代码顺序，极易在覆盖后仍访问 `indirect[outer]`。

**解决**：在二级间接路径内声明独立的局部数组 `dindirect[NINDIRECT]`，与通用的 `indirect` 完全隔离：

```c
// mkfs/mkfs.c — iappend() 二级间接分支
} else {
    uint dindirect[NINDIRECT];          // 外层独立缓冲区
    // ...
    rsect(xint(din.addrs[NDIRECT+1]), (char*)dindirect);
    // ...
    uint siblock = xint(dindirect[outer]);

    rsect(siblock, (char*)indirect);    // 内层复用通用缓冲区
    // ...
}
```

### 10.6 `bmap()` 部分分配残留

**问题**：在二级间接路径中，若外层间接块分配成功但内层间接块或数据块分配失败（`balloc` 返回 0），函数直接返回 0。此时外层间接块已在 bitmap 中标记为占用且指针已写入 `ip->addrs[NDIRECT+1]`，但 inode 尚未 `iupdate` 刷盘——若此时崩溃，外层间接块可能被遗忘在 bitmap 中，造成轻微空间泄漏。原始 xv6 的一级间接路径也有同样问题，**不是新引入的 regression，而是设计一致性的已知缺陷**。

**解决**：在 `bmap()` 函数头注释中添加说明，明确这是与上游一致的已知行为，避免未来维护者误以为是新 bug：

```c
// kernel/fs.c — bmap()
// NOTE: on allocation failure, intermediate indirect blocks already
// allocated are not rolled back (consistent with existing xv6 behavior).
```

若追求更严谨的分配语义，可在 `balloc` 失败时回滚已分配的中间块（调用 `bfree` 并清零对应 `addrs[]` 和间接块条目），但会显著增加错误路径的复杂度。

### 10.7 缺少 `itrunc` 二级间接路径的直接测试

**问题**：初始实现仅通过 `writebig` 测试间接覆盖 `itrunc`（文件 `unlink` 时触发），缺乏对二级间接块释放逻辑的直接验证。如果 `itrunc` 在遍历二级间接结构时 panic 或未正确释放所有块，现有测试无法捕获。

**解决**：新增 `truncbig` 测试，完整验证二级间接块的分配→释放→再分配循环：

1. 创建文件，写入 317 块（`NDIRECT + NINDIRECT + 50`），跨越二级间接区域
2. `unlink` 触发 `itrunc` 释放全部块
3. 创建新文件，再次写入 317 块——成功则证明旧块已被正确回收

```c
// user/usertests.c — truncbig()
enum { TRUNCSZ = NDIRECT + NINDIRECT + 50 };

// 写入 truncbig（317 块，含二级间接路径）
// → unlink（触发 itrunc 三步释放）
// → 创建 truncbig2（317 块）
// → 写入成功 = 旧块已正确回收
```

### 问题总结

| 问题 | 根因 | 解决方式 |
| --- | --- | --- |
| FSSIZE 与 MAXFILE 脱节 | FSSIZE 未随 MAXFILE 同步增大 | FSSIZE 2000→10000 |
| diskfull 测试退化 | MAXFILE 循环导致单文件撑爆 | 改用固定每文件上限 5000 块 |
| diskfull 超时 | 大 FSSIZE 下写入耗时过长 | FSSIZE 与测试时间折中 |
| writebig 语义割裂 | 硬编码 400 与 MAXFILE 无关 | 改用 NDIRECT+NINDIRECT+200 |
| mkfs 缓冲区重用 | 同一数组读取外/内层 | 独立 dindirect 数组 |
| bmap 部分分配残留 | balloc 失败后不回滚中间块 | 函数头注释说明已知行为 |
| 缺少 itrunc 测试 | 未直接验证二级间接释放 | 新增 truncbig 测试 |
