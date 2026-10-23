#!/usr/bin/env python3
"""Read-only X11 geometry diagnostic without xwininfo dependency."""
import ctypes as c, json
x=c.CDLL('libX11.so.6');u=c.c_ulong;i=c.c_int;p=c.c_void_p
x.XOpenDisplay.argtypes=[c.c_char_p];x.XOpenDisplay.restype=p
x.XDefaultRootWindow.argtypes=[p];x.XDefaultRootWindow.restype=u
x.XQueryTree.argtypes=[p,u,c.POINTER(u),c.POINTER(u),c.POINTER(c.POINTER(u)),c.POINTER(c.c_uint)]
x.XFetchName.argtypes=[p,u,c.POINTER(c.c_char_p)]
x.XGetGeometry.argtypes=[p,u,c.POINTER(u),c.POINTER(i),c.POINTER(i),c.POINTER(c.c_uint),c.POINTER(c.c_uint),c.POINTER(c.c_uint),c.POINTER(c.c_uint)]
x.XFree.argtypes=[p];x.XCloseDisplay.argtypes=[p]
class Attributes(c.Structure):
 _fields_=[('x',i),('y',i),('width',i),('height',i),('border',i),('depth',i),('visual',p),('root',u),('klass',i),('bit_gravity',i),('win_gravity',i),('backing_store',i),('backing_planes',u),('backing_pixel',u),('save_under',i),('colormap',u),('map_installed',i),('map_state',i),('all_event_masks',c.c_long),('your_event_mask',c.c_long),('do_not_propagate_mask',c.c_long),('override_redirect',i),('screen',p)]
x.XGetWindowAttributes.argtypes=[p,u,c.POINTER(Attributes)]
d=x.XOpenDisplay(None);assert d
rows=[]
def walk(window,depth):
 root=u();parent=u();children=c.POINTER(u)();count=c.c_uint()
 name=c.c_char_p();x.XFetchName(d,window,c.byref(name));title=name.value.decode(errors='replace') if name.value else ''
 if name:x.XFree(name)
 gx=i();gy=i();w=c.c_uint();h=c.c_uint();border=c.c_uint();bits=c.c_uint()
 x.XGetGeometry(d,window,c.byref(root),c.byref(gx),c.byref(gy),c.byref(w),c.byref(h),c.byref(border),c.byref(bits))
 attributes=Attributes();x.XGetWindowAttributes(d,window,c.byref(attributes))
 rows.append(dict(id=hex(window),depth=depth,title=title,x=gx.value,y=gy.value,width=w.value,height=h.value,map_state=attributes.map_state,override_redirect=attributes.override_redirect))
 if depth<4 and x.XQueryTree(d,window,c.byref(root),c.byref(parent),c.byref(children),c.byref(count)):
  for index in range(count.value):walk(children[index],depth+1)
  if children:x.XFree(children)
walk(x.XDefaultRootWindow(d),0);x.XCloseDisplay(d)
print(json.dumps(rows,ensure_ascii=False,indent=2))
