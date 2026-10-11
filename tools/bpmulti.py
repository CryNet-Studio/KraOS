#!/usr/bin/env python3
"""
同时在多个函数入口下断点，连续运行并记录每次命中的 (rip, rbx)，
用来找出「谁破坏了被调用者保存寄存器 rbx」。

TCG 不支持硬件观察点，只能软件断点；所以一次只能命��到一处、逐次记录。

用法：bpmulti.py <最多命中次数> <地址1> <地址2> ...
"""
import socket, sys, re

HOST, PORT = "127.0.0.1", 1234
LIMIT = int(sys.argv[1])
ADDRS = [int(a, 16) for a in sys.argv[2:]]


class RSP:
    def __init__(self, sock):
        self.s = sock
        self.buf = b""

    def _fill(self):
        c = self.s.recv(65536)
        if not c:
            raise EOFError
        self.buf += c

    def send(self, p):
        raw = p.encode()
        self.s.sendall(b"$" + raw + b"#" + ("%02x" % (sum(raw) & 0xFF)).encode())

    def recv(self, timeout=180):
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
        n = ["rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp", "rsp",
             "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15", "rip"]
        return dict(zip(n, [int.from_bytes(bytes.fromhex(h[i:i + 16]), "little")
                            for i in range(0, len(h) - 15, 16)]))


s = socket.create_connection((HOST, PORT), timeout=10)
r = RSP(s)
r.send("qSupported:multiprocess+;xmlRegisters=i386;qRelocInsn+")
r.recv()
r.send("?")
r.recv()

for a in ADDRS:
    r.send("Z0,%x,1" % a)
    ok = r.recv()
    if ok != "OK":
        print("0x%x 下断点失败: %s" % (a, ok))

r.send("c")
last = None
for i in range(LIMIT):
    stop = r.recv()
    g = r.regs()
    tag = ""
    if last is not None and last != g["rbx"]:
        tag = "   <<< rbx 0x%x -> 0x%x" % (last, g["rbx"])
    print("0x%012x  rbx=0x%012x  rdi=0x%x%s" % (g["rip"], g["rbx"], g["rdi"], tag))
    last = g["rbx"]
    r.send("c")

s.close()
