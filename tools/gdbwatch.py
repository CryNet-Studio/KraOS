#!/usr/bin/env python3
"""
用 QEMU 的 gdbstub 在内核里下断点并读寄存器/内存。

本机没装 gdb，而 gdb 远程协议（RSP）足够简单，直接用 socket 讲就行。
注意两点，都是踩出来的：
  1. QEMU 对每个请求先回一个裸 "+" 确认，真正的回复是下一个包 —— 解析时
     必须把 $ 之前的字节当垃圾丢掉，不能等它们拼成完整包。
  2. TCG 模拟器不支持硬件观察点（Z2/Z3/Z4），只能用软件断点 Z0。

用法：gdbwatch.py <断点地址(hex)>
"""
import socket, sys

HOST, PORT = "127.0.0.1", 1234
BP = int(sys.argv[1], 16) if len(sys.argv) > 1 else 0
VERBOSE = "-v" in sys.argv
DUMPS = [a for a in sys.argv[2:] if ":" in a] or ["1400000:100"]


class RSP:
    def __init__(self, sock):
        self.s = sock
        self.buf = b""

    def _fill(self):
        chunk = self.s.recv(65536)
        if not chunk:
            raise EOFError
        self.buf += chunk

    def send(self, payload: str):
        if VERBOSE:
            print("   -> %s" % payload[:120], flush=True)
        raw = payload.encode()
        self.s.sendall(b"$" + raw + b"#" + ("%02x" % (sum(raw) & 0xFF)).encode())

    def recv(self, timeout=60):
        """收一个数据包，返回负载字符串。$ 之前的裸字节一律丢弃。"""
        self.s.settimeout(timeout)
        while True:
            if b"$" not in self.buf:
                self.buf = b""
                self._fill()
                continue
            body = self.buf.split(b"$", 1)[1]
            if b"#" not in body or len(body.split(b"#", 1)[1]) < 2:
                self._fill()
                continue
            head, _, rest = body.partition(b"#")
            self.buf = rest[2:]
            data = head.decode("latin1")
            # 必须像真 gdb 那样回确认，否则 QEMU 可能在等我们点头才发下一个包。
            self.s.sendall(b"+")
            if VERBOSE:
                print("   <- %s" % data[:120], flush=True)
            # 注意别写 data.startswith("O")：成功回复就是 "OK"，以 O 开头，
            # 会被误判成控制台包然后一直等下一个包。必须要求后面还有内容。
            if data.startswith("O") and len(data) > 1 and all(
                    c in "0123456789abcdefABCDEF" for c in data[1:]):
                try:
                    sys.stdout.write(bytes.fromhex(data[1:]).decode("latin1", "replace"))
                    sys.stdout.flush()
                except ValueError:
                    pass
                continue
            return data


def parse_regs(h):
    names = ["rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp", "rsp",
             "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15", "rip"]
    # g 包里的寄存器是「目标机字节序」（x86_64 即小端）逐字节编码的，
    # 按大端整数读会得到 0x8b6e0101... 这种明显错位的值。
    vals = [int.from_bytes(bytes.fromhex(h[i:i + 16]), "little")
            for i in range(0, len(h) - 15, 16)]
    return dict(zip(names, vals))


def main():
    s = socket.create_connection((HOST, PORT), timeout=10)
    r = RSP(s)

    r.send("qSupported:multiprocess+;xmlRegisters=i386;qRelocInsn+")
    r.recv()
    r.send("?")
    print("初始状态:", r.recv())

    r.send("Z0,%x,1" % BP)
    reply = r.recv()
    print("在 0x%x 下断点 ->" % BP, reply)
    if reply != "OK":
        return 1

    r.send("c")
    print("停住:", r.recv(timeout=300))

    r.send("g")
    regs = parse_regs(r.recv())
    for k in ("rip", "rsp", "rbp", "rbx", "rax", "rcx", "rdi", "rsi", "r12", "r13"):
        print("  %-4s = 0x%016x" % (k, regs[k]))

    for spec in DUMPS:
        addr, length = (int(x, 16) for x in spec.split(":"))
        r.send("m%x,%x" % (addr, length))
        raw = r.recv()
        for off in range(0, len(raw), 32):
            chunk = raw[off:off + 32]
            vals = [int.from_bytes(bytes.fromhex(chunk[j:j + 16]), "little")
                    for j in range(0, len(chunk), 16)]
            print("    0x%08x: %s" % (addr + off // 2,
                  "  ".join("%016x" % v for v in vals)))
    s.close()
    return 0


sys.exit(main())
