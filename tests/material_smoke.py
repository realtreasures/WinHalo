"""Native page navigation, shadow pass-through and bounded optional underlay.

Uses isolated settings and a window owned by this script. No media/audio commands.
"""
import ctypes as c
from ctypes import wintypes as w
import json
import os
from pathlib import Path
import subprocess
import sys
import window_smoke as desktop
import system_smoke as system


def main():
    u, g, k = desktop.u, desktop.g, desktop.k
    if '--allow-existing' not in sys.argv:
        assert not u.FindWindowW('WinHalo.NativeIslandWindow', None), 'Close existing WinHalo'
    foreground = u.GetForegroundWindow()
    cursor = w.POINT(); u.GetCursorPos(c.byref(cursor))
    data = Path('artifacts/material-smoke-data').resolve(); data.mkdir(parents=True, exist_ok=True)
    (data / 'settings.json').write_text(json.dumps({'version': 2, 'reducedMotion': True}), encoding='utf-8')
    instance = k.GetModuleHandleW(None)
    brush = g.CreateSolidBrush(0x00D8E8F0)
    cls = desktop.WNDCLASS(0, desktop.probe_proc, 0, 0, instance, None, None, brush, None, 'WinHaloMaterialProbe')
    assert u.RegisterClassW(c.byref(cls))
    app = probe = handle = process = None
    native = False
    live_background = False
    alternate_brush = None
    try:
        probe = u.CreateWindowExW(0x08000088, cls.name, 'WinHalo material probe', 0x90000000,
            0, 0, 1000, 800, None, None, instance, None)
        environment = dict(os.environ, WINHALO_DATA_DIR=str(data), WINHALO_DISABLE_SERVICES='1')
        process = subprocess.Popen([str(Path(sys.argv[1]).resolve())], env=environment, creationflags=0x08000000)
        for _ in range(100):
            app = desktop.owned_window(process.pid)
            if app and u.IsWindowVisible(app): break
            assert process.poll() is None
            desktop.pump()
        assert app and u.IsWindowVisible(app)
        handle = k.OpenProcess(0x1000, False, process.pid)
        rect = w.RECT(); assert u.GetWindowRect(app, c.byref(rect))
        scale = u.GetDpiForWindow(app) / 96
        assert u.SetWindowPos(probe, None, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, 0x14)
        def point(x, y): return w.POINT(rect.left + round(x * scale), rect.top + round(y * scale))
        def click(x, y): system.click_dip(app, x, y)
        shadow = desktop.owned_window(process.pid, 'STATIC', 'WinHalo material shadow')
        glass = desktop.owned_window(process.pid, 'WinHalo.MaterialGlassWindow')
        assert shadow and glass and not u.IsWindowVisible(glass)
        assert u.GetWindowLongW(shadow, -20) & 0x80020 == 0x80020, 'Shadow is not a transparent layered window'
        # The visible shadow must not consume input outside the capsule.
        assert u.WindowFromPoint(point(240, 67)) == probe, 'Shadow stole pointer hit outside capsule'
        desktop.click_at(point(240, 67).x, point(240, 67).y)
        assert desktop.clicks, 'Shadow swallowed a cross-process click'
        desktop.send(app, 0x312, 3)
        desktop.pump(0.3)
        desktop.capture_host(rect, 'artifacts/material-home.png')
        # Music, volume and battery cards open real detail pages. No controls clicked.
        for x, y, name in [(130, 145, 'media'), (340, 145, 'volume'), (130, 240, 'battery')]:
            click(x, y)
            desktop.capture_host(rect, 'artifacts/material-' + name + '.png')
            if name == 'media':
                desktop.send(app, 0x312, 3)  # Focused player has no close arrow or home/settings footer.
                desktop.send(app, 0x312, 3); desktop.pump(0.3)
            else: click(90, 328)
        click(340, 240)  # Settings card.
        click(90, 328)   # System page.
        def pixel(x, y):
            dc = u.GetDC(None)
            try: return g.GetPixel(dc, point(x, y).x, point(x, y).y)
            finally: u.ReleaseDC(None, dc)
        corner_before = pixel(32, 18)
        click(240, 225)  # Optional desktop blur.
        desktop.pump(0.3)
        native = json.loads((data / 'settings.json').read_text(encoding='utf-8')).get('nativeGlass', False)
        if native:
            assert u.IsWindowVisible(glass)
            area = w.RECT(); assert u.GetWindowRect(glass, c.byref(area))
            assert abs((area.right - area.left) - round(420 * scale)) <= 2
            assert abs((area.bottom - area.top) - round(340 * scale)) <= 2
            assert u.WindowFromPoint(point(240, 120)) == app, 'Glass covered the interactive capsule'
            assert u.WindowFromPoint(point(5, 10)) == probe, 'Glass blocked empty host'
            assert pixel(32, 18) == corner_before, 'Native material escaped the rounded capsule into its corners'
            desktop.capture_host(rect, 'artifacts/material-glass.png')
            desktop.bind(u, 'SetClassLongPtrW', c.c_size_t, w.HWND, c.c_int, c.c_size_t)
            desktop.bind(u, 'InvalidateRect', w.BOOL, w.HWND, c.c_void_p, w.BOOL)
            desktop.bind(u, 'UpdateWindow', w.BOOL, w.HWND)
            before = pixel(240, 284)
            alternate_brush = g.CreateSolidBrush(0x00C85030)
            u.SetClassLongPtrW(probe, -10, alternate_brush)
            u.InvalidateRect(probe, None, True); u.UpdateWindow(probe)
            desktop.pump(0.3)
            live_background = pixel(240, 284) != before
            u.SetClassLongPtrW(probe, -10, brush)
            u.InvalidateRect(probe, None, True); u.UpdateWindow(probe)
            desktop.pump(0.3)
        baseline = u.GetGuiResources(handle, 0)
        for _ in range(20):
            click(415, 51)
            desktop.send(app, 0x312, 3)
        assert u.GetGuiResources(handle, 0) <= baseline + 2, 'Material windows leaked GDI objects'
        click(415, 51)
        desktop.send(app, 0x312, 1)
        assert not u.IsWindowVisible(shadow) and not u.IsWindowVisible(glass), 'Backdrop survived manual hide'
        desktop.send(app, 0x312, 1)
        assert u.IsWindowVisible(shadow)
        assert u.GetForegroundWindow() == foreground, 'Page or material interaction stole foreground focus'
        desktop.send(app, 0x10)
        assert process.wait(timeout=5) == 0
        assert not desktop.owned_window(process.pid, 'STATIC', 'WinHalo material shadow') and not desktop.owned_window(process.pid, 'WinHalo.MaterialGlassWindow'), 'Backdrop windows survived exit'
        print(json.dumps({'result': 'PASS', 'native_backdrop_api_available': native,
            'live_background_change_observed': live_background,
            'checks': ['module cards and return to home', 'shadow cross-process input transparency',
                'bounded native underlay', '20 material layout cycles', 'hide/show and teardown', 'no focus theft']}))
    finally:
        if process and process.poll() is None:
            if app: u.PostMessageW(app, 0x10, 0, 0)
            try: process.wait(timeout=5)
            except subprocess.TimeoutExpired: process.terminate(); process.wait(timeout=5)
        if handle: k.CloseHandle(handle)
        if probe: u.DestroyWindow(probe)
        u.UnregisterClassW(cls.name, instance); g.DeleteObject(brush)
        if alternate_brush: g.DeleteObject(alternate_brush)
        u.SetCursorPos(cursor.x, cursor.y)
        if foreground: u.SetForegroundWindow(foreground)


if __name__ == '__main__': main()
