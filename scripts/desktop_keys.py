"""Guarded physical-keyboard events for owned desktop test fixtures.

Wayland virtual keyboards bypass compositor IME grabs. Writing only EV_KEY
events through a writable evdev keyboard lets native text-input tests use the
physical input path. Never reads keyboard event contents.
"""
import json, os, pathlib, struct, subprocess, time
special={'Escape':1,'Return':28,'space':57,'BackSpace':14,'Delete':111,
 'Home':102,'End':107,'Left':105,'Right':106,'Up':103,'Down':108,
 'Tab':15,'Shift_L':42,'Page_Up':104,'Page_Down':109,'ctrl':29,'shift':42}
letters=dict(zip('qwertyuiop',range(16,26)))
letters.update(zip('asdfghjkl',range(30,39)))
letters.update(zip('zxcvbnm',range(44,51)))
letters.update(zip('1234567890',range(2,12)))
letters.update({' ':57,'-':12,'=':13,'[':26,']':27,';':39,"'":40,',':51,'.':52,'/':53})
def send(pid,args,window_id=None):
 def guard():
  windows=json.loads(subprocess.check_output(['niri','msg','-j','windows']))
  focused=next((w for w in windows if w['is_focused']),None)
  if not focused or (window_id is not None and focused['id']!=window_id) or (pid is not None and focused['pid']!=pid):
   raise RuntimeError('Fixture lost focus; abort key injection')
  if pid is None and window_id is None:raise RuntimeError('No fixture identity provided')
 device=None
 for name in pathlib.Path('/sys/class/input').glob('event*/device/name'):
  if name.read_text().strip()=='AT Translated Set 2 keyboard':
   candidate=pathlib.Path('/dev/input')/name.parent.parent.name
   if os.access(candidate,os.W_OK):device=candidate;break
 if device is None:raise RuntimeError('No writable test keyboard; native IME test unavailable')
 guard(); fd=os.open(device,os.O_WRONLY|os.O_NONBLOCK)
 pressed=set()
 def emit(code,value):
  os.write(fd,struct.pack('llHHI',0,0,1,code,value))
  os.write(fd,struct.pack('llHHI',0,0,0,0,0))
  if value:pressed.add(code)
  else:pressed.discard(code)
 def tap(code):
  guard();emit(code,1);time.sleep(.025);emit(code,0);time.sleep(.025)
 try:
  delay=.08;index=0
  while index<len(args):
   value=args[index];index+=1
   if value=='-d':delay=int(args[index])/1000;index+=1;continue
   if value=='-k':tap(special[args[index]]);index+=1;continue
   if value in ('-M','-m'):
    guard();emit(special[args[index]],int(value=='-M'));index+=1;continue
   for char in value:
    if char.isupper():
     guard();emit(42,1)
     try:tap(letters[char.lower()])
     finally:emit(42,0)
    else:tap(letters[char])
    time.sleep(delay)
 finally:
  for code in list(pressed):emit(code,0)
  os.close(fd)
