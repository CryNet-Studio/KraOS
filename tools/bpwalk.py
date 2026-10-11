#!/usr/bin/env python3
"""
在一串地址上依次下断点并继续，打印每处的寄存器，用来定位「寄存器从哪一步开始变野」。

TCG 不支持硬件观察点，只能靠软件断点一跳一跳走。

用法：bpwalk.py <地址1> <地址2> ...   （地址用十六进制）
"""
import socket, sys

HOST, PORT = "127.0.0.1", 1234
ADDRS = [int(a, 16) for a in sys.argv[1:]]
SHOW = ("rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp", "rsp", "r12", "r13", "r14", "r15")


class RSP:
    def __init__(self, sock):
        self.s = sock
        self.buf = b""

    def _fill(self):
        chunk = self.s.recv(65536)
        if not chunk:
            raise EOFError
        self.buf += chunk

    def send(self, p):
        raw = p.encode()
        self.s.sendall(b"$" + raw + b"#" + ("%02x" % (sum(raw) & 0xFF)).encode())

    def recv(self, timeout=120):
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
            self.s.sendall(b"+")
            if data.startswith("O") and len(data) > 1 and all(
                    c in "0123456789abcdefABCDEF" for c in data[1:]):
                continue
            return data

    def regs(self):
        self.send("g")
        h = self.recv()
        names = ["rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp", "rsp",
                 "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15", "rip"]
        return dict(zip(names, [int.from_bytes(bytes.fromhex(h[i:i + 16]), "little")
                                for i in range(0, len(h) - 15, 16)]))


s = socket.create_connection((HOST, PORT), timeout=10)
r = RSP(s)
r.send("qSupported:multiprocess+;xmlRegisters=i386;qRelocInsn+")
r.recv()
r.send("?")
r.recv()

prev = None
for a in ADDRS:
    r.send("Z0,%x,1" % a)
    if r.recv() != "OK":
        print("0x%x 下断点失败" % a)
        break
    r.send("c")
    try:
        r.recv(timeout=8)      # 有些跳转分支根本走不到，跳过即可
    except Exception:
        r.send("z0,%x,1" % a); r.recv()
        print("0x%012x  (未命中，跳过)" % a)
        prev = None
        continue
    g = r.regs()
    r.send("z0,%x,1" % a)
    r.recv()
    changed = ""
    if prev:
        changed = "  变化: " + " ".join(
            "%s:0x%x->0x%x" % (k, prev[k], g[k]) for k in SHOW if prev[k] != g[k])
    print("0x%012x  %s" % (a, "  ".join("%s=0x%x" % (k, g[k]) for k in SHOW)))
    if changed:
        print("     " + changed)
    prev = g

s.close()
