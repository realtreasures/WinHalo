"""Real pointer transitions for the line/capsule/panel hierarchy.

Uses an isolated, services-disabled app and a nonactivating window below it.
No input or commands are sent to a user's music player or other WinHalo.
"""
import ctypes as c
from ctypes import wintypes as w
import json
import os
from pathlib import Path
import subprocess
import sys
import time
import window_smoke as desktop


def main():
    u, g, k = desktop.u, desktop.g, desktop.k
    if '--allow-existing' not in sys.argv:
        assert not u.FindWindowW('WinHalo.NativeIslandWindow', None), 'Close existing WinHalo or use --allow-existing'
    executable = Path(sys.argv[1] if len(sys.argv) > 1 else 'x64/Release/WinHalo.exe').resolve()
    desktop.bind(u, 'SetWindowPos', w.BOOL, w.HWND, w.HWND,
                 c.c_int, c.c_int, c.c_int, c.c_int, w.UINT)
    cursor = w.POINT(); assert u.GetCursorPos(c.byref(cursor))
    foreground = u.GetForegroundWindow()
    data = Path('artifacts/line-smoke-data').resolve(); data.mkdir(parents=True, exist_ok=True)
    (data / 'settings.json').write_text(json.dumps({'version': 2, 'autoHideFullscreen': False}), encoding='utf-8')
    instance = k.GetModuleHandleW(None)
    brush = g.CreateSolidBrush(0x00D8E8F0)
    cls = desktop.WNDCLASS(0, desktop.probe_proc, 0, 0, instance, None, None, brush, None, 'WinHaloLineProbe')
    assert u.RegisterClassW(c.byref(cls))
    process = app = probe = handle = None
    samples = {}
    try:
        assert u.SetCursorPos(50, 300)
        probe = u.CreateWindowExW(0x08000088, cls.name, 'WinHalo line probe', 0x90000000,
            0, 0, 1000, 800, None, None, instance, None)
        assert probe
        env = dict(os.environ, WINHALO_DATA_DIR=str(data), WINHALO_DISABLE_SERVICES='1')
        process = subprocess.Popen([str(executable)], env=env, creationflags=0x08000000)
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            app = desktop.owned_window(process.pid)
            if app and u.IsWindowVisible(app): break
            assert process.poll() is None, 'Application exited on startup'
            desktop.pump()
        assert app and u.IsWindowVisible(app)
        rect = w.RECT(); assert u.GetWindowRect(app, c.byref(rect))
        assert u.SetWindowPos(probe, None, rect.left, rect.top,
            rect.right - rect.left, rect.bottom - rect.top, 0x14)
        assert u.SetWindowPos(app, w.HWND(-1), 0, 0, 0, 0, 0x13)
        handle = k.OpenProcess(0x1000, False, process.pid); assert handle

        def point(x, y): return desktop.point_dip(app, x, y)
        def move(x, y):
            value = point(x, y); assert u.SetCursorPos(value.x, value.y); desktop.pump(0.15)
        def hold(seconds, x, y):
            value = point(x, y)
            deadline = time.monotonic() + seconds
            while time.monotonic() < deadline:
                desktop.pump(0.05)
                actual = w.POINT(); assert u.GetCursorPos(c.byref(actual))
                assert abs(actual.x - value.x) <= 3 and abs(actual.y - value.y) <= 3, (
                    'Pointer moved during the idle timing check; rerun while the mouse is idle: actual=' +
                    str((actual.x, actual.y)) + ', expected=' + str((value.x, value.y)))
        def click(x, y):
            value = point(x, y); desktop.click_at(value.x, value.y)
        def line():
            width, height = desktop.region_size(app)
            return abs(width - 88) <= 2 and abs(height - 8) <= 2
        def compact():
            width, height = desktop.region_size(app)
            return 175 <= width <= 195 and 40 <= height <= 65
        def panel(): return desktop.region_size(app)[1] > 300
        def wait(predicate, message, seconds=3):
            until = time.monotonic() + seconds
            while time.monotonic() < until:
                assert process.poll() is None, 'Application exited during pointer interaction'
                if predicate(): return
                desktop.pump(0.05)
            raise AssertionError(message + ': ' + str(desktop.region_size(app)))
        def cpu(name):
            before = desktop.process_cpu(handle); desktop.pump(1.2)
            samples[name] = desktop.process_cpu(handle) - before
            assert samples[name] < 120, 'Static state consumed CPU: ' + name

        move(5, 100); desktop.pump(0.5)
        wait(line, 'Startup did not present the small line')
        assert u.WindowFromPoint(point(240, 20)) == app, 'Visible line cannot receive input'
        assert u.WindowFromPoint(point(240, 38)) == probe, 'Hidden capsule area blocks another process'
        desktop.capture_host(rect, 'artifacts/line-idle.png')
        detail_top_left = point(170, 0); detail_bottom_right = point(310, 40)
        desktop.capture_host(w.RECT(detail_top_left.x, detail_top_left.y,
            detail_bottom_right.x, detail_bottom_right.y), 'artifacts/line-idle-detail.png')
        cpu('line')
        desktop.reveal_capsule(app)
        assert compact(), 'Line touch did not reveal only the compact capsule'
        desktop.capture_host(rect, 'artifacts/line-revealed.png')
        hold(5.3, 240, 20)
        observed = w.POINT(); assert u.GetCursorPos(c.byref(observed))
        assert compact(), ('Capsule collapsed while the pointer was inside: cursor=' +
            str((observed.x, observed.y)) + ', expected=' + str((point(240, 20).x, point(240, 20).y)) +
            ', hit=' + str(u.WindowFromPoint(observed)) + ', app=' + str(app) +
            ', region=' + str(desktop.region_size(app)))
        cpu('held_compact')

        move(5, 100); hold(3.7, 5, 100)
        assert compact(), 'Capsule collapsed before its grace period'
        move(240, 38); hold(1.5, 240, 38)
        assert compact(), 'Old leave deadline survived pointer re-entry'
        move(5, 100); hold(3.7, 5, 100)
        assert compact(), 'Re-entry failed to grant a complete new grace period'
        wait(line, 'Five seconds without pointer contact did not restore the line', 2.5)
        # The spring can cross the target bounds before its final settling frame.
        desktop.pump(0.8)
        # An independently running WinHalo can keep its own topmost capsule
        # beneath this one. Put our probe immediately below the owned island
        # before sending a real click through the newly empty footprint.
        assert u.SetWindowPos(probe, w.HWND(-1), 0, 0, 0, 0, 0x13)
        assert u.SetWindowPos(app, w.HWND(-1), 0, 0, 0, 0, 0x13)
        hit = u.WindowFromPoint(point(240, 38))
        assert hit == probe, ('Empty compact footprint does not expose the owned probe: hit=' +
            str(hit) + ', probe=' + str(probe) + ', app=' + str(app) +
            ', region=' + str(desktop.region_size(app)))
        desktop.clicks.clear(); click(240, 38)
        assert desktop.clicks, ('Empty compact footprint failed cross-process click-through: hit=' +
            str(u.WindowFromPoint(point(240, 38))) + ', probe=' + str(probe) + ', app=' + str(app) +
            ', region=' + str(desktop.region_size(app)))
        assert line(), ('Clicking outside the line revealed its hidden capsule: clicks=' +
            str(desktop.clicks) + ', hit=' + str(u.WindowFromPoint(point(240, 38))) +
            ', probe=' + str(probe) + ', app=' + str(app) + ', region=' + str(desktop.region_size(app)))

        desktop.reveal_capsule(app); click(240, 38); wait(panel, 'Capsule click did not open the full panel')
        move(5, 100); desktop.pump(5.5)
        assert panel(), 'Full panel incorrectly used the compact idle timeout'
        click(415, 51); wait(compact, 'Panel close skipped the compact stage')

        # Context menu time does not consume the grace period.
        desktop.reveal_capsule(app); move(5, 100)
        at = point(240, 38)
        assert u.PostMessageW(app, 0x7B, app, (at.y << 16) | (at.x & 0xFFFF))
        deadline = time.monotonic() + 3
        menu = None
        while time.monotonic() < deadline:
            menu = desktop.owned_window(process.pid, '#32768')
            if menu: break
            desktop.pump(0.05)
        assert menu, 'Owned context menu did not open'
        desktop.pump(5.3)
        assert compact(), 'Context menu did not pause automatic collapse'
        assert u.PostMessageW(menu, 0x100, 0x1B, 0)  # Escape only the owned menu.
        deadline = time.monotonic() + 3
        while desktop.owned_window(process.pid, '#32768') and time.monotonic() < deadline:
            desktop.pump(0.05)
        assert not desktop.owned_window(process.pid, '#32768'), 'Owned menu failed to close'
        move(5, 100); desktop.pump(3.7)
        assert compact(), 'Closing the context menu lost its grace period'
        wait(line, 'Capsule did not idle after menu dismissal', 2.5)

        # In particular, re-entry during the shrinking spring must not form a
        # WM_MOUSELEAVE/re-registration loop or lose the next leave deadline.
        desktop.reveal_capsule(app); move(5, 100)
        hold(3.7, 5, 100); assert compact(), 'Post-menu reveal lost its grace period'
        wait(line, 'Post-menu capsule lost pointer leave tracking', 2.5)
        desktop.pump(0.8)
        desktop.reveal_capsule(app); move(5, 100)
        desktop.send(app, 0x312, 1)
        assert not u.IsWindowVisible(app)
        desktop.pump(5.3)
        desktop.send(app, 0x312, 1)
        assert u.IsWindowVisible(app)
        wait(compact, 'Hidden time consumed the capsule grace period')
        desktop.pump(3.7); assert compact(), 'Restored capsule collapsed too soon'
        wait(line, 'Restored capsule never returned to line', 2.5)
        # Explicit hotkey must still open the full panel from the line.
        desktop.send(app, 0x312, 3); wait(panel, 'Panel hotkey stopped working in line state')
        desktop.send(app, 0x312, 3); wait(compact, 'Panel hotkey skipped the compact state')
        desktop.pump(5.7); wait(line, 'Hotkey-closed capsule did not return to line')
        after = w.RECT(); assert u.GetWindowRect(app, c.byref(after))
        assert tuple(getattr(after, name) for name in ('left', 'top', 'right', 'bottom')) == tuple(
            getattr(rect, name) for name in ('left', 'top', 'right', 'bottom')), 'State transitions resized the native host'
        assert u.GetForegroundWindow() == foreground, 'Line/capsule interaction stole keyboard focus'
        desktop.send(app, 0x10); assert process.wait(timeout=5) == 0
        print(json.dumps({'result': 'PASS', 'dpi': u.GetDpiForWindow(probe), 'cpu_ms_per_1_2_seconds': samples,
            'checks': ['startup line', 'real hover reveals compact only', 'held pointer prevents collapse',
                'five-second leave deadline and re-entry reset', 'cross-process click-through',
                'panel and menu timeout exclusion', 'hide/show timer restoration', 'hotkey from line',
                'fixed host size', 'static CPU', 'no focus theft']}))
    finally:
        if process and process.poll() is None:
            if app: u.PostMessageW(app, 0x10, 0, 0)
            try: process.wait(timeout=5)
            except subprocess.TimeoutExpired: process.terminate(); process.wait(timeout=5)
        if handle: k.CloseHandle(handle)
        if probe: u.DestroyWindow(probe)
        u.UnregisterClassW(cls.name, instance); g.DeleteObject(brush)
        u.SetCursorPos(cursor.x, cursor.y)


if __name__ == '__main__': main()
