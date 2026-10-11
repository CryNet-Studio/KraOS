# ArkOS 0.1.0 验证记录

验证日期：2026-10-02。主机：x86_64 Linux；QEMU 8.2.2 TCG；256 MiB；标准 VGA；PS/2 输入。

| 验证项 | 结果 |
| --- | --- |
| GCC / ld freestanding 构建，-Werror | 通过 |
| Multiboot2 头检查 | 通过 |
| BIOS 启动至 1024×768 桌面 | 通过 |
| OVMF UEFI 启动至桌面 | 通过 |
| 键盘命令与 RAM 文件创建、读取、删除 | 通过 |
| Notes 编辑后由终端读取内容 | 通过 |
| 功能键切换应用与鼠标打开 Settings | 通过 |
| 鼠标切换浅色外观 | 屏幕转储人工确认 |
| RAM 目录满载时拒绝编辑、释放槽位后恢复 | 通过 |
| Files 超出一屏时随选中项滚动 | 通过，附截图 |
| 创建新文件，重启后确认文件消失 | BIOS / UEFI 均通过 |
| 内核发起 QEMU 关机 | BIOS / UEFI 均通过 |
| 800×600 窗口与提示符位置 | 同版内核的临时低分辨率引导头测试通过 |

最终 ISO SHA-256：

```text
0865bae232888db8cb10b13350cfd3b447ac76094141630bc3fb9ae3eaa190f8
```

完整串口记录与代表性截图位于 `docs/verification/`。
执行完整目录容量测试：`RUN_EDGE=1 python3 tests/boot_test.py bios`。

未验证物理电脑、VirtualBox、VMware、UTM 的具体默认配置。没有硬盘安装或硬件驱动兼容性承诺。
