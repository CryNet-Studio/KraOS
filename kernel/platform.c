/* ArkOS x86_64 platform. Target: QEMU PC, standard VGA, i8042 PS/2.
 * No firmware calls are made after GRUB hands control to the kernel.
 */
#include "ark.h"

static inline void out8(uint16_t port, uint8_t value) {
    __asm__ volatile("outb %0,%1" : : "a"(value), "Nd"(port));
}
static inline uint8_t in8(uint16_t port) {
    uint8_t value;
    __asm__ volatile("inb %1,%0" : "=a"(value) : "Nd"(port));
    return value;
}
static inline void out16(uint16_t port, uint16_t value) {
    __asm__ volatile("outw %0,%1" : : "a"(value), "Nd"(port));
}
static inline void io_wait(void) { out8(0x80, 0); }
static inline uint64_t irq_save(void) {
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
    return flags;
}
static inline void irq_restore(uint64_t flags) {
    if (flags & (1u << 9)) __asm__ volatile("sti" : : : "memory");
}

static void serial_byte(char c) {
    /* Bounded polling also allows a machine without COM1 to boot. */
    unsigned tries = 100000;
    while (!(in8(0x3fd) & 0x20) && --tries) { }
    if (tries) out8(0x3f8, (uint8_t)c);
}
void serial_write(const char *s) {
    while (*s) {
        if (*s == '\n') serial_byte('\r');
        serial_byte(*s++);
    }
}
static void serial_number(uint64_t value) {
    char buffer[24];
    uint_to_str(value, buffer);
    serial_write(buffer);
}
static void serial_hex(uint64_t value) {
    const char *digits = "0123456789abcdef";
    char buffer[19];
    buffer[0] = '0'; buffer[1] = 'x';
    for (int i = 0; i < 16; ++i)
        buffer[2 + i] = digits[(value >> (60 - i * 4)) & 15];
    buffer[18] = 0;
    serial_write(buffer);
}
static __attribute__((noreturn)) void halt_forever(void) {
    for (;;) __asm__ volatile("cli; hlt" : : : "memory");
}
static __attribute__((noreturn)) void panic(const char *reason) {
    serial_write("[fatal] "); serial_write(reason); serial_write("\n");
    halt_forever();
}

typedef struct __attribute__((packed)) {
    uint32_t type, size;
} MBTag;
typedef struct __attribute__((packed)) {
    uint32_t type, size;
    uint64_t address;
    uint32_t pitch, width, height;
    uint8_t bpp, kind;
    uint16_t reserved;
    uint8_t red_pos, red_size, green_pos, green_size, blue_pos, blue_size;
} MBFramebuffer;
typedef struct __attribute__((packed)) {
    uint32_t type, size, entry_size, entry_version;
} MBMemoryMap;
typedef struct __attribute__((packed)) {
    uint64_t base, length;
    uint32_t type, reserved;
} MBMemoryEntry;

static void parse_multiboot(uint32_t magic, uint32_t address, BootInfo *info) {
    if (magic != 0x36d76289) panic("Missing Multiboot2 boot magic.");
    if (!address || (address & 7) || address > 0xfffffff7u)
        panic("Invalid Multiboot2 information address.");
    const uint8_t *base = (const uint8_t *)(uintptr_t)address;
    uint32_t total = *(const uint32_t *)base;
    if (total < 16 || total > 16 * 1024 * 1024 || (total & 7) ||
        (uint64_t)address + total > 0x100000000ull)
        panic("Invalid Multiboot2 information length.");
    memset(info, 0, sizeof(*info));
    bool found_end = false, found_fb = false, found_map = false;
    uint64_t usable = 0;
    for (uint32_t offset = 8; offset <= total - 8;) {
        const MBTag *tag = (const MBTag *)(base + offset);
        if (tag->size < 8 || tag->size > total - offset)
            panic("Malformed Multiboot2 tag.");
        if (tag->type == 0) {
            if (tag->size != 8) panic("Malformed Multiboot2 end tag.");
            found_end = true;
            break;
        }
        if (tag->type == 8) {
            const MBFramebuffer *fb = (const MBFramebuffer *)tag;
            if (tag->size < sizeof(*fb) || fb->kind != 1 || fb->bpp != 32)
                panic("A 32-bit RGB framebuffer is required; use QEMU -vga std.");
            if (fb->width < 640 || fb->height < 480 ||
                fb->width > 4096 || fb->height > 2160 ||
                fb->pitch < fb->width * 4 || (fb->pitch & 3))
                panic("Unsupported framebuffer geometry or pitch.");
            uint64_t bytes = (uint64_t)fb->pitch * fb->height;
            if (!fb->address || fb->address >= 0x100000000ull ||
                bytes > 0x100000000ull - fb->address)
                panic("Framebuffer must fit below the 4 GiB identity-map limit.");
            if (fb->red_size != 8 || fb->green_size != 8 || fb->blue_size != 8 ||
                fb->red_pos > 24 || fb->green_pos > 24 || fb->blue_pos > 24)
                panic("Unsupported framebuffer channel layout.");
            uint32_t rm = 255u << fb->red_pos, gm = 255u << fb->green_pos,
                     bm = 255u << fb->blue_pos;
            if ((rm & gm) || (rm & bm) || (gm & bm))
                panic("Overlapping framebuffer color channels.");
            info->framebuffer = fb->address;
            info->width = fb->width; info->height = fb->height;
            info->pitch = fb->pitch; info->bpp = fb->bpp;
            info->red_pos = fb->red_pos; info->red_size = fb->red_size;
            info->green_pos = fb->green_pos; info->green_size = fb->green_size;
            info->blue_pos = fb->blue_pos; info->blue_size = fb->blue_size;
            found_fb = true;
        } else if (tag->type == 6) {
            const MBMemoryMap *map = (const MBMemoryMap *)tag;
            if (tag->size < sizeof(*map) || map->entry_size < sizeof(MBMemoryEntry) ||
                map->entry_version != 0 ||
                (tag->size - sizeof(*map)) % map->entry_size)
                panic("Malformed Multiboot2 memory map.");
            for (uint32_t at = sizeof(*map); at < tag->size; at += map->entry_size) {
                const MBMemoryEntry *entry = (const MBMemoryEntry *)((const uint8_t *)tag + at);
                if (entry->type == 1) {
                    if (entry->length > UINT64_MAX - usable)
                        panic("Multiboot2 memory size overflow.");
                    usable += entry->length;
                }
            }
            found_map = true;
        }
        uint32_t next = (tag->size + 7u) & ~7u;
        if (next > total - offset) panic("Unaligned Multiboot2 tag boundary.");
        offset += next;
    }
    if (!found_end) panic("Multiboot2 information has no end tag.");
    if (!found_fb) panic("GRUB did not supply a graphics framebuffer.");
    if (!found_map) serial_write("[boot] Warning: no physical memory map supplied.\n");
    info->memory_mib = usable / (1024 * 1024);
    serial_write("[boot] Framebuffer "); serial_number(info->width);
    serial_write("x"); serial_number(info->height); serial_write("x32 at ");
    serial_hex(info->framebuffer); serial_write("; usable RAM ");
    serial_number(info->memory_mib); serial_write(" MiB\n");
}

typedef struct __attribute__((packed)) {
    uint16_t low, selector;
    uint8_t ist, attributes;
    uint16_t middle;
    uint32_t high, reserved;
} IDTEntry;
typedef struct __attribute__((packed)) { uint16_t limit; uint64_t base; } IDTPointer;
static IDTEntry idt[256] __attribute__((aligned(16)));
extern void *isr_stub_table[256];
static void init_idt(void) {
    for (unsigned i = 0; i < 256; ++i) {
        uint64_t target = (uint64_t)(uintptr_t)isr_stub_table[i];
        idt[i] = (IDTEntry){(uint16_t)target, 8, 0, 0x8e,
                           (uint16_t)(target >> 16), (uint32_t)(target >> 32), 0};
    }
    IDTPointer descriptor = {(uint16_t)(sizeof(idt) - 1), (uint64_t)(uintptr_t)idt};
    __asm__ volatile("lidt %0" : : "m"(descriptor));
}
static void init_pic(void) {
    out8(0x20, 0x11); io_wait(); out8(0xa0, 0x11); io_wait();
    out8(0x21, 0x20); io_wait(); out8(0xa1, 0x28); io_wait();
    out8(0x21, 4); io_wait(); out8(0xa1, 2); io_wait();
    out8(0x21, 1); io_wait(); out8(0xa1, 1); io_wait();
    out8(0x21, 0xff); out8(0xa1, 0xff);
}
static volatile uint64_t ticks;
#define EVENT_CAP 256u
static Event events[EVENT_CAP];
static volatile unsigned event_head, event_tail;
static void enqueue(Event event) {
    unsigned next = (event_head + 1) % EVENT_CAP;
    /* Retain the latest state, including a mouse-button release, on overflow. */
    if (next == event_tail) event_tail = (event_tail + 1) % EVENT_CAP;
    events[event_head] = event;
    __asm__ volatile("" : : : "memory");
    event_head = next;
}
bool platform_next_event(Event *event) {
    uint64_t flags = irq_save();
    bool available = event_tail != event_head;
    if (available) {
        *event = events[event_tail];
        event_tail = (event_tail + 1) % EVENT_CAP;
    }
    irq_restore(flags);
    return available;
}
uint64_t platform_ticks(void) { return ticks; }
void platform_idle(void) {
    /* CLI/check/STI/HLT prevents sleeping past an already queued input event.
     * The CPU defers an IRQ until after the instruction following STI.
     */
    __asm__ volatile("cli" : : : "memory");
    if (event_head == event_tail)
        __asm__ volatile("sti; hlt" : : : "memory");
    else
        __asm__ volatile("sti" : : : "memory");
}

static const unsigned char key_plain[128] = {
    [0x02]='1',[0x03]='2',[0x04]='3',[0x05]='4',[0x06]='5',[0x07]='6',
    [0x08]='7',[0x09]='8',[0x0a]='9',[0x0b]='0',[0x0c]='-',[0x0d]='=',
    [0x10]='q',[0x11]='w',[0x12]='e',[0x13]='r',[0x14]='t',[0x15]='y',
    [0x16]='u',[0x17]='i',[0x18]='o',[0x19]='p',[0x1a]='[',[0x1b]=']',
    [0x1e]='a',[0x1f]='s',[0x20]='d',[0x21]='f',[0x22]='g',[0x23]='h',
    [0x24]='j',[0x25]='k',[0x26]='l',[0x27]=';',[0x28]='\'',[0x29]='`',
    [0x2b]='\\',[0x2c]='z',[0x2d]='x',[0x2e]='c',[0x2f]='v',[0x30]='b',
    [0x31]='n',[0x32]='m',[0x33]=',',[0x34]='.',[0x35]='/',[0x37]='*',
    [0x39]=' ',[0x4a]='-',[0x4e]='+'
};
static const unsigned char key_shifted[128] = {
    [0x02]='!',[0x03]='@',[0x04]='#',[0x05]='$',[0x06]='%',[0x07]='^',
    [0x08]='&',[0x09]='*',[0x0a]='(',[0x0b]=')',[0x0c]='_',[0x0d]='+',
    [0x1a]='{',[0x1b]='}',[0x27]=':',[0x28]='"',[0x29]='~',[0x2b]='|',
    [0x33]='<',[0x34]='>',[0x35]='?'
};
static uint8_t shift_bits, ctrl_bits;
static bool caps_lock, caps_down, extended;
static unsigned pause_bytes;
static void keyboard_byte(uint8_t byte) {
    if (pause_bytes) { --pause_bytes; return; }
    if (byte == 0xe1) { pause_bytes = 5; return; }
    if (byte == 0xe0) { extended = true; return; }
    bool ext = extended, released = (byte & 0x80) != 0;
    extended = false;
    uint8_t sc = byte & 0x7f;
    if (!ext && (sc == 0x2a || sc == 0x36)) {
        uint8_t bit = sc == 0x2a ? 1 : 2;
        if (released) shift_bits &= (uint8_t)~bit; else shift_bits |= bit;
        return;
    }
    if (sc == 0x1d) {
        uint8_t bit = ext ? 2 : 1;
        if (released) ctrl_bits &= (uint8_t)~bit; else ctrl_bits |= bit;
        return;
    }
    if (!ext && sc == 0x3a) {
        if (!released && !caps_down) caps_lock = !caps_lock;
        caps_down = !released;
        return;
    }
    if (released) return;
    int key = 0;
    if (sc == 0x1c) key = KEY_ENTER;
    else if (ext) {
        switch (sc) {
        case 0x48: key = KEY_UP; break; case 0x50: key = KEY_DOWN; break;
        case 0x4b: key = KEY_LEFT; break; case 0x4d: key = KEY_RIGHT; break;
        case 0x53: key = KEY_DELETE; break; case 0x35: key = '/'; break;
        default: break;
        }
    } else {
        switch (sc) {
        case 0x01: key = KEY_ESCAPE; break; case 0x0e: key = KEY_BACKSPACE; break;
        case 0x0f: key = KEY_TAB; break; case 0x3b: key = KEY_F1; break;
        case 0x3c: key = KEY_F2; break; case 0x3d: key = KEY_F3; break;
        case 0x3e: key = KEY_F4; break;
        default:
            key = key_plain[sc];
            if (key >= 'a' && key <= 'z') {
                if (ctrl_bits) key = key - 'a' + 1;
                else if ((shift_bits != 0) != caps_lock) key -= 'a' - 'A';
            } else if (shift_bits && key_shifted[sc]) key = key_shifted[sc];
            break;
        }
    }
    if (key) enqueue((Event){EV_KEY, key, 0, 0, 0});
}
static uint8_t mouse_packet[3], mouse_at;
static void mouse_byte(uint8_t byte) {
    if (!mouse_at && !(byte & 8)) return; /* Packet synchronization bit. */
    mouse_packet[mouse_at++] = byte;
    if (mouse_at != 3) return;
    mouse_at = 0;
    uint8_t flags = mouse_packet[0];
    int dx = (int)mouse_packet[1] - ((flags & 0x10) ? 256 : 0);
    int dy = (int)mouse_packet[2] - ((flags & 0x20) ? 256 : 0);
    if (flags & 0x40) dx = 0;
    if (flags & 0x80) dy = 0;
    /* PS/2 uses positive-up Y; desktop coordinates use positive-down Y. */
    enqueue((Event){EV_MOUSE, 0, dx, -dy, (uint8_t)(flags & 7)});
}
static void drain_input(void) {
    for (unsigned i = 0; i < 64; ++i) {
        uint8_t status = in8(0x64);
        if (!(status & 1)) break;
        uint8_t byte = in8(0x60);
        if (status & 0xc0) continue; /* Corrupt byte: timeout/parity error. */
        if (status & 0x20) mouse_byte(byte); else keyboard_byte(byte);
    }
}
static bool ps2_wait_write(void) {
    for (unsigned i = 0; i < 100000; ++i)
        if (!(in8(0x64) & 2)) return true;
    return false;
}
static bool ps2_command(uint8_t command) {
    if (!ps2_wait_write()) return false;
    out8(0x64, command);
    return true;
}
static bool ps2_data(uint8_t data) {
    if (!ps2_wait_write()) return false;
    out8(0x60, data);
    return true;
}
static bool ps2_read(bool auxiliary, uint8_t *value) {
    for (unsigned i = 0; i < 100000; ++i) {
        uint8_t status = in8(0x64);
        if (!(status & 1)) continue;
        uint8_t byte = in8(0x60);
        if ((status & 0xc0) || ((status & 0x20) != (auxiliary ? 0x20 : 0))) continue;
        *value = byte;
        return true;
    }
    return false;
}
static bool ps2_device_command(bool auxiliary, uint8_t value) {
    for (int attempt = 0; attempt < 3; ++attempt) {
        uint8_t response;
        if (auxiliary && !ps2_command(0xd4)) return false;
        if (!ps2_data(value) || !ps2_read(auxiliary, &response)) return false;
        if (response == 0xfa) return true;
        if (response != 0xfe) return false;
    }
    return false;
}
static void init_ps2(void) {
    ps2_command(0xad); ps2_command(0xa7);
    for (unsigned i = 0; i < 256 && (in8(0x64) & 1); ++i) (void)in8(0x60);
    uint8_t config = 0;
    if (!ps2_command(0x20) || !ps2_read(false, &config)) {
        serial_write("[input] Warning: i8042 controller unavailable.\n");
        return;
    }
    /* Translation converts keyboard set 2 into the set 1 decoded above. */
    config = (uint8_t)((config | 0x40) & ~(0x01 | 0x02));
    if (!ps2_command(0x60) || !ps2_data(config)) return;
    ps2_command(0xae); ps2_command(0xa8);
    bool keyboard = ps2_device_command(false, 0xf6) && ps2_device_command(false, 0xf4);
    bool mouse = ps2_device_command(true, 0xf6) && ps2_device_command(true, 0xf4);
    config = (uint8_t)((config | 0x43) & ~0x30);
    ps2_command(0x60); ps2_data(config);
    serial_write(keyboard ? "[input] PS/2 keyboard ready\n" : "[input] Warning: keyboard did not acknowledge setup.\n");
    serial_write(mouse ? "[input] PS/2 mouse ready\n" : "[input] Warning: mouse did not acknowledge setup.\n");
}

/* Matches the register pushes in interrupts.S. All interrupt C code is GP-only. */
typedef struct {
    uint64_t r15,r14,r13,r12,r11,r10,r9,r8,rsi,rdi,rbp,rdx,rcx,rbx,rax;
    uint64_t vector,error,rip,cs,rflags,rsp,ss;
} InterruptFrame;
void interrupt_dispatch(const InterruptFrame *frame) {
    unsigned vector = (unsigned)frame->vector;
    if (vector < 32) {
        static const char *names[32] = {
            "divide error", "debug", "NMI", "breakpoint", "overflow", "bound range",
            "invalid opcode", "device unavailable", "double fault", "coprocessor overrun",
            "invalid TSS", "segment not present", "stack fault", "general protection",
            "page fault", "reserved", "x87 fault", "alignment check", "machine check",
            "SIMD fault", "virtualization", "control protection", "reserved", "reserved",
            "reserved", "reserved", "reserved", "reserved", "hypervisor", "VMM communication",
            "security", "reserved"
        };
        serial_write("[exception] "); serial_write(names[vector]);
        serial_write(" vector="); serial_number(vector);
        serial_write(" error="); serial_hex(frame->error);
        serial_write(" rip="); serial_hex(frame->rip);
        if (vector == 14) {
            uint64_t cr2; __asm__ volatile("mov %%cr2,%0" : "=r"(cr2));
            serial_write(" cr2="); serial_hex(cr2);
        }
        serial_write("\n"); halt_forever();
    }
    if (vector < 32 || vector >= 48) return;
    /* Spurious IRQ7/15 have no matching ISR bit to acknowledge. */
    if (vector == 39 || vector == 47) {
        uint16_t port = vector == 39 ? 0x20 : 0xa0;
        out8(port, 0x0b);
        if (!(in8(port) & 0x80)) {
            if (vector == 47) out8(0x20, 0x20);
            return;
        }
    }
    if (vector == 32) ++ticks;
    if (vector == 33 || vector == 44) drain_input();
    if (vector >= 40) out8(0xa0, 0x20);
    out8(0x20, 0x20);
}

void platform_init(uint32_t magic, uint32_t mb_addr, BootInfo *info) {
    __asm__ volatile("cli" : : : "memory");
    /* Install our 64-bit IDT before parsing any boot data. */
    init_idt();
    serial_write("[boot] Long mode active; 64-bit IDT installed\n");
    parse_multiboot(magic, mb_addr, info);
    init_pic();
    init_ps2();
    const unsigned divisor = 1193182u / 100u;
    out8(0x43, 0x36); out8(0x40, (uint8_t)divisor); out8(0x40, (uint8_t)(divisor >> 8));
    /* IRQ0 timer, IRQ1 keyboard, IRQ2 cascade, IRQ12 mouse. */
    out8(0x21, 0xf8); out8(0xa1, 0xef);
    serial_write("[boot] PIT 100 Hz; interrupts enabled\n");
    __asm__ volatile("sti" : : : "memory");
}

static uint8_t cmos_read(uint8_t reg) {
    out8(0x70, (uint8_t)(0x80 | reg));
    io_wait();
    return in8(0x71);
}
void platform_time(int *h, int *m, int *s) {
    uint64_t flags = irq_save();
    uint8_t hour = 0, minute = 0, second = 0, format = 2;
    bool valid = false;
    /* Two identical snapshots outside update-in-progress avoid torn times. */
    for (unsigned attempt = 0; attempt < 8 && !valid; ++attempt) {
        unsigned spins = 100000;
        while ((cmos_read(0x0a) & 0x80) && --spins) { }
        if (!spins) break;
        second = cmos_read(0); minute = cmos_read(2); hour = cmos_read(4);
        format = cmos_read(0x0b);
        if (cmos_read(0x0a) & 0x80) continue;
        valid = second == cmos_read(0) && minute == cmos_read(2) && hour == cmos_read(4);
    }
    out8(0x70, 0x0d); /* Release the temporary NMI mask. */
    irq_restore(flags);
    if (!valid) { *h = *m = *s = 0; return; }
    bool pm = (hour & 0x80) != 0;
    hour &= 0x7f;
    if (!(format & 4)) {
        second = (uint8_t)((second & 15) + (second >> 4) * 10);
        minute = (uint8_t)((minute & 15) + (minute >> 4) * 10);
        hour = (uint8_t)((hour & 15) + (hour >> 4) * 10);
    }
    if (!(format & 2)) hour = (uint8_t)(hour % 12 + (pm ? 12 : 0));
    *h = hour % 24; *m = minute % 60; *s = second % 60;
}
void platform_reboot(void) {
    serial_write("[power] Reboot requested\n");
    __asm__ volatile("cli" : : : "memory");
    if (ps2_wait_write()) out8(0x64, 0xfe);
    /* If the controller does not reset, force a triple fault. */
    IDTPointer empty = {0, 0};
    __asm__ volatile("lidt %0; int3" : : "m"(empty) : "memory");
    halt_forever();
}
void platform_poweroff(void) {
    serial_write("[power] Power off requested (QEMU PC ACPI port)\n");
    /* This is explicitly a QEMU-PC poweroff, not a general ACPI interpreter. */
    out16(0x604, 0x2000);
    out16(0xb004, 0x2000); /* Older QEMU/Bochs-compatible machines. */
    serial_write("[power] Machine did not power off; CPU halted.\n");
    halt_forever();
}
