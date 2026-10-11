#!/usr/bin/env python3
"""
逐次跟踪 operator new（_Znwm）的入参和返回值，找出第一次返回野指针的分配。

做法：把断点下在 _Znwm 入口，读到大小 n 和返回地址 RA；撤掉入口断点、把断点
挪到 RA 上，读到返回值后打印 (n, 返回值)；然后重新下入口断点继续。TCG 模拟器
不支持硬件观察点，只能这么来回倒腾。

用法：newtrace.py <_Znwm 地址> [最多跟踪多少次]
"""
import socket, sys

HOST, PORT = "127.0.0.1", 1234
NEW = int(sys.argv[1], 16)
LIMIT = int(sys.argv[2]) if len(sys.argv) > 2 else 40


class RSP:
    def __init__(self, sock):
        self.s = sock
        self.buf = b""

    def _fill(self):
        chunk = self.s.recv(65536)
        if not chunk:
            raise EOFError
        self.buf += chunk

    def send(self, payload):
        raw = payload.encode()
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
            # 别写 data.startswith("O")：成功回复就是 "OK"，会被误判。
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

    def mem(self, addr, n):
        self.send("m%x,%x" % (addr, n))
        return bytes.fromhex(self.recv())


s = socket.create_connection((HOST, PORT), timeout=10)
r = RSP(s)
r.send("qSupported:multiprocess+;xmlRegisters=i386;qRelocInsn+")
r.recv()
r.send("?")
r.recv()

HEAP_LO, HEAP_HI = 0x1400000, 0x1600000


def classify(p):
    if p == 0:
        return "空"
    if HEAP_LO <= p < HEAP_HI:
        return "堆内"
    if 0x1250000 <= p < 0x1260000:
        return "*** 栈上 ***"
    if 0x1200000 <= p < 0x1202000:
        return "*** TLS 块 ***"
    return "其它"


for i in range(LIMIT):
    r.send("Z0,%x,1" % NEW)
    if r.recv() != "OK":
        print("下入口断点失败")
        break
    r.send("c")
    r.recv()
    g = r.regs()
    size = g["rdi"]
    ra = int.from_bytes(r.mem(g["rsp"], 8), "little")
    r.send("z0,%x,1" % NEW)
    r.recv()
    r.send("Z0,%x,1" % ra)
    if r.recv() != "OK":
        print("下返回点断点失败 ra=0x%x" % ra)
        break
    r.send("c")
    r.recv()
    g2 = r.regs()
    r.send("z0,%x,1" % ra)
    r.recv()
    ret = g2["rax"]
    print("#%02d  new(%d) = 0x%012x  %s" % (i + 1, size, ret, classify(ret)))
    if "***" in classify(ret) and size:
        print("      ^^^ 野指针，调用点 rip=0x%x" % (ra - 5))
        break

s.close()
