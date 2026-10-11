# Boot and platform notes

ArkOS is a freestanding x86-64 kernel loaded by GRUB 2 using Multiboot2. It is
intended for the QEMU `pc` machine with `-vga std`, 256 MiB of RAM, one CPU, and
its built-in PS/2 keyboard/mouse. BIOS and OVMF UEFI boots are supported; Secure
Boot is not configured. No BIOS/UEFI services are called by the running kernel.

## Handoff

`entry.S` requests a 1024 × 768, 32-bit framebuffer and enters at the 32-bit
Multiboot2 entry point. It checks for long-mode support, builds a 4 GiB identity
map with 2 MiB pages, enables PAE and long mode, installs a small GDT, and calls:

```c
void kernel_main(uint32_t multiboot_magic, uint32_t multiboot_info_address);
```

The Multiboot header is kept within the first 32 KiB of the ELF file. The image
is linked at physical address 1 MiB. GRUB initializes the ELF NOBITS sections,
including the static canvas and event buffers. The bootstrap owns a 128 KiB
stack. There is no higher-half mapping, paging allocator, user mode, or process
isolation. The first 4 GiB are supervisor writable; page permissions are not a
security boundary in this prototype.

`platform.c` checks the boot magic, tag boundaries, RGB framebuffer pitch and
channel layout, and memory-map entry sizes. Framebuffer storage must fit below
4 GiB. The desktop imposes the narrower geometry limit of 800 × 600 through
1920 × 1200. “Usable RAM” is the sum of type-1 memory-map ranges, not current free
memory and not a count of installed RAM modules.

## Interrupt and input ABI

All C sources must be built with `-mno-red-zone -mgeneral-regs-only` in addition
to the freestanding and non-PIE flags. `interrupts.S` saves every general register,
normalizes exception error codes, clears DF, aligns the stack for the System V
AMD64 C calling convention, and restores state with `iretq`.

The 8259 PIC maps IRQs to vectors 32–47. Only the PIT, keyboard, PIC cascade, and
mouse IRQs are unmasked. The PIT runs at approximately 100 Hz. Interrupt code
writes to a fixed 256-slot event ring; foreground consumption runs with IRQs
briefly disabled. On overflow the oldest event is discarded so the newest mouse
button state remains available. Idle uses `cli` / queue check / `sti; hlt` to
avoid losing the wake-up for an input event that is already queued.

The i8042 translation mode supplies set-1 keyboard scancodes. ASCII input uses a
US keyboard layout; Shift, Caps Lock, Control, arrows, Enter, Escape, Backspace,
Delete, Tab, and F1–F4 are decoded. Mouse input uses the basic three-byte PS/2
protocol (no wheel or absolute pointer support). USB and Bluetooth drivers are
not included. QEMU captures the relative mouse; the host's QEMU release shortcut
can be used to release it.

## Clock, reset, and diagnostics

The clock reads stable CMOS snapshots and handles BCD/binary and 12/24-hour
formats. The display shows the VM RTC directly; the provided QEMU configuration
uses UTC. ArkOS has no timezone database or clock synchronization.

COM1 outputs 115200-baud boot messages and fatal exception details, including RIP,
error code, and CR2 for page faults. Reboot requests an i8042 reset, with a triple
fault fallback. Shutdown writes the QEMU PC ACPI PM control port (and the old
Bochs-compatible port). This is a QEMU-targeted shutdown path, not a general
ACPI implementation for physical computers.

## Primary references

- [GNU Multiboot2 specification](https://www.gnu.org/software/grub/manual/multiboot2/multiboot.html)
- [Intel 64 and IA-32 architecture manuals](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html)
- [QEMU PC machine documentation](https://www.qemu.org/docs/master/system/i386/pc.html)
- [QEMU standard VGA documentation](https://www.qemu.org/docs/master/specs/standard-vga.html)
