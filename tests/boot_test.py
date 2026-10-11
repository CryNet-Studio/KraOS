#!/usr/bin/env python3
"""Boot the real ISO in QEMU TCG; drive its PS/2 input through QMP."""
from pathlib import Path
import json,os,socket,subprocess,time,sys
ROOT=Path(__file__).resolve().parents[1]
MODE=sys.argv[1] if len(sys.argv)>1 else 'bios'
OUT=ROOT/'build'/('test-'+MODE); OUT.mkdir(parents=True,exist_ok=True)
QEMU=os.environ.get('QEMU_BIN','qemu-system-x86_64')
class VM:
 def __init__(self):
  self.sockpath=OUT/'qmp.sock'; self.sockpath.unlink(missing_ok=True)
  self.log=OUT/'serial.log';self.log.write_text('')
  args=[QEMU,'-machine','pc','-accel','tcg','-m','256M','-cdrom',str(ROOT/'build/arkos-0.1.0.iso'),'-boot','d','-vga','std','-nic','none','-display','none','-serial','file:'+str(self.log),'-qmp','stdio']
  if os.environ.get('QEMU_DATA'):args+=['-L',os.environ['QEMU_DATA']]
  if MODE=='uefi':
   code=os.environ.get('OVMF_CODE','/usr/share/OVMF/OVMF_CODE_4M.fd')
   args+=['-drive','if=pflash,format=raw,readonly=on,file='+code]
  self.err=open(OUT/'qemu.log','w');self.p=subprocess.Popen(args,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=self.err)
  self.p.stdout.readline();self.q('qmp_capabilities')
 def q(self,cmd,args=None):
  self.p.stdin.write((json.dumps({'execute':cmd,'arguments':args or {}})+'\n').encode());self.p.stdin.flush()
  while True:
   r=json.loads(self.p.stdout.readline())
   if 'error' in r:raise RuntimeError(r)
   if 'return' in r:return r['return']
 def wait(self,needle,timeout=30):
  until=time.time()+timeout
  while needle not in self.log.read_text():
   if self.p.poll() is not None:raise RuntimeError('QEMU exited: '+self.log.read_text())
   if time.time()>until:raise TimeoutError(needle+'\n'+self.log.read_text())
   time.sleep(.1)
 def key(self,key):
  self.q('human-monitor-command',{'command-line':'sendkey '+key+' 25'});time.sleep(.055)
 def type(self,text):
  for ch in text:
   key={' ':'spc','.':'dot','-':'minus','_':'shift-minus','/':'slash'}.get(ch,ch.lower())
   if ch.isupper():key='shift-'+key
   self.key(key)
 def command(self,s):self.type(s);self.key('ret')
 def screen(self,name):
  time.sleep(.3);self.q('screendump',{'filename':str(OUT/(name+'.ppm'))})
 def move(self,dx,dy):
  # relative PS/2 events in small chunks to avoid 8-bit packet overflow
  while dx or dy:
   x=max(-80,min(80,dx));y=max(-80,min(80,dy));dx-=x;dy-=y
   self.q('input-send-event',{'events':[{'type':'rel','data':{'axis':'x','value':x}},{'type':'rel','data':{'axis':'y','value':y}}]});time.sleep(.04)
 def click(self):
  for down in [True,False]:
   self.q('input-send-event',{'events':[{'type':'btn','data':{'down':down,'button':'left'}}]});time.sleep(.1)
 def close(self):
  if self.p.poll() is None:
   self.q('quit');self.p.wait(timeout=5)
  self.err.close()
vm=VM()
try:
 vm.wait('[arkos] desktop ready');vm.screen('desktop');print(MODE+': desktop boot OK',flush=True)
 vm.command('uname');vm.wait('ArkOS 0.1.0 x86_64')
 vm.command('write test.txt boot-ok');vm.wait('Saved in RAM.')
 vm.command('cat test.txt');vm.wait('\nboot-ok')
 vm.command('mem');vm.wait('boot memory')
 vm.command('uptime');vm.command('date')
 vm.key('f3');vm.wait('[ui] open Notes');vm.type('notes-ok');vm.key('f1');vm.command('cat notes.txt');vm.wait('notes-ok')
 vm.key('f2');vm.wait('[ui] open Files');vm.screen('files')
 vm.key('f1');vm.command('clear');vm.command('help');vm.screen('terminal')
 # Mouse starts at 512,384. Hit Settings dock tile centered at 589,717.
 vm.move(77,333);vm.click();vm.wait('[ui] open Settings');vm.screen('settings')
 vm.move(-190,-470);vm.click();vm.screen('light-theme')
 vm.key('f1');vm.command('rm test.txt');vm.wait('File removed from RAM.')
 vm.command('cat test.txt');vm.wait('File not found')
 print(MODE+': keyboard, shell CRUD, notes, app switch and mouse OK',flush=True)
 if os.environ.get('RUN_EDGE')=='1':
  vm.command('rm notes.txt')
  for n in range(22):vm.command('touch f'+str(n))
  vm.key('f3');vm.type('x');vm.wait('[ui] Notes: RAM directory full');vm.screen('notes-full')
  vm.key('f1');vm.command('rm f0');vm.key('f3');vm.type('savedagain')
  vm.key('f1');vm.command('cat notes.txt');vm.wait('savedagain');vm.command('rm f1')
  vm.key('f2')
  for _ in range(20):vm.key('down')
  vm.screen('files-scrolled');vm.key('f1')
  print(MODE+': full RAM directory error/recovery and file scrolling OK',flush=True)
 # Reboot through kernel and verify RAM reset in a fresh boot.
 vm.command('write persist.txt must-reset');vm.wait('must-reset');vm.command('reboot');until=time.time()+30
 while vm.log.read_text().count('[arkos] desktop ready')<2:
  if time.time()>until:raise TimeoutError('reboot did not reach desktop')
  time.sleep(.1)
 vm.command('cat persist.txt');time.sleep(.4)
 assert vm.log.read_text().split('[arkos] desktop ready')[-1].count('File not found')>0
 vm.screen('rebooted');print(MODE+': reboot and RAM reset OK',flush=True)
 vm.command('shutdown');vm.p.wait(timeout=10);assert vm.p.returncode==0
 print(MODE+': poweroff OK',flush=True)
finally:vm.close()
