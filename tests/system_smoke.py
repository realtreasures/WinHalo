"""Real foreground-window events and isolated HKCU startup settings.

Creates a temporary fullscreen fixture, restores the desktop afterwards, and
never modifies the real Windows Run key or sends media/audio commands.
"""
import ctypes as c
from ctypes import wintypes as w
import json
import os
from pathlib import Path
import subprocess
import sys
import time
import winreg
import window_smoke as desktop

u = desktop.u


class MONITORINFO(c.Structure):
    _fields_ = [('size', w.DWORD), ('monitor', w.RECT), ('work', w.RECT), ('flags', w.DWORD)]


desktop.bind(u, 'MonitorFromWindow', w.HANDLE, w.HWND, w.DWORD)
desktop.bind(u, 'GetMonitorInfoW', w.BOOL, w.HANDLE, c.POINTER(MONITORINFO))
desktop.bind(u, 'SetForegroundWindow', w.BOOL, w.HWND)
desktop.bind(u, 'ShowWindow', w.BOOL, w.HWND, c.c_int)
desktop.bind(u, 'SetWindowPos', w.BOOL, w.HWND, w.HWND, c.c_int, c.c_int, c.c_int, c.c_int, w.UINT)


def wait_visible(app, visible, message):
    until = time.monotonic() + 4
    while time.monotonic() < until:
        if bool(u.IsWindowVisible(app)) == visible:
            return
        desktop.pump(0.05)
    raise AssertionError(message)


def click_dip(app, x, y):
    scale = u.GetDpiForWindow(app) / 96
    desktop.send(app, 0x202, lp=(round(y * scale) << 16) | round(x * scale))
    desktop.pump(0.15)


def main():
    executable = Path(sys.argv[1] if len(sys.argv) > 1 else 'x64/Release/WinHalo.exe').resolve()
    if '--allow-existing' not in sys.argv:
        assert not u.FindWindowW('WinHalo.NativeIslandWindow', None), 'Close existing WinHalo before testing'
    foreground = u.GetForegroundWindow()
    original_cursor = w.POINT()
    assert u.GetCursorPos(c.byref(original_cursor))
    data = Path('artifacts/system-smoke-data').resolve()
    data.mkdir(parents=True, exist_ok=True)
    settings = data / 'settings.json'
    if settings.exists():
        settings.unlink()
    test_key = 'Software\\WinHalo\\Tests\\Desktop-' + str(os.getpid())
    with winreg.CreateKey(winreg.HKEY_CURRENT_USER, test_key) as key:
        winreg.SetValueEx(key, 'Sentinel', 0, winreg.REG_SZ, 'preserve')
    environment = dict(os.environ, WINHALO_DATA_DIR=str(data), WINHALO_STARTUP_TEST_KEY=test_key)
    environment.pop('WINHALO_DISABLE_SERVICES', None)
    instance = desktop.k.GetModuleHandleW(None)
    brush = desktop.g.CreateSolidBrush(0x00352B25)
    cls = desktop.WNDCLASS(0, desktop.probe_proc, 0, 0, instance, None, None, brush, None, 'WinHaloFullScreenFixture')
    assert u.RegisterClassW(c.byref(cls))
    process = app = fixture = None
    try:
        process = subprocess.Popen([str(executable)], creationflags=0x08000000, env=environment)
        until = time.monotonic() + 10
        while time.monotonic() < until:
            app = desktop.owned_window(process.pid)
            if app and u.IsWindowVisible(app):
                break
            assert process.poll() is None, 'Application exited on startup'
            desktop.pump()
        assert app
        wait_visible(app, True, 'Initial island was unexpectedly hidden')
        desktop.pump(1)
        # Open animation settings, switch to system page, and exercise the real UI actions.
        rect = w.RECT()
        assert u.GetWindowRect(app, c.byref(rect))
        scale = u.GetDpiForWindow(app) / 96
        cx, cy = rect.left + round(240 * scale), rect.top + round(38 * scale)
        desktop.select_context_item(app, cx, cy, 8)
        desktop.pump(1)
        click_dip(app, 90, 328)
        desktop.capture_host(rect, 'artifacts/system-settings.png')
        click_dip(app, 240, 125)
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, test_key) as key:
            command, kind = winreg.QueryValueEx(key, 'WinHalo')
            assert kind == winreg.REG_SZ and command == '"' + str(executable) + '"', 'Startup command is not correctly quoted'
            assert winreg.QueryValueEx(key, 'Sentinel')[0] == 'preserve'
        click_dip(app, 240, 125)
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, test_key) as key:
            try:
                winreg.QueryValueEx(key, 'WinHalo')
                raise AssertionError('Startup toggle did not remove its own value')
            except FileNotFoundError:
                pass
            assert winreg.QueryValueEx(key, 'Sentinel')[0] == 'preserve'
        # Disable, persist and enable fullscreen detection through the same settings row.
        click_dip(app, 240, 175)
        assert json.loads(settings.read_text(encoding='utf-8'))['autoHideFullscreen'] is False
        click_dip(app, 240, 175)
        assert json.loads(settings.read_text(encoding='utf-8'))['autoHideFullscreen'] is True
        click_dip(app, 415, 51)
        desktop.pump(1)
        info = MONITORINFO()
        info.size = c.sizeof(info)
        assert u.GetMonitorInfoW(u.MonitorFromWindow(app, 2), c.byref(info))
        area = info.monitor
        width, height = area.right - area.left, area.bottom - area.top
        fixture = u.CreateWindowExW(0, cls.name, 'WinHalo fullscreen test', 0x80000000,
            area.left + 80, area.top + 80, width - 160, height - 160, None, None, instance, None)
        assert fixture
        u.ShowWindow(fixture, 5)
        assert u.SetForegroundWindow(fixture)
        desktop.pump(0.4)
        assert u.GetForegroundWindow() == fixture, 'Could not activate owned fullscreen fixture'
        wait_visible(app, True, 'Ordinary borderless window incorrectly hid the island')

        def full():
            assert u.SetWindowPos(fixture, None, area.left, area.top, width, height, 0x14)
            desktop.pump(0.3)

        def small():
            assert u.SetWindowPos(fixture, None, area.left + 80, area.top + 80, width - 160, height - 160, 0x14)
            desktop.pump(0.3)

        full()
        wait_visible(app, False, 'Fullscreen location event did not hide the island')
        # A newly launched app must remain hidden if the foreground is already fullscreen.
        desktop.send(app, 0x10)
        assert process.wait(timeout=5) == 0
        process = subprocess.Popen([str(executable)], creationflags=0x08000000, env=environment)
        app = None
        until = time.monotonic() + 10
        while time.monotonic() < until:
            app = desktop.owned_window(process.pid)
            if app:
                break
            assert process.poll() is None
            desktop.pump()
        assert app
        desktop.pump(1)
        wait_visible(app, False, 'Starting inside fullscreen exposed the island')
        small()
        wait_visible(app, True, 'Leaving fullscreen did not restore the island')
        desktop.send(app, 0x312, 1)
        wait_visible(app, False, 'Manual hide failed')
        full()
        small()
        wait_visible(app, False, 'Fullscreen exit overrode manual hide')
        desktop.send(app, 0x312, 1)
        wait_visible(app, True, 'Explicit show failed')
        full()
        wait_visible(app, False, 'Second fullscreen transition did not hide')
        desktop.send(app, 0x8015, lp=(1 << 16) | 0x400)
        wait_visible(app, True, 'Tray cannot explicitly restore an automatically hidden island')
        desktop.select_tray_item(app, cx, cy, 2)
        desktop.pump(0.5)
        wait_visible(app, True, 'Tray Settings override was revoked by foreground menu restoration')
        click_dip(app, 415, 51)
        desktop.pump(1)
        small()
        wait_visible(app, True, 'Fullscreen exit lost the explicit show preference')
        assert u.GetForegroundWindow() == fixture, 'Automatic visibility changes took foreground focus'
        # Leave automatic hiding disabled and verify a fullscreen event has no effect.
        desktop.select_tray_item(app, cx, cy, 2)
        desktop.pump(1)
        click_dip(app, 90, 328)
        click_dip(app, 240, 175)
        assert json.loads(settings.read_text(encoding='utf-8'))['autoHideFullscreen'] is False
        click_dip(app, 415, 51)
        desktop.pump(1)
        full()
        wait_visible(app, True, 'Disabled automatic hiding still hid the island')
        desktop.send(app, 0x218, 0xA)  # Real power query via WM_POWERBROADCAST, no fake status.
        desktop.send(app, 0x10)
        assert process.wait(timeout=5) == 0
        print(json.dumps({'result': 'PASS', 'checks': ['system settings navigation',
            'quoted startup command and isolated enable/disable', 'unrelated registry value preserved',
            'fullscreen setting JSON', 'real fullscreen foreground/location events',
            'startup inside fullscreen remains hidden', 'fullscreen exit restoration', 'manual hide preservation', 'tray explicit show override',
            'no foreground theft', 'disabled fullscreen detection', 'power event query', 'clean exit'],
            'real_startup_key_modified': False}))
    finally:
        if process and process.poll() is None:
            if app:
                u.PostMessageW(app, 0x10, 0, 0)
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.terminate()
                process.wait(timeout=5)
        if fixture:
            u.DestroyWindow(fixture)
        u.UnregisterClassW(cls.name, instance)
        desktop.g.DeleteObject(brush)
        if foreground:
            u.SetForegroundWindow(foreground)
        u.SetCursorPos(original_cursor.x, original_cursor.y)
        # Remove only the exact test key owned by this process.
        winreg.DeleteKey(winreg.HKEY_CURRENT_USER, test_key)


if __name__ == '__main__':
    main()
