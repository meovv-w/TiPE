"""Pointer input limited to an explicitly owned fixture and target rectangle."""
import json, pathlib, subprocess

root = pathlib.Path(__file__).resolve().parent.parent

def prepare_pointer():
    """Build the fixture-only virtual pointer; this sends no input."""
    build = root / 'build/desktop-pointer'; build.mkdir(exist_ok=True)
    protocol = root / 'integration/protocols/wlr-virtual-pointer-unstable-v1.xml'
    source = root / 'tests/desktop_pointer.c'
    binary = build / 'pointer'
    if not binary.exists() or binary.stat().st_mtime < max(source.stat().st_mtime, protocol.stat().st_mtime):
        subprocess.run(['wayland-scanner', 'client-header', str(protocol), str(build/'tipe-virtual-pointer-client.h')], check=True)
        subprocess.run(['wayland-scanner', 'private-code', str(protocol), str(build/'protocol.c')], check=True)
        flags = subprocess.check_output(['pkg-config', '--cflags', '--libs', 'wayland-client'], text=True).split()
        subprocess.run(['cc', '-O2', '-I'+str(build), str(source), str(build/'protocol.c'), '-o', str(binary), *flags], check=True)
    return binary

def point(window_id, x, y, region, click=False):
    """Coordinates and region are logical output pixels, from fixture geometry.

    This test helper is never used to position the product's candidate popup.
    It supports the single-output desktop used by the acceptance fixture.
    """
    windows = json.loads(subprocess.check_output(['niri', 'msg', '-j', 'windows']))
    focused = next((w for w in windows if w['is_focused']), None)
    if focused is None or focused['id'] != window_id:
        raise RuntimeError('Owned fixture lost focus; abort pointer injection')
    left, top, width, height = region
    if not (left <= x < left + width and top <= y < top + height):
        raise RuntimeError('Pointer target outside allowed fixture region')
    outputs = json.loads(subprocess.check_output(['niri', 'msg', '-j', 'outputs']))
    active = [o['logical'] for o in outputs.values() if o.get('logical')]
    if len(active) != 1 or active[0]['x'] or active[0]['y']:
        raise RuntimeError('Pointer fixture currently requires one output at the origin')
    output = active[0]
    binary = prepare_pointer()
    # Recheck after compilation, before sending any pointer event.
    windows = json.loads(subprocess.check_output(['niri', 'msg', '-j', 'windows']))
    if not any(w['id'] == window_id and w['is_focused'] for w in windows):
        raise RuntimeError('Owned fixture lost focus while preparing pointer')
    subprocess.run([str(binary), str(int(x)), str(int(y)), str(output['width']),
                    str(output['height']), 'click' if click else 'move'], check=True, timeout=5)
