"""Owned-PID chat-preview integration; no real notifications or pointer input.

Uses isolated configuration and WINHALO_DISABLE_SERVICES. All message targets
belong to the process launched here. Lock/hide tests send messages to that PID
only; they do not lock Windows or change another application's window.
"""
import ctypes as c
from ctypes import wintypes as w
import json
import os
from pathlib import Path
import subprocess
import sys
import time
import uuid
import window_smoke as desktop

u = desktop.u
desktop.bind(u, 'GetDlgItem', w.HWND, w.HWND, c.c_int)
desktop.bind(u, 'IsWindow', w.BOOL, w.HWND)
desktop.bind(u, 'IsWindowEnabled', w.BOOL, w.HWND)
desktop.bind(u, 'SetForegroundWindow', w.BOOL, w.HWND)


def main():
    executable = Path(sys.argv[1]).resolve()
    if '--allow-existing' not in sys.argv:
        assert not u.FindWindowW('WinHalo.NativeIslandWindow', None), 'Close WinHalo or pass --allow-existing'
    data = Path('artifacts').resolve() / ('chat-smoke-data-' + uuid.uuid4().hex[:8])
    data.mkdir(parents=True)
    settings_file = data / 'settings.json'
    settings_file.write_text(json.dumps({'version': 4, 'language': 'zh-CN',
        'reducedMotion': True, 'autoHideFullscreen': False}), encoding='utf-8')
    process = app = settings = content = None
    foreground = u.GetForegroundWindow()
    checks = []

    def wait(predicate, message, seconds=5):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            assert process.poll() is None, 'Owned process exited: ' + message
            if predicate(): return
            desktop.pump(.035)
        raise AssertionError(message)

    def owned(hwnd):
        assert hwnd
        pid = w.DWORD(); u.GetWindowThreadProcessId(hwnd, c.byref(pid))
        assert pid.value == process.pid, 'Refusing to send a message to another process'
        return hwnd

    def child(identifier):
        return owned(u.GetDlgItem(content, identifier))

    def command(identifier):
        desktop.send(content, 0x111, identifier, child(identifier))

    def toggle(identifier, value):
        desktop.send(child(identifier), 0xF1, int(value))  # BM_SETCHECK
        command(identifier)
        desktop.pump(.05)

    def popup():
        hwnd = desktop.owned_window(process.pid, 'WinHalo.ChatNotification')
        return owned(hwnd) if hwnd and u.IsWindowVisible(hwnd) else None

    def preview():
        command(1116)
        wait(popup, 'Sample chat preview did not appear')
        return popup()

    def close_card(hwnd):
        desktop.send(owned(hwnd), 0x10)

    try:
        env = dict(os.environ, WINHALO_DATA_DIR=str(data), WINHALO_DISABLE_SERVICES='1')
        process = subprocess.Popen([str(executable)], env=env, creationflags=0x08000000)
        wait(lambda: desktop.owned_window(process.pid), 'Island did not start', 12)
        app = owned(desktop.owned_window(process.pid))
        desktop.send(app, 0x7B, app, -1)
        wait(lambda: desktop.owned_window(process.pid, 'WinHalo.SettingsWindow'), 'Settings did not open')
        settings = owned(desktop.owned_window(process.pid, 'WinHalo.SettingsWindow'))
        content = owned(u.FindWindowExW(settings, None, 'WinHalo.SettingsContent', None))
        assert not u.IsWindowEnabled(child(1096)) and not u.IsWindowEnabled(child(1116))
        command(1116); desktop.pump(.08)
        assert not popup(), 'Disabled preview command unexpectedly opened a card'
        checks.append('chat opt-in defaults and disabled controls')

        toggle(1092, True)
        wait(lambda: u.IsWindowEnabled(child(1116)), 'Chat controls did not enable')
        before_focus = u.GetForegroundWindow()
        first = preview()
        assert u.GetForegroundWindow() == before_focus, 'Preview stole foreground focus'
        assert u.GetWindowLongW(first, -20) & 0x08000000, 'Preview lost NOACTIVATE'
        checks.append('settings callback to real preview window without activation')

        # A second sample waits behind the first, then becomes visible when closed.
        command(1116); desktop.pump(.05)
        close_card(first); desktop.pump(.1)
        wait(popup, 'Closing the first card did not advance the pending queue')
        close_card(popup()); desktop.pump(.1)
        assert not popup()
        checks.append('pending sample advances after dismissal')

        preview()
        desktop.send(app, 0x2B1, 7, 0)  # WM_WTSSESSION_CHANGE / WTS_SESSION_LOCK, own window only.
        wait(lambda: not popup(), 'Session lock message did not suppress the card')
        desktop.send(app, 0x2B1, 8, 0)
        desktop.pump(.1); assert not popup(), 'Unlock replayed a suppressed card'
        checks.append('owned session lock/unlock messages suppress without replay')

        preview()
        desktop.send(app, 0x312, 1)  # Own WM_HOTKEY route, no global keyboard input.
        wait(lambda: not u.IsWindowVisible(app) and not popup(), 'Manual hide did not suppress the card')
        desktop.send(app, 0x312, 1)
        wait(lambda: u.IsWindowVisible(app), 'Own island did not restore')
        assert not popup(), 'Manual restore replayed a suppressed card'
        checks.append('manual hide and restore discard pending card')

        preview(); toggle(1104, False)
        wait(lambda: not popup(), 'Hiding message content left stale card pixels')
        preview(); toggle(1092, False)
        wait(lambda: not popup(), 'Disabling chat left the preview visible')
        assert not u.IsWindowEnabled(child(1096))
        preferences = json.loads(settings_file.read_text(encoding='utf-8'))
        assert preferences['version'] == 4 and not preferences['chatNotifications'] and not preferences['chatShowBody']
        assert 'WinHalo.Test' not in settings_file.read_text(encoding='utf-8')
        checks.append('presentation changes clear cards and save preferences only')

        desktop.send(settings, 0x10)
        wait(lambda: not u.IsWindow(settings), 'Closing settings did not destroy only settings')
        assert process.poll() is None
        desktop.send(app, 0x10)
        assert process.wait(timeout=8) == 0, 'Application did not exit normally'
        log = (data / 'WinHalo.log').read_text(encoding='utf-8', errors='replace')
        assert 'WinHalo.Test' not in log and '这是 WinHalo 弹窗预览' not in log
        checks.append('clean shutdown and no sample message content in logs')
        print(json.dumps({'result': 'PASS', 'checks': checks,
            'scope': 'sample notifications and owned messages only; no QQ/WeChat access or authorization',
            'data_directory': str(data)}, ensure_ascii=False))
    finally:
        if process and process.poll() is None:
            if app: u.PostMessageW(app, 0x10, 0, 0)
            try: process.wait(timeout=8)
            except subprocess.TimeoutExpired: process.terminate(); process.wait(timeout=5)
        if foreground and u.IsWindow(foreground): u.SetForegroundWindow(foreground)


if __name__ == '__main__':
    main()
