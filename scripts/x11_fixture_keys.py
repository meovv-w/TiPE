"""XTest keys for a focused, explicitly owned X11 fixture only."""
import ctypes as c
import time


def send(display_name, window_id, arguments):
    x=c.CDLL('libX11.so.6');test=c.CDLL('libXtst.so.6')
    x.XOpenDisplay.argtypes=[c.c_char_p];x.XOpenDisplay.restype=c.c_void_p
    x.XCloseDisplay.argtypes=[c.c_void_p]
    x.XGetInputFocus.argtypes=[c.c_void_p,c.POINTER(c.c_ulong),c.POINTER(c.c_int)]
    x.XQueryTree.argtypes=[c.c_void_p,c.c_ulong,c.POINTER(c.c_ulong),c.POINTER(c.c_ulong),
                          c.POINTER(c.POINTER(c.c_ulong)),c.POINTER(c.c_uint)]
    x.XFree.argtypes=[c.c_void_p]
    x.XStringToKeysym.argtypes=[c.c_char_p];x.XStringToKeysym.restype=c.c_ulong
    x.XKeysymToKeycode.argtypes=[c.c_void_p,c.c_ulong];x.XKeysymToKeycode.restype=c.c_ubyte
    x.XSync.argtypes=[c.c_void_p,c.c_int]
    test.XTestFakeKeyEvent.argtypes=[c.c_void_p,c.c_uint,c.c_int,c.c_ulong]
    display=x.XOpenDisplay(display_name.encode())
    assert display, 'Cannot connect to private Xwayland'
    try:
        def guard():
            focus=c.c_ulong();revert=c.c_int()
            x.XGetInputFocus(display,c.byref(focus),c.byref(revert))
            current=focus.value
            for _ in range(32):
                if current==window_id:return
                root=c.c_ulong();parent=c.c_ulong();children=c.POINTER(c.c_ulong)();count=c.c_uint()
                assert current>1 and x.XQueryTree(display,current,c.byref(root),c.byref(parent),
                                                 c.byref(children),c.byref(count)), 'No focused owned X11 window'
                if children:x.XFree(children)
                if parent.value==current:break
                current=parent.value
            raise RuntimeError('Owned X11 fixture lost input focus')
        names=[];iterator=iter(arguments)
        for value in iterator:
            if value=='-k':names.append(next(iterator))
            else:
                assert not value.startswith('-'), 'Unsupported fixture key option'
                names.extend(value)
        for name in names:
            guard()
            symbol=x.XStringToKeysym(name.encode())
            code=x.XKeysymToKeycode(display,symbol)
            assert symbol and code, ('No X11 key mapping',name)
            test.XTestFakeKeyEvent(display,code,1,0);x.XSync(display,False)
            time.sleep(.02)
            test.XTestFakeKeyEvent(display,code,0,0);x.XSync(display,False)
            time.sleep(.02)
    finally:x.XCloseDisplay(display)
