"""Isolated native-settings integration check; run on an unlocked desktop.

Only the launched PID receives messages. Services, startup changes and real
volume/brightness controls stay disabled. Clipboard checks run only when every
existing clipboard format is plain text; those formats are restored afterwards.
The script never clicks a link's Open button or invokes a browser.
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


u, k = desktop.u, desktop.k
desktop.bind(u, 'GetDlgItem', w.HWND, w.HWND, c.c_int)
desktop.bind(u, 'GetParent', w.HWND, w.HWND)
desktop.bind(u, 'IsWindow', w.BOOL, w.HWND)
desktop.bind(u, 'SetForegroundWindow', w.BOOL, w.HWND)
desktop.bind(u, 'SetWindowTextW', w.BOOL, w.HWND, w.LPCWSTR)
desktop.bind(u, 'GetWindowTextW', c.c_int, w.HWND, w.LPWSTR, c.c_int)
desktop.bind(u, 'SetWindowPos', w.BOOL, w.HWND, w.HWND,
             c.c_int, c.c_int, c.c_int, c.c_int, w.UINT)
desktop.bind(u, 'OpenClipboard', w.BOOL, w.HWND)
desktop.bind(u, 'CloseClipboard', w.BOOL)
desktop.bind(u, 'EmptyClipboard', w.BOOL)
desktop.bind(u, 'EnumClipboardFormats', w.UINT, w.UINT)
desktop.bind(u, 'GetClipboardData', w.HANDLE, w.UINT)
desktop.bind(u, 'SetClipboardData', w.HANDLE, w.UINT, w.HANDLE)
desktop.bind(u, 'GetClipboardSequenceNumber', w.DWORD)
desktop.bind(k, 'GlobalAlloc', w.HANDLE, w.UINT, c.c_size_t)
desktop.bind(k, 'GlobalLock', w.LPVOID, w.HANDLE)
desktop.bind(k, 'GlobalUnlock', w.BOOL, w.HANDLE)
desktop.bind(k, 'GlobalSize', c.c_size_t, w.HANDLE)
desktop.bind(k, 'GlobalFree', w.HANDLE, w.HANDLE)

SETTINGS_CLASS = 'WinHalo.SettingsWindow'
CONTENT_CLASS = 'WinHalo.SettingsContent'
FIELDS = {'language': 1, 'width': 5, 'fontSize': 11, 'lineWidth': 12, 'autoHideIdle': 13,
          'rightDrag': 15, 'showNotifications': 16, 'clipboardLinks': 18, 'privacyIndicators': 19}


def control_id(field, part=0):
    return 1000 + FIELDS[field] * 4 + part


def wait(predicate, message, process, seconds=5):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        assert process.poll() is None, 'Owned application exited: ' + message
        if predicate():
            return
        desktop.pump(0.05)
    raise AssertionError(message)


def plain_clipboard_snapshot(owner):
    """Return None instead of discarding rich text, images or custom formats."""
    if not u.OpenClipboard(owner):
        return None
    try:
        snapshots = []
        current = 0
        while True:
            current = u.EnumClipboardFormats(current)
            if not current:
                return snapshots
            if current not in (1, 7, 13, 16):
                return None
            handle = u.GetClipboardData(current)
            size = k.GlobalSize(handle) if handle else 0
            if not size or size > 1024 * 1024:
                return None
            pointer = k.GlobalLock(handle)
            if not pointer:
                return None
            try:
                snapshots.append((current, c.string_at(pointer, size)))
            finally:
                k.GlobalUnlock(handle)
    finally:
        u.CloseClipboard()


def replace_clipboard(owner, snapshots):
    assert u.OpenClipboard(owner), 'Clipboard is busy'
    try:
        assert u.EmptyClipboard()
        for kind, data in snapshots:
            handle = k.GlobalAlloc(0x42, len(data))
            assert handle
            pointer = k.GlobalLock(handle)
            if not pointer:
                k.GlobalFree(handle)
                raise AssertionError('Could not lock test clipboard memory')
            c.memmove(pointer, data, len(data))
            k.GlobalUnlock(handle)
            if not u.SetClipboardData(kind, handle):
                k.GlobalFree(handle)
                raise AssertionError('Could not publish test clipboard')
        return u.GetClipboardSequenceNumber()
    finally:
        u.CloseClipboard()


def main():
    executable = Path(sys.argv[1] if len(sys.argv) > 1 else 'x64/Release/WinHalo.exe').resolve()
    if '--allow-existing' not in sys.argv:
        assert not u.FindWindowW('WinHalo.NativeIslandWindow', None), 'Close WinHalo or pass --allow-existing'
    artifacts = Path('artifacts').resolve()
    artifacts.mkdir(exist_ok=True)
    data = artifacts / ('settings-smoke-data-' + uuid.uuid4().hex[:8])
    data.mkdir()
    settings_file = data / 'settings.json'
    settings_file.write_text(json.dumps({'version': 3, 'language': 'zh-CN',
        'reducedMotion': True, 'autoHideFullscreen': False}), encoding='utf-8')
    cursor = w.POINT(); assert u.GetCursorPos(c.byref(cursor))
    previous_foreground = u.GetForegroundWindow()
    process = app = settings = content = clipboard_owner = None
    saved_clipboard = None
    clipboard_sequence = None
    clipboard_result = 'skipped: non-text or busy clipboard'
    checks = []

    def owned(hwnd):
        assert hwnd
        pid = w.DWORD(); u.GetWindowThreadProcessId(hwnd, c.byref(pid))
        assert pid.value == process.pid, 'Refusing to send input to another process'
        return hwnd

    def state():
        return json.loads(settings_file.read_text(encoding='utf-8'))

    def setting_is(key, value):
        try:
            return state().get(key) == value
        except (OSError, ValueError):
            return False

    def child(field, part=0):
        return owned(u.GetDlgItem(content, control_id(field, part)))

    def set_toggle(field, enabled):
        handle = child(field)
        desktop.send(handle, 0xF1, int(enabled))  # BM_SETCHECK
        desktop.send(content, 0x111, control_id(field), handle)  # BN_CLICKED
        wait(lambda: setting_is(field, enabled), 'Toggle did not save: ' + field, process)

    def set_number(field, value):
        handle = child(field)
        value_buffer = c.create_unicode_buffer(str(value))
        assert desktop.send(handle, 0x0C, 0, c.addressof(value_buffer))
        typed = c.create_unicode_buffer(64); desktop.send(handle, 0x0D, len(typed), c.addressof(typed))
        assert typed.value == str(value), 'Numeric control reset before commit: ' + repr(typed.value)
        desktop.send(content, 0x111, control_id(field) | (0x200 << 16), handle)  # EN_KILLFOCUS
        try:
            wait(lambda: setting_is(field, value), 'Number did not save: ' + field, process)
        except AssertionError:
            rectangle = w.RECT(); u.GetWindowRect(settings, c.byref(rectangle))
            desktop.capture_host(rectangle, str(artifacts / 'settings-submit-failure.png'))
            raise

    def open_settings():
        nonlocal settings, content
        point = desktop.point_dip(app, 240, 20)
        desktop.send(app, 0x7B, app, (point.y << 16) | (point.x & 0xFFFF))
        wait(lambda: desktop.owned_window(process.pid, SETTINGS_CLASS), 'Settings did not open', process)
        settings = owned(desktop.owned_window(process.pid, SETTINGS_CLASS))
        content = owned(u.FindWindowExW(settings, None, CONTENT_CLASS, None))

    def close_settings():
        desktop.send(settings, 0x10)
        wait(lambda: not desktop.owned_window(process.pid, SETTINGS_CLASS), 'Settings did not close', process)

    def screenshot(section, filename):
        navigation = owned(u.GetDlgItem(settings, 100 + section))
        desktop.send(settings, 0x111, 100 + section, navigation)
        visible = bool(u.IsWindowVisible(app))
        if visible:
            desktop.send(app, 0x312, 1)
        desktop.pump(0.3)
        rectangle = w.RECT(); assert u.GetWindowRect(settings, c.byref(rectangle))
        desktop.capture_host(rectangle, str(artifacts / filename))
        if visible:
            desktop.send(app, 0x312, 1)

    try:
        env = dict(os.environ, WINHALO_DATA_DIR=str(data), WINHALO_DISABLE_SERVICES='1')
        process = subprocess.Popen([str(executable)], env=env, creationflags=0x08000000)
        wait(lambda: desktop.owned_window(process.pid), 'Owned island did not start', process, 10)
        app = owned(desktop.owned_window(process.pid))
        wait(lambda: u.IsWindowVisible(app), 'Owned island remained hidden', process)
        assert u.SetWindowPos(app, w.HWND(-1), 0, 0, 0, 0, 0x13)
        desktop.reveal_capsule(app)
        desktop.click_at(*tuple(getattr(desktop.point_dip(app, 240, 38), key) for key in ('x', 'y')))
        assert desktop.region_size(app)[1] < 90, 'No-media click reopened a module panel'
        checks.append('no-media capsule click is inactive')

        # A real short right click goes straight to settings, without a popup menu.
        point = desktop.point_dip(app, 240, 38)
        assert u.SetCursorPos(point.x, point.y)
        u.mouse_event(8, 0, 0, 0, 0); desktop.pump(0.08)
        u.mouse_event(16, 0, 0, 0, 0)
        wait(lambda: desktop.owned_window(process.pid, SETTINGS_CLASS), 'Short right click did not open settings', process)
        settings = owned(desktop.owned_window(process.pid, SETTINGS_CLASS))
        content = owned(u.FindWindowExW(settings, None, CONTENT_CLASS, None))
        assert not desktop.owned_window(process.pid, '#32768'), 'Short right click still opened an embedded menu'
        original_settings = settings
        open_settings(); assert settings == original_settings, 'Settings window was duplicated'
        assert not (u.GetWindowLongW(settings, -20) & (0x08000000 | 8)), 'Settings inherited NOACTIVATE or TOPMOST'
        checks.append('short right click and singleton normal settings window')

        screenshot(1, 'settings-appearance.png')
        set_number('width', 196)
        desktop.send(content, 0x111, control_id('width', 2), child('width', 2))
        wait(lambda: setting_is('width', 197), 'Increase button did not save', process)
        desktop.send(content, 0x111, control_id('width', 1), child('width', 1))
        wait(lambda: setting_is('width', 196), 'Decrease button did not save', process)
        set_number('lineWidth', 128)
        set_number('fontSize', 18)
        set_number('fontSize', 0)
        language = child('language')
        desktop.send(language, 0x14E, 2)
        desktop.send(content, 0x111, control_id('language') | (1 << 16), language)
        wait(lambda: setting_is('language', 'en-US'), 'Language did not save', process)
        caption = c.create_unicode_buffer(128); u.GetWindowTextW(settings, caption, len(caption))
        assert 'Settings' in caption.value, 'Language change did not update the settings title'
        desktop.send(language, 0x14E, 1)
        desktop.send(content, 0x111, control_id('language') | (1 << 16), language)
        wait(lambda: setting_is('language', 'zh-CN'), 'Chinese language did not restore', process)
        set_toggle('privacyIndicators', True)
        set_toggle('privacyIndicators', False)

        set_toggle('autoHideIdle', False)
        screenshot(1, 'settings-appearance.png')
        screenshot(2, 'settings-behavior.png')
        close_settings()
        desktop.pump(5.4)
        assert desktop.region_size(app)[1] > 30, 'Disabled idle hiding still collapsed the capsule'
        checks.append('number input/steppers, autosave, appearance/behavior screenshots, idle disabled')

        # Actual press-and-drag must save one position and never open settings.
        assert u.SetWindowPos(app, w.HWND(-1), 0, 0, 0, 0, 0x13)
        point = desktop.point_dip(app, 240, 38)
        assert u.SetCursorPos(point.x, point.y); desktop.pump(0.1)
        before = w.RECT(); assert u.GetWindowRect(app, c.byref(before))
        u.mouse_event(8, 0, 0, 0, 0); desktop.pump(0.5)
        assert u.SetCursorPos(point.x + 60, point.y + 36); desktop.pump(0.2)
        u.mouse_event(16, 0, 0, 0, 0); desktop.pump(0.6)
        after = w.RECT(); assert u.GetWindowRect(app, c.byref(after))
        assert (before.left, before.top) != (after.left, after.top), 'Long right drag did not move the island'
        assert not desktop.owned_window(process.pid, SETTINGS_CLASS), 'Drag release also opened settings'
        saved_position = state()
        assert saved_position['offsetX'] or saved_position['offsetY'], 'Drag position was not saved'
        checks.append('long right drag saves position without opening settings')

        open_settings()
        set_toggle('autoHideIdle', True)
        close_settings()
        assert u.SetCursorPos(30, 300)
        wait(lambda: desktop.region_size(app)[1] < 12, 'Idle hiding did not return', process, 7)
        assert abs(desktop.region_size(app)[0] - 128) <= 2, 'Saved line width was not applied'
        checks.append('idle enabled and configured line width')

        # Use a test-owned native window for temporary clipboard ownership.
        clipboard_owner = u.CreateWindowExW(0x08000080, 'STATIC', 'WinHalo settings clipboard fixture',
            0, 0, 0, 0, 0, None, None, k.GetModuleHandleW(None), None)
        assert clipboard_owner
        saved_clipboard = plain_clipboard_snapshot(clipboard_owner)
        if saved_clipboard is not None:
            open_settings(); set_toggle('clipboardLinks', True); close_settings()
            assert u.SetCursorPos(30, 300)
            foreground = u.GetForegroundWindow()
            url = 'https://example.invalid/winhalo-smoke-' + uuid.uuid4().hex
            clipboard_sequence = replace_clipboard(clipboard_owner, [(13, (url + '\0').encode('utf-16-le'))])
            wait(lambda: desktop.region_size(app)[1] >= 80, 'Copied link did not produce a confirmation', process)
            desktop.pump(0.7)
            assert desktop.region_size(app)[1] < 110, 'Clipboard confirmation opened a full panel'
            assert u.GetForegroundWindow() == foreground, 'Clipboard change opened or activated another application'
            # Never select Open. Closing the prompt is the only link action tested.
            rectangle = w.RECT(); assert u.GetWindowRect(app, c.byref(rectangle))
            box = desktop.region_box(app); scale = u.GetDpiForWindow(app) / 96
            desktop.click_at(rectangle.left + box.right - round(43 * scale),
                rectangle.top + box.top + round(65 * scale))
            open_settings(); set_toggle('clipboardLinks', False); close_settings()
            assert u.SetCursorPos(30, 300)
            wait(lambda: desktop.region_size(app)[1] < 12, 'Dismissed link did not restore line', process, 7)
            clipboard_sequence = replace_clipboard(clipboard_owner, [(13, (url + '-disabled\0').encode('utf-16-le'))])
            desktop.pump(0.8)
            assert desktop.region_size(app)[1] < 12, 'Clipboard listener remained active after disabling'
            clipboard_result = 'PASS: confirmation only, disabled listener, original plain-text formats retained'
            checks.append('clipboard confirmation without auto-open and listener disabled')

        # Persisted preferences must survive a fresh process.
        desktop.send(app, 0x10); assert process.wait(timeout=8) == 0
        process = subprocess.Popen([str(executable)], env=env, creationflags=0x08000000)
        wait(lambda: desktop.owned_window(process.pid), 'Restart did not create an island', process, 10)
        app = owned(desktop.owned_window(process.pid))
        wait(lambda: u.IsWindowVisible(app), 'Restart remained hidden', process)
        assert abs(desktop.region_size(app)[0] - 128) <= 2
        open_settings()
        text = c.create_unicode_buffer(64); assert desktop.send(child('width'), 0x0D, len(text), c.addressof(text))
        assert float(text.value) == 196, 'Numeric preference was lost on restart'
        assert abs(state()['offsetX'] - saved_position['offsetX']) < 0.01
        assert abs(state()['offsetY'] - saved_position['offsetY']) < 0.01
        close_settings(); assert process.poll() is None, 'Closing settings exited WinHalo'
        checks.append('restart persistence and closing settings keeps island running')
        desktop.send(app, 0x10); assert process.wait(timeout=8) == 0
        print(json.dumps({'result': 'PASS', 'checks': checks, 'clipboard': clipboard_result,
            'data_directory': str(data), 'screenshots': ['artifacts/settings-appearance.png',
            'artifacts/settings-behavior.png']}, ensure_ascii=False))
    finally:
        u.mouse_event(16, 0, 0, 0, 0)
        if process and process.poll() is None:
            if app: u.PostMessageW(app, 0x10, 0, 0)
            try: process.wait(timeout=8)
            except subprocess.TimeoutExpired: process.terminate(); process.wait(timeout=5)
        if saved_clipboard is not None and clipboard_sequence == u.GetClipboardSequenceNumber():
            replace_clipboard(clipboard_owner, saved_clipboard)
        if clipboard_owner: u.DestroyWindow(clipboard_owner)
        u.SetCursorPos(cursor.x, cursor.y)
        if previous_foreground and u.IsWindow(previous_foreground): u.SetForegroundWindow(previous_foreground)


if __name__ == '__main__':
    main()
