#!/usr/bin/env python3
"""打印目标机当前的通用寄存器（含 RIP），不设断点、不改状态。

本机没有 gdb，但 QEMU 的 -gdb 端口直接讲 RSP 协议，只读一次 `g` 包足够定位
「卡在哪个地址」——比从头跑一遍 -d int 干净得多。

用法：ripdump.py  [等待秒数，默认 3]
前提：QEMU 已用 -S -gdb tcp::1234 启动并停在入口。
"""
import socket
import subprocess
import sys
import time

NAMES = ['rax', 'rbx', 'rcx', 'rdx', 'rsi', 'rdi', 'rbp', 'rsp',
         'r8', 'r9', 'r10', 'r11', 'r12', 'r13', 'r14', 'r15', 'rip']


def read_packets(s: socket.socket, deadline: float) -> list:
    """收包直到超时，返回所有 '$<payload>#<cks>' 的 payload 字节串。

    RSP 的坑：
      * QEMU 对每个请求先回一个裸 '+' 确认，真正的回复是下一个包；
      * 一次 recv 可能带回多个包，也可能只带回半个，必须攒够再解析；
      * 回复 payload 不一定是十六进制（错误消息是原始字符串），
        所以不能假设「第一个 $ 包就是我要的」。
    """
    buf = b''
    out = []
    while time.time() < deadline:
        try:
            chunk = s.recv(65536)
        except socket.timeout:
            break
        if not chunk:
            break
        buf += chunk
        while True:
            i = buf.find(b'$')
            if i < 0:
                buf = b''
                break
            j = buf.find(b'#', i)
            if j < 0 or len(buf) < j + 3:
                break
            out.append(buf[i + 1:j])
            buf = buf[j + 3:]
    return out


def main() -> None:
    wait = float(sys.argv[1]) if len(sys.argv) > 1 else 3.0
    s = socket.create_connection(('127.0.0.1', 1234), 5)
    s.settimeout(0.5)

    def send(pkt: str) -> None:
        cks = sum(pkt.encode()) & 0xFF
        s.sendall(b'$' + pkt.encode() + b'#%02x' % cks)
        s.recv(65536)          # 吃掉裸 '+'

    send('qSupported:multiprocess+')
    send('?')

    # QEMU 加了 -S 时 CPU 停在复位态（RIP=0xfff0，还在 BIOS 区），寄存器包虽然
    # 读得到但完全说明不了问题。必须先 c 放行，跑一会儿，再用 Ctrl-C 打断，
    # 停下来的瞬间才拿得到有意义的现场。
    s.sendall(b'c')
    time.sleep(wait)
    s.sendall(b'\x03')            # 中断请求
    read_packets(s, time.time() + 1.0)

    send('g')
    packets = read_packets(s, time.time() + wait)

    # `g` 的回复必须是 17*16 = 272 个十六进制字符
    hexdigits = b'0123456789abcdef'
    body = None
    for p in packets:
        if len(p) >= 272 and all(c in hexdigits for c in p[:272].lower()):
            body = p[:272].lower()
            break
    if body is None:
        print('未找到寄存器包，收到的包：', [p[:40] for p in packets])
        return

    regs = [int.from_bytes(bytes.fromhex(body[i * 16:(i + 1) * 16]), 'little')
            for i in range(17)]
    for n, v in zip(NAMES, regs):
        print('%-4s = %016x' % (n, v))

    # 把 RIP 归到最近的符号上
    out = subprocess.run(['nm', '-n', 'build/kernel.elf'],
                         capture_output=True, text=True).stdout
    syms = []
    for line in out.splitlines():
        p = line.split()
        if len(p) >= 3 and p[1] in 'TtWwVvDdBbRr':
            syms.append((int(p[0], 16), p[2]))
    syms.sort()
    rip = regs[16]
    best = None
    for addr, name in syms:
        if addr <= rip:
            best = (addr, name)
        else:
            break
    if best:
        print('\nRIP 落在 %s + 0x%x' % (best[1], rip - best[0]))
    else:
        print('\nRIP 早于所有已知符号')


if __name__ == '__main__':
    main()