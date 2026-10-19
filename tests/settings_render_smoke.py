"""Native settings scroll/repaint regression; run on an unlocked desktop.

Uses an isolated data directory and services-disabled, PID-owned windows only.
Does not edit controls, startup registration, audio, brightness or the clipboard.
Default coverage is the desktop's actual DPI; --expected-dpi verifies a fixture
or real desktop scale without claiming to simulate another monitor's DPI.
"""
import argparse
import ctypes as c
from ctypes import wintypes as w
import json
import os
from pathlib import Path
import struct
import subprocess
import time
import uuid
import zlib
import window_smoke as desktop

u, g, k = desktop.u, desktop.g, desktop.k
dwm = c.WinDLL('dwmapi', use_last_error=True)
desktop.bind(dwm, 'DwmFlush', w.LONG)
desktop.bind(u, 'GetDlgItem', w.HWND, w.HWND, c.c_int)
desktop.bind(u, 'GetClientRect', w.BOOL, w.HWND, c.POINTER(w.RECT))
desktop.bind(u, 'ClientToScreen', w.BOOL, w.HWND, c.POINTER(w.POINT))
desktop.bind(u, 'GetClassNameW', c.c_int, w.HWND, w.LPWSTR, c.c_int)
desktop.bind(u, 'IsChild', w.BOOL, w.HWND, w.HWND)
desktop.bind(u, 'IsWindow', w.BOOL, w.HWND)
desktop.bind(u, 'SetForegroundWindow', w.BOOL, w.HWND)
desktop.bind(u, 'RedrawWindow', w.BOOL, w.HWND, w.LPVOID, w.HRGN, w.UINT)


class ScrollInfo(c.Structure):
    _fields_ = [('size', w.UINT), ('mask', w.UINT), ('minimum', c.c_int),
                ('maximum', c.c_int), ('page', w.UINT), ('position', c.c_int), ('track', c.c_int)]


class ScrollBarInfo(c.Structure):
    _fields_ = [('size', w.DWORD), ('rect', w.RECT), ('line', c.c_int),
                ('thumb_top', c.c_int), ('thumb_bottom', c.c_int),
                ('reserved', c.c_int), ('states', w.DWORD * 6)]


class GuiInfo(c.Structure):
    _fields_ = [('size', w.DWORD), ('flags', w.DWORD), ('active', w.HWND),
                ('focus', w.HWND), ('capture', w.HWND), ('menu_owner', w.HWND),
                ('move_size', w.HWND), ('caret', w.HWND), ('caret_rect', w.RECT)]


desktop.bind(u, 'GetScrollInfo', w.BOOL, w.HWND, c.c_int, c.POINTER(ScrollInfo))
desktop.bind(u, 'GetScrollBarInfo', w.BOOL, w.HWND, w.LONG, c.POINTER(ScrollBarInfo))
desktop.bind(u, 'GetGUIThreadInfo', w.BOOL, w.DWORD, c.POINTER(GuiInfo))


def wait(predicate, message, process, seconds=8):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        assert process.poll() is None, 'Owned application exited: ' + message
        if predicate(): return
        desktop.pump(.05)
    raise AssertionError(message)


def client_screen(hwnd):
    rect = w.RECT(); assert u.GetClientRect(hwnd, c.byref(rect))
    point = w.POINT(); assert u.ClientToScreen(hwnd, c.byref(point))
    return w.RECT(point.x, point.y, point.x + rect.right, point.y + rect.bottom)


def pixels(rect):
    width, height = rect.right - rect.left, rect.bottom - rect.top
    screen = u.GetDC(None); assert screen
    memory = g.CreateCompatibleDC(screen); assert memory
    address = c.c_void_p()
    info = desktop.BITMAPINFO(40, width, -height, 1, 32, 0, 0, 0, 0, 0, 0)
    bitmap = g.CreateDIBSection(screen, c.byref(info), 0, c.byref(address), None, 0); assert bitmap
    previous = g.SelectObject(memory, bitmap)
    try:
        assert g.BitBlt(memory, 0, 0, width, height, screen, rect.left, rect.top, 0x40CC0020)
        return c.string_at(address, width * height * 4)
    finally:
        g.SelectObject(memory, previous); g.DeleteObject(bitmap)
        g.DeleteDC(memory); u.ReleaseDC(None, screen)


def save_pixels(rect, bgra, path):
    width, height = rect.right - rect.left, rect.bottom - rect.top
    rows = bytearray()
    for y in range(height):
        rows.append(0)
        for x in range(width):
            at = (y * width + x) * 4
            rows.extend((bgra[at + 2], bgra[at + 1], bgra[at]))
    def chunk(kind, data):
        return struct.pack('!I', len(data)) + kind + data + struct.pack('!I', zlib.crc32(kind + data) & 0xFFFFFFFF)
    Path(path).write_bytes(b'\x89PNG\r\n\x1a\n' +
        chunk(b'IHDR', struct.pack('!IIBBBBB', width, height, 8, 2, 0, 0, 0)) +
        chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b''))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('executable', nargs='?', default='x64/Release/WinHalo.exe')
    parser.add_argument('--allow-existing', action='store_true')
    parser.add_argument('--expected-dpi', type=int)
    args = parser.parse_args()
    if not args.allow_existing:
        assert not u.FindWindowW('WinHalo.NativeIslandWindow', None), 'Close WinHalo or pass --allow-existing'
    artifacts = Path('artifacts').resolve(); artifacts.mkdir(exist_ok=True)
    data = artifacts / ('settings-render-data-' + uuid.uuid4().hex[:8]); data.mkdir()
    (data / 'settings.json').write_text(json.dumps({'version': 3, 'language': 'zh-CN',
        'reducedMotion': True, 'autoHideFullscreen': False, 'systemControls': False,
        'clipboardLinks': False, 'privacyIndicators': False}), encoding='utf-8')
    cursor = w.POINT(); assert u.GetCursorPos(c.byref(cursor))
    foreground = u.GetForegroundWindow()
    process = app = settings = content = handle = None
    comparisons = []
    checks = []

    def owned(hwnd):
        assert hwnd
        pid = w.DWORD(); u.GetWindowThreadProcessId(hwnd, c.byref(pid))
        assert pid.value == process.pid, 'Refusing to address another process'
        return hwnd

    def gui():
        state = GuiInfo(); state.size = c.sizeof(state)
        assert u.GetGUIThreadInfo(thread, c.byref(state))
        return state

    def scroll_position():
        info = ScrollInfo(); info.size = c.sizeof(info); info.mask = 0x17
        assert u.GetScrollInfo(content, 1, c.byref(info))
        return info.position

    def native_text(hwnd):
        text = c.create_unicode_buffer(512)
        desktop.send(hwnd, 0xD, len(text), c.cast(text, c.c_void_p).value)  # WM_GETTEXT, marshalled by USER32
        return text.value

    def control_state():
        parent = client_screen(content)
        position = scroll_position()
        result = {}
        for identifier, hwnd in controls:
            rect = w.RECT(); assert u.GetWindowRect(hwnd, c.byref(rect))
            name = c.create_unicode_buffer(64); assert u.GetClassNameW(hwnd, name, len(name))
            text = native_text(hwnd) if name.value.casefold() in ('edit', 'combobox') else None
            result[identifier] = (rect.left - parent.left,
                rect.top - parent.top + round(position * scale),
                rect.right - rect.left, rect.bottom - rect.top, text)
        return result

    def validate_controls():
        current = control_state()
        assert current.keys() == baseline_controls.keys(), 'Native controls were replaced during scrolling'
        for identifier, value in current.items():
            original = baseline_controls[identifier]
            assert value[4] == original[4], 'Native edit/combo text changed: ' + str(identifier)
            assert all(abs(value[index] - original[index]) <= 2 for index in range(4)), (
                'Control did not follow the shared scroll offset: ' + str(identifier) +
                ', actual=' + str(value) + ', baseline=' + str(original))

    def settle():
        desktop.pump(.18)
        desktop.send(content, 0)
        desktop.pump(.05)
        assert dwm.DwmFlush() >= 0 and dwm.DwmFlush() >= 0, 'Desktop composition did not flush'

    def navigation(index, check_focus=True):
        button = owned(u.GetDlgItem(settings, 100 + index))
        rect = w.RECT(); assert u.GetWindowRect(button, c.byref(rect))
        point = w.POINT((rect.left + rect.right) // 2, (rect.top + rect.bottom) // 2)
        assert u.WindowFromPoint(point) == button, 'Owned navigation button is covered by another window'
        desktop.click_at(point.x, point.y)
        settle()
        if check_focus:
            assert desktop.send(button, 0x129) & 1, 'Mouse navigation left visible keyboard focus cues'
        validate_controls()

    def wheel(delta):
        rect = client_screen(content)
        assert u.SetCursorPos(rect.left + round(20 * scale), (rect.top + rect.bottom) // 2)
        assert u.WindowFromPoint(w.POINT(rect.left + round(20 * scale), (rect.top + rect.bottom) // 2)) == content
        u.mouse_event(0x0800, 0, 0, delta & 0xFFFFFFFF, 0)

    def caret_mask():
        state = gui()
        if not state.caret: return None
        owned(state.caret)
        a = w.POINT(state.caret_rect.left, state.caret_rect.top)
        b = w.POINT(state.caret_rect.right, state.caret_rect.bottom)
        assert u.ClientToScreen(state.caret, c.byref(a)) and u.ClientToScreen(state.caret, c.byref(b))
        return (a.x - 3, a.y - 3, b.x + 3, b.y + 3)

    def compare_redraw(label):
        settle(); validate_controls()
        area = client_screen(content)
        # Right-hand controls only, excluding the native nonclient scroll bar.
        area.left = max(area.left, area.right - round(220 * scale))
        before_caret = caret_mask()
        before = pixels(area)
        assert u.RedrawWindow(settings, None, None, 0x185)  # INVALIDATE | ERASE | ALLCHILDREN | UPDATENOW
        desktop.pump(.08)
        assert dwm.DwmFlush() >= 0 and dwm.DwmFlush() >= 0
        after_caret = caret_mask()
        after = pixels(area)
        width, height = area.right - area.left, area.bottom - area.top
        changed = 0
        masks = [value for value in (before_caret, after_caret) if value]
        for y in range(height):
            for x in range(width):
                sx, sy = area.left + x, area.top + y
                if any(left <= sx < right and top <= sy < bottom for left, top, right, bottom in masks):
                    continue
                at = (y * width + x) * 4
                if max(abs(before[at + channel] - after[at + channel]) for channel in range(3)) > 10:
                    changed += 1
        save_pixels(area, before, artifacts / ('settings-render-' + label + '-before.png'))
        save_pixels(area, after, artifacts / ('settings-render-' + label + '-after.png'))
        comparisons.append({'state': label, 'changed_pixels': changed, 'area_pixels': width * height})
        assert changed <= 8, 'Full redraw repaired stale control pixels: ' + str(comparisons[-1])
        validate_controls()

    def thumb_drag(fraction, screenshot=False):
        info = ScrollBarInfo(); info.size = c.sizeof(info)
        assert u.GetScrollBarInfo(content, -5, c.byref(info))  # OBJID_VSCROLL
        before = scroll_position()
        x = (info.rect.left + info.rect.right) // 2
        y = info.rect.top + (info.thumb_top + info.thumb_bottom) // 2
        available = info.rect.bottom - info.rect.top - 2 * info.line
        target = info.rect.top + info.line + round(available * fraction)
        assert u.SetCursorPos(x, y)
        assert u.WindowFromPoint(w.POINT(x, y)) == content, 'Owned scroll bar is covered'
        u.mouse_event(2, 0, 0, 0, 0); desktop.pump(.06)
        for step in range(1, 7):
            assert u.SetCursorPos(x, y + round((target - y) * step / 6))
            desktop.pump(.025)
            if screenshot and step == 4:
                rect = client_screen(content)
                desktop.capture_host(rect, str(artifacts / 'settings-render-thumb-moving.png'))
        u.mouse_event(4, 0, 0, 0, 0)
        settle()
        assert scroll_position() != before, 'Dragging the real scroll-bar thumb had no effect'
        validate_controls()

    try:
        env = dict(os.environ, WINHALO_DATA_DIR=str(data), WINHALO_DISABLE_SERVICES='1')
        process = subprocess.Popen([str(Path(args.executable).resolve())], env=env, creationflags=0x08000000)
        wait(lambda: desktop.owned_window(process.pid), 'Owned island did not start', process)
        app = owned(desktop.owned_window(process.pid))
        desktop.send(app, 0x7B, app, -1)  # Open settings via its owned context route.
        wait(lambda: desktop.owned_window(process.pid, 'WinHalo.SettingsWindow'), 'Settings did not open', process)
        settings = owned(desktop.owned_window(process.pid, 'WinHalo.SettingsWindow'))
        content = owned(u.FindWindowExW(settings, None, 'WinHalo.SettingsContent', None))
        thread = u.GetWindowThreadProcessId(settings, None)
        dpi = u.GetDpiForWindow(settings)
        if args.expected_dpi: assert dpi == args.expected_dpi, 'Requested DPI fixture is not active'
        scale = dpi / 96
        # The floating island must not cover any pixels in a settings screenshot.
        if u.IsWindowVisible(app): desktop.send(app, 0x312, 1)
        controls = [(identifier, owned(hwnd)) for identifier in range(1000, 1000 + 23 * 4)
                    if (hwnd := u.GetDlgItem(content, identifier))]
        assert len(controls) >= 23, 'Expected native setting controls were not created'
        settle()
        baseline_controls = control_state()
        for identifier, value in baseline_controls.items():
            if value[4] is not None: assert value[4], 'A native edit/combo has no readable value'
        for index in (2, 1, 0): navigation(index)
        compare_redraw('initial')
        handle = k.OpenProcess(0x1400, False, process.pid); assert handle
        baseline_gdi = u.GetGuiResources(handle, 0)
        assert baseline_gdi > 0, 'Could not read owned-process GDI resources'

        for step in range(24):
            wheel(-360 if step < 12 else 360)
            desktop.pump(.015)
            if step == 7:
                desktop.capture_host(client_screen(content), str(artifacts / 'settings-render-wheel-moving.png'))
        compare_redraw('wheel')
        thumb_drag(.75, True); compare_redraw('thumb-down')
        thumb_drag(.20); compare_redraw('thumb-up')
        for index in (2, 0, 1, 2, 1, 0): navigation(index)
        compare_redraw('navigation')
        desktop.capture_host(client_screen(content), str(artifacts / 'settings-render-stable.png'))
        checks.extend(['real fast wheel up/down', 'real scroll-bar thumb drag', 'repeated mouse navigation',
                       'unchanged native text and normalized control positions', 'no stale pixels repaired by full redraw',
                       'mouse navigation hides keyboard focus cues'])

        first_focus = gui().focus
        assert u.PostMessageW(settings, 0x100, 9, 0) and u.PostMessageW(settings, 0x101, 9, 0)
        settle()
        next_focus = owned(gui().focus)
        assert next_focus != first_focus and u.IsChild(settings, next_focus), 'Keyboard Tab no longer advances focus'
        dialog_code = desktop.send(next_focus, 0x87)  # WM_GETDLGCODE, no synthetic text input.
        assert dialog_code, 'Focused native control no longer exposes dialog-key behavior'
        checks.append('owned keyboard Tab focus and native dialog-key behavior')
        final_gdi = u.GetGuiResources(handle, 0)
        assert final_gdi <= baseline_gdi + 12, 'Scroll/repaint leaked GDI resources'
        before_cpu = desktop.process_cpu(handle); desktop.pump(1.2)
        idle_cpu = desktop.process_cpu(handle) - before_cpu
        assert idle_cpu < 120, 'Idle settings window is continuously repainting'
        checks.extend(['bounded GDI resources', 'bounded idle CPU'])
        print(json.dumps({'result': 'PASS', 'dpi': dpi, 'dpi_coverage': 'current actual DPI only',
            'checks': checks, 'pixel_comparisons': comparisons,
            'gdi_before': baseline_gdi, 'gdi_after': final_gdi, 'idle_cpu_ms_per_1_2_seconds': idle_cpu,
            'data_directory': str(data)}, ensure_ascii=False))
    finally:
        u.mouse_event(4, 0, 0, 0, 0)
        if process and process.poll() is None:
            if app: u.PostMessageW(app, 0x10, 0, 0)
            try: process.wait(timeout=8)
            except subprocess.TimeoutExpired: process.terminate(); process.wait(timeout=5)
        if handle: k.CloseHandle(handle)
        u.SetCursorPos(cursor.x, cursor.y)
        if foreground and u.IsWindow(foreground): u.SetForegroundWindow(foreground)


if __name__ == '__main__':
    main()
