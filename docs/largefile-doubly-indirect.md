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

## 4. 核心函数变更（`kernel/fs.c`）

### 4.1 `bmap()` —— 块映射（分配）

在原有直接块和一级间接块分支之后，新增二级间接块分支：

```c
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

`iappend()` 函数负责构建初始文件系统镜像时将用户程序写入 inode。原有 `if/else` 二分支改为 `if/else if/else` 三分支：

```c
if(fbn < NDIRECT){
    // 直接块
} else if(fbn < NDIRECT + NINDIRECT){
    // 一级间接块
} else {
    // 二级间接块
    uint dfbn = fbn - (NDIRECT + NINDIRECT);  // 关键：不修改 fbn
    uint outer = dfbn / NINDIRECT;
    uint inner = dfbn % NINDIRECT;

    // 分配二级间接块
    if(xint(din.addrs[NDIRECT+1]) == 0)
        din.addrs[NDIRECT+1] = xint(freeblock++);
    rsect(xint(din.addrs[NDIRECT+1]), (char*)indirect);

    // 分配一级间接块
    if(indirect[outer] == 0){
        indirect[outer] = xint(freeblock++);
        wsect(xint(din.addrs[NDIRECT+1]), (char*)indirect);
    }
    uint siblock = xint(indirect[outer]);

    // 分配数据块
    rsect(siblock, (char*)indirect);
    if(indirect[inner] == 0){
        indirect[inner] = xint(freeblock++);
        wsect(siblock, (char*)indirect);
    }
    x = xint(indirect[inner]);
}
```

**注意**：`fbn` 不可修改（不同于 `bmap` 中的 `bn`），因为后续代码仍需使用 `fbn` 计算写入偏移 `(fbn + 1) * BSIZE - off`。

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

### 7.1 测试用例更新

`user/usertests.c` 中的 `writebig` 测试从写入 `MAXFILE`（65803）块改为写入 `BIGBLOCKS = 400` 块：

```c
enum { BIGBLOCKS = 400 };
// 块 0-10:   直接块
// 块 11-266: 一级间接块
// 块 267-399: 二级间接块（outer[0] 内的 133 个块）
```

400 个数据块 + 3 个间接块 = 403 块，在 FSSIZE=2000（约 1953 个数据块）内完全容纳。

### 7.2 测试结果

```bash
$ ./test-xv6.py writebig
test writebig: OK
ALL TESTS PASSED

$ ./test-xv6.py -q usertests
# ... 全部 40+ 项测试 ...
ALL TESTS PASSED
```

`writebig` 测试覆盖了直接块、一级间接块和二级间接块三条代码路径的写入和读取验证。全部快速测试套件无回归。

## 8. 改动文件清单

| 文件 | 改动内容 |
| --- | --- |
| `kernel/fs.h:27-39` | NDIRECT 12→11，新增 NDOUBLYINDIRECT，更新 MAXFILE，addrs 数组 NDIRECT+2 |
| `kernel/file.h:30` | inode.addrs 同步变更为 NDIRECT+2 |
| `kernel/fs.c:420-604` | 注释更新 + bmap/bmap_read/itrunc 新增二级间接块分支 |
| `mkfs/mkfs.c:270-300` | iappend 新增二级间接块分配分支 |
| `user/usertests.c:586-629` | writebig 改用 BIGBLOCKS=400 测试三级寻址 |

## 9. 兼容性

- `sizeof(struct dinode)` 保持 64 字节，`IPB` 保持 16
- `sizeof(ip->addrs)` 保持 52 字节
- 超级块布局完全不变
- 旧 `fs.img` 可直接挂载使用（inode 大小一致）
- 如需更大的单文件，可将 `FSSIZE` 从 2000 增大（`kernel/param.h`）
