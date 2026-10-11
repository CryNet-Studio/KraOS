# ArkOS 存储与驱动层实施计划

状态：**已确认方案，尚未实施**
记录时间：2026-10-02

> **实施方式更新（KraOS / Kotlin 优先）**：依 `PLAN-kraos-kotlin-native.md` 的已确认决策，本计划将在 **KraOS 0.2.0** 上以 **Kotlin/Native** 实现，且**不在 C 侧先做参考实现**。
> 下文 `kernel/*.c` 的落点应理解为**逻辑模块划分**，实际落点为 `kernel/*.kt`；端口 I/O、MMIO 与中断汇编仍由 `boot/*.S` 与 cinterop 提供。
> 代码前缀统一为 `kra_`/`KRA_`（即下文的 `ARK_DRIVER_DECLARE` → `KRA_DRIVER_DECLARE`、`ark_driver_register()` → `kra_driver_register()`）。
> 实际开工点在工具链就绪（K3）之后。

## 已确认的四项决策

| 决策点 | 结论 |
| --- | --- |
| 虚拟磁盘驱动范围 | ATA PIO（先行）→ virtio-blk (PCI) → AHCI (SATA)，NVMe 暂不做 |
| 磁盘文件系统 | 自定义 **ArkFS**（不采用 FAT32 / ext2） |
| 插件机制 | **运行时加载 ELF 模块**（ET_REL），不是编译期注册 |
| 内存基础设施 | 引入最小 **bump 分配器**，只分配不回收 |

## 关键约束：先有盘，才能装插件

运行时加载模块存在引导依赖链：

```
读 .ko 文件 → 需要 ArkFS → 需要块层 → 需要 ATA PIO
```

因此插件不能是启动第一步，方案定为两条注册路径共用同一套 API：

- **内置驱动**（ATA PIO / 块层 / ArkFS，作为 bootstrap）：`ARK_DRIVER_DECLARE` 宏放入链接段 `.ark_drivers`，启动时统一注册。
- **外部驱动**（virtio-blk、AHCI）：磁盘上的 `.ko`，运行时由 ELF 加载器载入，其 `ark_module_init()` 调用 `ark_driver_register()`。
- 两者最终都走 `driver_register()`，插件模型统一，仅来源不同。

## 分层与文件落点

```
kernel/mm.c        bump 分配器 + phys_addr() + 内存屏障        (P0)
kernel/ksym.c      ARK_EXPORT 符号表，供模块解析              (P0/P5)
kernel/pci.c       0xCF8/0xCFC 配置空间、BAR、Bus Master      (P1)
kernel/driver.c    bus/device/driver/class 核心 + 注册表      (P1)
kernel/block.c     块设备 + MBR 分区扫描                      (P2)
kernel/ata_pio.c   ATA PIO 驱动（内置 bootstrap）             (P3)
kernel/arkfs.c     ArkFS 挂载/读写/同步 + VFS 对接            (P4)
kernel/elf.c       ELF64 ET_REL 模块加载器                    (P5)
modules/virtio_blk.c, modules/ahci.c   .ko 插件               (P6)
scripts/mkarkfs.py 造/查镜像工具                              (P4)
```

新增 `kernel/*.c` 会被 `Makefile` 的通配规则自动收录；只需给 `boot/linker.ld` 增加 `.ark_ksym` 与 `.ark_drivers` 两个 `KEEP` 段。

## 核心 API 草案

### Driver API（对标 Linux `driver_probe_device`，同步单线程版）

```c
struct ark_bus    { const char *name; int (*match)(struct ark_device*, struct ark_driver*);
                    int (*probe)(struct ark_device*); void (*remove)(struct ark_device*); };
struct ark_device { struct ark_bus *bus; uint16_t vendor, device; uint8_t class, subclass;
                    uint64_t bar[6]; int irq; uint64_t dma_mask; void *priv; };
struct ark_driver { const char *name; struct ark_bus *bus;
                    const struct ark_id *ids;          /* 匹配表 */
                    int (*probe)(struct ark_device*); void (*remove)(struct ark_device*); };

void ark_bus_register(struct ark_bus *);
void ark_driver_register(struct ark_driver *);   /* 内置与 .ko 共用，需 ARK_EXPORT */
```

- `ARK_DRIVER_DECLARE(...)` → 链接段 `.ark_drivers`，启动自动注册（内置路径）
- `.ko` 的 `ark_module_init()` 里调 `ark_driver_register()`（外部路径）

### 类接口（首个：block）

```c
struct block_ops { int (*read)(struct block_device*, uint64_t lba, void *buf, unsigned n);
                   int (*write)(struct block_device*, uint64_t lba, const void *buf, unsigned n);
                   int (*flush)(struct block_device*); };
```

设备节点 `/dev/vda`、分区 `vda1`。

### 模块 ABI（ET_REL，运行在 ring 0）

```c
/* .ko 必须导出 */
int  ark_module_init(void);   /* 注册驱动等 */
void ark_module_exit(void);   /* 仅做注销，内存不回收 */
```

- 编译标志与内核一致：`-ffreestanding -fno-pie -mcmodel=small -mno-red-zone -mgeneral-regs-only`
- 支持重定位：`R_X86_64_64 / PC32 / PLT32 / 32 / 32S / GOTPCREL(X) / REX_GOTPCRELX`
- 符号解析走 `ARK_EXPORT` 表（Linux `EXPORT_SYMBOL` 的等价物），未解析符号直接拒载
- 模块段用 bump 分配器加载到低端内存（<4GiB，且 <2GiB 以满足 `32S`）

### ArkFS 磁盘格式（自定义，扁平目录）

- 512B 块；`block0` 超级块（magic `ARKFS1`、版本、容量、目录区、位图区、数据区）
- 固定目录项：`used / name[40] / first_block / block_count / size / type`
- 连续块分配、写穿透、无日志；`sync` 落盘
- `scripts/mkarkfs.py` 造镜像、列表、解包，供 `make disk` 与测试复用

### 虚拟磁盘驱动优先级（对齐 Linux 源码）

| 优先级 | 驱动 | QEMU 参数 | Linux 参考 | 难度 |
| --- | --- | --- | --- | --- |
| 1 | ATA PIO (IDE) | `-drive if=ide`（默认即插） | `drivers/ata/libata-sff.c` PIO 路径 | 低 |
| 2 | virtio-blk (PCI) | `-device virtio-blk-pci` | `drivers/block/virtio_blk.c` | 中 |
| 3 | AHCI (SATA) | `-device ich9-ahci` | `drivers/ata/ahci.c` | 中 |

## 阶段计划

| 阶段 | 内容 | 验证方式 |
| --- | --- | --- |
| P0 | bump 分配器、`ARK_EXPORT`/符号表、DMA 屏障约定、链接段 | 串口打印可用内存池；宿主单测分配边界 |
| P1 | PCI 枚举 + Driver API + 注册表 | QEMU 串口列出发现的 PCI 设备（能看到 virtio/ahci） |
| P2 | 块层 + MBR 解析 | 空盘/无盘优雅跳过，不 panic |
| P3 | ATA PIO 内置驱动 | `-drive if=ide` 读 MBR，打印容量 |
| P4 | ArkFS + VFS 对接 + `mount/df/sync` 命令 | `write`→`sync`→`reboot`→`cat` 数据仍在（与 RAM 重置用例对照） |
| P5 | ELF64 模块加载器 + `insmod/lsmod/rmmod` + `make modules` | 加载最小 `.ko`，其 init 注册空驱动并被 `lsmod` 列出 |
| P6 | virtio-blk `.ko`、AHCI `.ko`；`make disk` | 两种盘都能挂载 ArkFS 并通过读写测试 |
| P7 | 更新 `tests/boot_test.py`、`ARCHITECTURE.md`、`VALIDATION.md`、`README.md` | BIOS + UEFI 双路径回归 |

## 需要改动的现有文件

| 文件 | 改动 |
| --- | --- |
| `include/ark.h` | 新增驱动/块/FS/模块接口声明 |
| `boot/linker.ld` | 增加 `.ark_ksym`、`.ark_drivers` 两个 KEEP 段 |
| `Makefile` | 模块编译目标、磁盘镜像目标 |
| `kernel/vfs.c` | 改造成 `ramfs` 类型，保留「重启即失」语义 |
| `kernel/shell.c` | 新增 `mount`/`df`/`sync`/`insmod`/`lsmod`/`rmmod` |
| `tests/boot_test.py` | 增加挂盘持久化用例 |
| `boot/grub.cfg` | 无需改动 |

## 风险与提前认下的代价

1. **ELF 模块加载是最高风险项**：无 MMU 保护、全部 ring 0，坏模块会直接崩整个内核；bump 分配器不回收，`rmmod` 只能注销驱动、不能真正释放内存。这是选择运行时模块的固有代价。
2. **不能把驱动全做成 `.ko`**：至少 ATA PIO 必须内置，否则无法读盘、无法加载任何模块。
3. **同步单请求 I/O**：当前无调度器（`desktop.c` 是单前台循环），virtio/AHCI 先用轮询而非多队列中断模型。
4. **DMA 约束**：缓冲区必须 <4GiB，分配器须落在低端内存并在代码里强制校验。
5. **现有回归不能被破坏**：`boot_test.py` 的「reboot 后 RAM 重置」断言必须继续通过——磁盘写入只在显式 `sync` 时发生。

## 交付切分

**P0–P4 即可交付一条完整链路**：能读能写、`sync` 后重启不丢的磁盘 + 文件系统。P5 起再接插件加载器，P6 用两个真实驱动验证插件模型。

---

## 上游 ArkOS 0.5.0 同步与重写映射（2026-10-02）

上游 `upstream/ArkOS-0.5.0/` 已落地一条「ATA PIO 传输 + 块读写 + 两仓快照文件系统 + 外部卷」的链路，**部分覆盖了本计划 P0–P4 的目标**，但结构与本计划不一致：驱动硬编码、ATA 代码重复两份、无 Driver API / 插件模型、无 PCI / virtio / AHCI、无 ELF 模块加载器。同步策略：**取功能与踩坑经验，不取其结构**；代码按本计划分层与 `kra_` / `KRA_` 命名重写，落点仍是 Kotlin（K3 之后）。

### 差异对照

| 能力 | 上游 0.5.0 | 本计划 | 动作 |
| --- | --- | --- | --- |
| ATA PIO | `kernel/block.c` 与 `kernel/storage.c` **两份重复**的内联 asm 实现 | P3 单一内置 bootstrap 驱动 | 重写并去重 |
| 块设备 | `BlockDevice{id,present,sectors}` + `block_read/write/flush`，最多 2 台，无 ops/类 | `block_device` + `block_ops` + `/dev/vda` | 重写 |
| 驱动注册 | 无（硬编码） | `kra_driver_register` + `.ark_drivers` 段 | 新增 |
| PCI 枚举 | 无 | P1 `0xCF8/0xCFC` | 新增 |
| virtio-blk / AHCI | 无 | P6 `.ko`（NVMe 不做） | 新增 |
| 文件系统 | ArkFS：`ARKFS1` 超级块 + 双仓 `ARKBANK1` + CRC32 + 原子提交 | ArkFS：超级块 + 位图 + 连续块 + 扁平目录项 | 重写（保留上游双仓/CRC/恢复经验） |
| VFS | 路径式、`is_dir`、`vfs_mkdir/rename/copy`、`name[128]`、`VFS_MAX_FILES=64` | 路径式 VFS + `/dev` + 挂载点 | 采纳能力，重写 |
| 外部卷 | `fat32.c` / `extfs.c` / `ntfs.c` | 不在 P0–P7 | 暂不同步 |
| 内存 / 符号表 | 无 | P0 bump 分配器 + `KRA_EXPORT` 表 | 新增 |
| 模块加载 | 无 | P5 ELF64 ET_REL | 新增 |

### 必须继承的正确性细节（避免重复踩坑）

1. **IDENTIFY 校验**：检查 `identify[49] & 0x200`（LBA）、`identify[83]` 的 FLUSH CACHE 支持、逻辑扇区必须 512、容量上限截断（上游截到 `0x10000000` 扇区）。
2. **传输**：每批 ≤255 扇区；先写 `0x1F6`（设备号 + LBA 高 4 位）再轮询；`lba/count` 越界检查；每扇区 DRQ 后 256 次 `inw/outw`。
3. **提交顺序**：非活动仓正文 → FLUSH → 仓头（generation / 正文 CRC / 提交标记 / 头 CRC）→ FLUSH，成功后才切活动仓——掉电一致性关键。
4. **双仓恢复**：选 generation 最大且校验通过的仓；最新仓损坏时回退旧仓。
5. **端口 I/O 收敛**：上游内联 asm 散落在两个文件；本计划统一到 `boot/*.S` / 单一 `asmio` 封装。
6. **不采用文本序列化快照**：上游把 VFS 文本灌进快照，要求正文无内嵌 NUL（`strlen(data)==size`）；我们的 ArkFS 走块/目录项，二进制安全。

### 同步边界（重要）

- **不整包并入**上游的 accounts / process / net / gpu / arkui 等子系统——它们不属于本计划范围。
- 遵循方案 B：上游 C 实现**只作参考**，不落进本仓库的 C 基线；存储栈从第一天就是 Kotlin（K3 之后开工）。
- 唯一可直接借鉴的**非代码资产**：上游 `docs/STORAGE.md` 的磁盘格式与持久化说明、`tests/storage_test.c` 的主机侧测试思路、`scripts/create-disk.py` 的造盘方式。