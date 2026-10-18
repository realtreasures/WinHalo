"""Windows desktop integration check; requires Python 3.8+, no packages.

Creates a temporary window beneath WinHalo, moves the pointer to verify
real clicks across processes, then restores it and closes both test windows.
Run while the desktop is unlocked and do not use the mouse during the check.
"""
import ctypes as c
from ctypes import wintypes as w
import json
import os
from pathlib import Path
import subprocess
import sys
import time
import struct
import zlib

u = c.WinDLL('user32', use_last_error=True)
g = c.WinDLL('gdi32', use_last_error=True)
k = c.WinDLL('kernel32', use_last_error=True)
LRESULT = c.c_ssize_t
WPARAM = c.c_size_t
LPARAM = c.c_ssize_t
WNDPROC = c.WINFUNCTYPE(LRESULT, w.HWND, w.UINT, WPARAM, LPARAM)


class WNDCLASS(c.Structure):
    _fields_ = [('style', w.UINT), ('proc', WNDPROC), ('class_extra', c.c_int),
                ('window_extra', c.c_int), ('instance', w.HINSTANCE), ('icon', w.HICON),
                ('cursor', w.HANDLE), ('brush', w.HBRUSH), ('menu', w.LPCWSTR), ('name', w.LPCWSTR)]


class BITMAPINFO(c.Structure):
    _fields_ = [('size', w.DWORD), ('width', w.LONG), ('height', w.LONG), ('planes', w.WORD),
                ('bits', w.WORD), ('compression', w.DWORD), ('image_size', w.DWORD),
                ('xppm', w.LONG), ('yppm', w.LONG), ('used', w.DWORD), ('important', w.DWORD)]


def bind(lib, name, result, *args):
    fn = getattr(lib, name)
    fn.restype, fn.argtypes = result, args
    return fn


bind(u, 'SetProcessDpiAwarenessContext', w.BOOL, w.HANDLE)(w.HANDLE(-4))
bind(u, 'DefWindowProcW', LRESULT, w.HWND, w.UINT, WPARAM, LPARAM)
bind(u, 'RegisterClassW', w.ATOM, c.POINTER(WNDCLASS))
bind(u, 'UnregisterClassW', w.BOOL, w.LPCWSTR, w.HINSTANCE)
bind(u, 'CreateWindowExW', w.HWND, w.DWORD, w.LPCWSTR, w.LPCWSTR, w.DWORD,
     c.c_int, c.c_int, c.c_int, c.c_int, w.HWND, w.HMENU, w.HINSTANCE, w.LPVOID)
bind(u, 'DestroyWindow', w.BOOL, w.HWND)
bind(u, 'FindWindowW', w.HWND, w.LPCWSTR, w.LPCWSTR)
bind(u, 'FindWindowExW', w.HWND, w.HWND, w.HWND, w.LPCWSTR, w.LPCWSTR)
bind(u, 'GetWindowThreadProcessId', w.DWORD, w.HWND, c.POINTER(w.DWORD))
bind(u, 'GetWindowRect', w.BOOL, w.HWND, c.POINTER(w.RECT))
bind(u, 'GetMenuItemRect', w.BOOL, w.HWND, w.HMENU, w.UINT, c.POINTER(w.RECT))
bind(u, 'GetWindowLongW', w.LONG, w.HWND, c.c_int)
bind(u, 'GetForegroundWindow', w.HWND)
bind(u, 'GetDpiForWindow', w.UINT, w.HWND)
bind(u, 'GetCursorPos', w.BOOL, c.POINTER(w.POINT))
bind(u, 'SetCursorPos', w.BOOL, c.c_int, c.c_int)
bind(u, 'WindowFromPoint', w.HWND, w.POINT)
bind(u, 'mouse_event', None, w.DWORD, w.DWORD, w.DWORD, w.DWORD, WPARAM)
bind(u, 'PeekMessageW', w.BOOL, c.POINTER(w.MSG), w.HWND, w.UINT, w.UINT, w.UINT)
bind(u, 'TranslateMessage', w.BOOL, c.POINTER(w.MSG))
bind(u, 'DispatchMessageW', LRESULT, c.POINTER(w.MSG))
bind(u, 'PostMessageW', w.BOOL, w.HWND, w.UINT, WPARAM, LPARAM)
bind(u, 'SendMessageTimeoutW', LRESULT, w.HWND, w.UINT, WPARAM, LPARAM,
     w.UINT, w.UINT, c.POINTER(WPARAM))
bind(u, 'IsWindowVisible', w.BOOL, w.HWND)
bind(u, 'GetDC', w.HDC, w.HWND)
bind(u, 'ReleaseDC', c.c_int, w.HWND, w.HDC)
bind(g, 'CreateSolidBrush', w.HBRUSH, w.DWORD)
bind(g, 'DeleteObject', w.BOOL, w.HANDLE)
bind(g, 'GetPixel', w.DWORD, w.HDC, c.c_int, c.c_int)
bind(g, 'CreateCompatibleDC', w.HDC, w.HDC)
bind(g, 'DeleteDC', w.BOOL, w.HDC)
bind(g, 'CreateDIBSection', w.HBITMAP, w.HDC, c.POINTER(BITMAPINFO), w.UINT, c.POINTER(c.c_void_p), w.HANDLE, w.DWORD)
bind(g, 'SelectObject', w.HANDLE, w.HDC, w.HANDLE)
bind(g, 'BitBlt', w.BOOL, w.HDC, c.c_int, c.c_int, c.c_int, c.c_int, w.HDC, c.c_int, c.c_int, w.DWORD)
bind(g, 'CreateRectRgn', w.HRGN, c.c_int, c.c_int, c.c_int, c.c_int)
bind(g, 'GetRgnBox', c.c_int, w.HRGN, c.POINTER(w.RECT))
bind(u, 'GetWindowRgn', c.c_int, w.HWND, w.HRGN)
bind(k, 'GetModuleHandleW', w.HMODULE, w.LPCWSTR)
bind(k, 'OpenProcess', w.HANDLE, w.DWORD, w.BOOL, w.DWORD)
bind(k, 'CloseHandle', w.BOOL, w.HANDLE)
bind(k, 'GetProcessTimes', w.BOOL, w.HANDLE, c.POINTER(w.FILETIME), c.POINTER(w.FILETIME),
     c.POINTER(w.FILETIME), c.POINTER(w.FILETIME))
bind(u, 'GetGuiResources', w.DWORD, w.HANDLE, w.DWORD)

clicks = []


@WNDPROC
def probe_proc(hwnd, message, wp, lp):
    if message == 0x21:  # WM_MOUSEACTIVATE
        return 3  # MA_NOACTIVATE
    if message == 0x202:
        clicks.append(lp)
        return 0
    return u.DefWindowProcW(hwnd, message, wp, lp)


def pump(seconds=0.1):
    deadline = time.monotonic() + seconds
    msg = w.MSG()
    while time.monotonic() < deadline:
        while u.PeekMessageW(c.byref(msg), None, 0, 0, 1):
            u.TranslateMessage(c.byref(msg))
            u.DispatchMessageW(c.byref(msg))
        time.sleep(0.01)


def process_cpu(handle):
    times = [w.FILETIME() for _ in range(4)]
    assert k.GetProcessTimes(handle, *(c.byref(item) for item in times))
    return sum((item.dwHighDateTime << 32) | item.dwLowDateTime for item in times[2:]) / 10000


def send(hwnd, message, wp=0, lp=0):
    result = WPARAM()
    if not u.SendMessageTimeoutW(hwnd, message, wp, lp, 2, 2000, c.byref(result)):
        raise RuntimeError('Window did not respond: ' + hex(message))
    return result.value


def click_at(x, y):
    assert u.SetCursorPos(x, y)
    pump()
    cursor = w.POINT(); assert u.GetCursorPos(c.byref(cursor))
    assert abs(cursor.x - x) <= 3 and abs(cursor.y - y) <= 3, 'Pointer moved during automated input; rerun while the mouse is idle'
    u.mouse_event(2, 0, 0, 0, 0)
    u.mouse_event(4, 0, 0, 0, 0)
    pump()


def owned_window(pid, class_name='WinHalo.NativeIslandWindow', title=None):
    """Never send input to another WinHalo, including a user VS debug session."""
    candidate = None
    while True:
        candidate = u.FindWindowExW(None, candidate, class_name, title)
        if not candidate:
            return None
        owner = w.DWORD()
        u.GetWindowThreadProcessId(candidate, c.byref(owner))
        if owner.value == pid:
            return candidate


def region_box(app):
    region = g.CreateRectRgn(0, 0, 0, 0)
    box = w.RECT()
    try:
        assert u.GetWindowRgn(app, region), 'Island has no bounded input region'
        assert g.GetRgnBox(region, c.byref(box))
        return box
    finally:
        g.DeleteObject(region)


def region_size(app):
    box = region_box(app)
    scale = u.GetDpiForWindow(app) / 96
    return ((box.right - box.left) / scale, (box.bottom - box.top) / scale)


def point_dip(app, x, y):
    rect = w.RECT(); assert u.GetWindowRect(app, c.byref(rect))
    scale = u.GetDpiForWindow(app) / 96
    return w.POINT(rect.left + round(x * scale), rect.top + round(y * scale))


def reveal_capsule(app):
    """Real hover opens line only; keep pointer inside for subsequent checks."""
    if region_size(app)[1] > 30:
        return
    point = point_dip(app, 240, 20)
    assert u.SetCursorPos(point.x, point.y)
    deadline = time.monotonic() + 3
    while time.monotonic() < deadline:
        pump(0.05)
        if region_size(app)[1] >= 40:
            pump(0.65)
            assert region_size(app)[1] < 90, 'Hover opened a full panel instead of the capsule'
            return
    final_size = region_size(app)
    cursor = w.POINT(); u.GetCursorPos(c.byref(cursor))
    class GUI(c.Structure):
        _fields_ = [('size', w.DWORD), ('flags', w.DWORD)] + [(name, w.HWND) for name in
            ('active', 'focus', 'capture', 'menu', 'move', 'caret')] + [('rect', w.RECT)]
    bind(u, 'GetGUIThreadInfo', w.BOOL, w.DWORD, c.POINTER(GUI))
    gui = GUI(); gui.size = c.sizeof(gui)
    u.GetGUIThreadInfo(u.GetWindowThreadProcessId(app, None), c.byref(gui))
    raise AssertionError('Touching the line did not reveal the capsule: region=' + str(final_size) +
        ', cursor=' + str((cursor.x, cursor.y)) + ', target=' + str((point.x, point.y)) +
        ', hit=' + str(u.WindowFromPoint(point)) + ', app=' + str(app) +
        ', gui=' + str((gui.flags, gui.capture, gui.menu, gui.active)))


def capture_host(rect, path):
    width, height = rect.right - rect.left, rect.bottom - rect.top
    screen = u.GetDC(None)
    memory = g.CreateCompatibleDC(screen)
    pixels = c.c_void_p()
    info = BITMAPINFO(40, width, -height, 1, 32, 0, 0, 0, 0, 0, 0)
    bitmap = g.CreateDIBSection(screen, c.byref(info), 0, c.byref(pixels), None, 0)
    old = g.SelectObject(memory, bitmap)
    try:
        assert g.BitBlt(memory, 0, 0, width, height, screen, rect.left, rect.top, 0x40CC0020)
        bgra = c.string_at(pixels, width * height * 4)
        rows = bytearray()
        for y in range(height):
            rows.append(0)
            for x in range(width):
                index = (y * width + x) * 4
                rows.extend((bgra[index + 2], bgra[index + 1], bgra[index]))
        def chunk(kind, data):
            return struct.pack('!I', len(data)) + kind + data + struct.pack('!I', zlib.crc32(kind + data) & 0xFFFFFFFF)
        Path(path).write_bytes(b'\x89PNG\r\n\x1a\n' +
                              chunk(b'IHDR', struct.pack('!IIBBBBB', width, height, 8, 2, 0, 0, 0)) +
                              chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b''))
    finally:
        g.SelectObject(memory, old)
        g.DeleteObject(bitmap)
        g.DeleteDC(memory)
        u.ReleaseDC(None, screen)


def select_context_item(app, x, y, index):
    assert u.PostMessageW(app, 0x7B, app, (y << 16) | (x & 0xFFFF))
    select_popup_item(index)


def select_tray_item(app, x, y, index):
    # NOTIFYICON_VERSION_4 packs the event and 16-bit icon ID in lParam.
    assert u.PostMessageW(app, 0x8015, (y << 16) | (x & 0xFFFF), (1 << 16) | 0x7B)
    select_popup_item(index)

    # A menu HWND disappears before TrackPopupMenu's caller finishes restoring
    # foreground and dispatching its command. Wait for the temporary owner to
    # be destroyed, then synchronize with the application's message handler.
    deadline = time.monotonic() + 3
    while u.FindWindowW('STATIC', 'WinHalo tray menu') and time.monotonic() < deadline:
        pump()
    assert not u.FindWindowW('STATIC', 'WinHalo tray menu'), 'Tray menu owner did not close'
    send(app, 0)


def select_popup_item(index):
    deadline = time.monotonic() + 3
    menu_window = None
    while time.monotonic() < deadline:
        menu_window = u.FindWindowW('#32768', None)
        if menu_window:
            break
        pump()
    assert menu_window, 'Context menu did not appear'
    # Menu windows exist before the system's opening animation has settled.
    pump(0.25)
    menu = send(menu_window, 0x1E1)  # MN_GETHMENU
    item = w.RECT()
    assert u.GetMenuItemRect(None, menu, index, c.byref(item))
    click_at((item.left + item.right) // 2, (item.top + item.bottom) // 2)
    deadline = time.monotonic() + 3
    while u.FindWindowW('#32768', None) and time.monotonic() < deadline:
        pump()
    assert not u.FindWindowW('#32768', None), 'Context menu did not close after selection'


def main():
    executable = Path(sys.argv[1] if len(sys.argv) > 1 else 'x64/Release/WinHalo.exe').resolve()
    if '--allow-existing' not in sys.argv:
        assert not u.FindWindowW('WinHalo.NativeIslandWindow', None), 'Close existing WinHalo before testing'
    original_cursor = w.POINT()
    u.GetCursorPos(c.byref(original_cursor))
    foreground = u.GetForegroundWindow()
    instance = k.GetModuleHandleW(None)
    brush = g.CreateSolidBrush(0x00D8E8F0)
    cls = WNDCLASS(0, probe_proc, 0, 0, instance, None, None, brush, None, 'WinHaloSmokeProbe')
    assert u.RegisterClassW(c.byref(cls))
    probe = app = None
    process = process_handle = None
    try:
        # Probe belongs to another process than the application under test.
        probe = u.CreateWindowExW(0x08000088, cls.name, 'WinHalo test probe',
            0x90000000, 0, 0, 1000, 600, None, None, instance, None)
        assert probe
        data_dir = Path('artifacts/smoke-data').resolve()
        data_dir.mkdir(parents=True, exist_ok=True)
        settings_path = data_dir / 'settings.json'
        if settings_path.exists():
            settings_path.unlink()
        # The geometry/settings check must remain independent of live media and audio.
        environment = dict(os.environ, WINHALO_DATA_DIR=str(data_dir), WINHALO_DISABLE_SERVICES='1')
        process = subprocess.Popen([str(executable)], creationflags=0x08000000, env=environment)
        process_handle = k.OpenProcess(0x1000, False, process.pid)
        assert process_handle
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            app = owned_window(process.pid)
            if app and u.IsWindowVisible(app):
                break
            assert process.poll() is None, 'Application exited during startup'
            pump()
        assert app and u.IsWindowVisible(app), 'Application window not found'
        assert u.GetForegroundWindow() == foreground, 'Startup changed foreground: before=' + str(foreground) + ', after=' + str(u.GetForegroundWindow()) + ', app=' + str(app)
        rect = w.RECT()
        assert u.GetWindowRect(app, c.byref(rect))
        dpi = u.GetDpiForWindow(app)
        scale = dpi / 96
        cx = rect.left + round(240 * scale)
        cy = rect.top + round(38 * scale)
        # Position the probe below the host without changing application Z order.
        bind(u, 'SetWindowPos', w.BOOL, w.HWND, w.HWND, c.c_int, c.c_int, c.c_int, c.c_int, w.UINT)
        assert u.SetWindowPos(probe, None, rect.left, rect.top, rect.right - rect.left,
                              rect.bottom - rect.top, 0x14)  # NOZORDER | NOACTIVATE
        style = u.GetWindowLongW(app, -16) & 0xFFFFFFFF
        exstyle = u.GetWindowLongW(app, -20) & 0xFFFFFFFF
        assert style & 0x80000000 and not style & 0x00C40000, 'Window has a frame'
        assert exstyle & 0x08200088 == 0x08200088, 'Required composition styles absent'
        assert not exstyle & 0x80000, 'Composition window incorrectly uses layered style'
        assert not exstyle & 0x20, 'Entire host was made click-through'
        reveal_capsule(app)
        assert u.WindowFromPoint(w.POINT(cx, cy)) == app, 'Capsule cannot receive mouse input'
        assert send(app, 0x84, lp=(cy << 16) | (cx & 0xFFFF)) == 1, 'Capsule hit test failed'
        click_at(cx, cy)
        pump(1.2)
        assert not clicks, 'Capsule click leaked into probe'
        assert u.GetForegroundWindow() == foreground, 'Capsule click stole foreground focus'
        assert region_size(app)[1] < 90, 'Clock click opened a removed module home'
        select_context_item(app, cx, cy, 8)  # Only explicit settings can open a no-player panel.
        pump(1.2)
        expanded_y = rect.top + round(110 * scale)
        assert u.WindowFromPoint(w.POINT(cx, expanded_y)) == app, 'Expanded hit region did not grow'
        capture_host(rect, 'artifacts/expanded-settings.png')
        after_expansion = w.RECT()
        assert u.GetWindowRect(app, c.byref(after_expansion))
        assert (after_expansion.left, after_expansion.top, after_expansion.right, after_expansion.bottom) == (
            rect.left, rect.top, rect.right, rect.bottom), 'Animation changed host window size or position'
        close_x = rect.left + round(415 * scale)
        close_y = rect.top + round(51 * scale)
        click_at(close_x, close_y)
        pump(1.2)
        empty_x, empty_y = cx, rect.top + round(100 * scale)
        assert u.WindowFromPoint(w.POINT(empty_x, empty_y)) == probe, 'Transparent host blocks another process'
        click_at(empty_x, empty_y)
        assert len(clicks) == 1, 'Transparent area failed to pass a real click to another process'
        corner_x, corner_y = rect.left + round(150 * scale), rect.top + round(16 * scale)
        assert u.WindowFromPoint(w.POINT(corner_x, corner_y)) == probe, 'Rounded corner region blocks probe'
        click_at(corner_x, corner_y)
        assert len(clicks) == 2, 'Rounded corner failed click-through; clicks=' + str(len(clicks))
        pump(1.2)
        dc = u.GetDC(None)
        try:
            material = g.GetPixel(dc, rect.left + round(190 * scale), cy)
            assert 10 < (material & 255) < 80 and 10 < ((material >> 8) & 255) < 80, 'Semitransparent dark material is missing'
            assert g.GetPixel(dc, empty_x, empty_y) == 0x00D8E8F0, 'Background is not truly transparent'
        finally:
            u.ReleaseDC(None, dc)
        gdi_baseline = u.GetGuiResources(process_handle, 0)
        cpu_before = process_cpu(process_handle)
        idle_start = time.monotonic()
        pump(1)
        idle_duration = time.monotonic() - idle_start
        idle_cpu_ms = process_cpu(process_handle) - cpu_before
        assert idle_cpu_ms < 100, 'Idle application is consuming CPU continuously'
        # WM_HOTKEY checks routing without sending keystrokes to the user's foreground app.
        send(app, 0x312, 1)
        assert not u.IsWindowVisible(app), 'Hide command failed'
        send(app, 0x312, 1)
        assert u.IsWindowVisible(app), 'Show command failed'
        send(app, 0x312, 1)
        assert not u.IsWindowVisible(app), 'Hide command failed before tray restore'
        send(app, 0x8015, lp=(1 << 16) | 0x400)  # NIN_SELECT, icon ID 1
        assert u.IsWindowVisible(app), 'Tray mouse selection did not restore the hidden island'
        send(app, 0x8015, lp=(1 << 16) | 0x400)
        assert not u.IsWindowVisible(app), 'Tray mouse selection did not hide the island'
        send(app, 0x8015, lp=(1 << 16) | 0x401)  # NIN_KEYSELECT
        assert u.IsWindowVisible(app), 'Tray keyboard selection did not restore the island'
        assert u.GetForegroundWindow() == foreground, 'Tray selection stole foreground focus'
        send(app, 0x8015, lp=(1 << 16) | 0x400)
        select_tray_item(app, cx, cy, 0)  # Show, while the island is hidden
        assert u.IsWindowVisible(app), 'Tray menu could not restore the hidden island'
        assert u.GetForegroundWindow() == foreground, 'Tray menu did not restore foreground focus'
        send(app, 0x312, 3)  # Panel request is inert without a media session.
        pump(1.2)
        assert u.WindowFromPoint(w.POINT(cx, empty_y)) == probe, 'No-player hotkey opened an obsolete module panel'
        select_tray_item(app, cx, cy, 0)  # Hide
        assert not u.IsWindowVisible(app), 'Tray menu did not hide the island'
        select_tray_item(app, cx, cy, 2)  # Settings, while the island is hidden
        pump(1.2)
        assert u.IsWindowVisible(app), 'Tray Settings did not show the island'
        assert u.WindowFromPoint(w.POINT(cx, empty_y)) == app, 'Tray Settings did not expand the panel'
        click_at(close_x, close_y)
        pump(1.2)
        assert u.GetForegroundWindow() == foreground, 'Tray Settings did not restore foreground focus'
        gdi_after_tray = u.GetGuiResources(process_handle, 0)
        # User32 creates popup-menu font/paint caches on first use. Check that
        # repeated menus stop allocating before measuring animation resources.
        tray_gdi_peak = gdi_after_tray
        tray_gdi_samples = []
        for _ in range(20):
            select_tray_item(app, cx, cy, 0)
            assert not u.IsWindowVisible(app), 'Repeated tray menu did not hide the island'
            select_tray_item(app, cx, cy, 0)
            assert u.IsWindowVisible(app), 'Repeated tray menu did not show the island'
            # TrackPopupMenu dismissal returns before deferred menu paint cleanup.
            pump(0.2)
            tray_gdi_peak = max(tray_gdi_peak, u.GetGuiResources(process_handle, 0))
            tray_gdi_samples.append(u.GetGuiResources(process_handle, 0))
        print('Tray GDI samples after initial menus:', tray_gdi_samples)
        # Menu painting can briefly own an extra GDI object (observed 18->19->18).
        # Compare idle counts to the warmed baseline to detect retained allocations.
        pump(1)
        tray_gdi_settled = []
        for _ in range(5):
            pump(0.2)
            tray_gdi_settled.append(u.GetGuiResources(process_handle, 0))
        assert all(count <= gdi_after_tray for count in tray_gdi_settled), (
            'GDI objects retained after repeated tray menus: baseline=' + str(gdi_after_tray) +
            ', settled=' + str(tray_gdi_settled) + ', during_menus=' + str(tray_gdi_samples))
        gdi_before_reversals = u.GetGuiResources(process_handle, 0)
        reveal_capsule(app)
        for _ in range(100):
            send(app, 0x201, lp=(round(38 * scale) << 16) | round(240 * scale))
            send(app, 0x202, lp=(round(38 * scale) << 16) | round(240 * scale))
        pump(1.2)
        if u.WindowFromPoint(w.POINT(cx, empty_y)) != probe:
            region = g.CreateRectRgn(0, 0, 0, 0)
            box = w.RECT()
            u.GetWindowRgn(app, region)
            g.GetRgnBox(region, c.byref(box))
            g.DeleteObject(region)
            raise AssertionError('Repeated clock clicks opened an obsolete panel; region=' +
                                 str((box.left, box.top, box.right, box.bottom)))
        gdi_after_reversals = u.GetGuiResources(process_handle, 0)
        assert gdi_after_reversals <= gdi_before_reversals + 3, (
            'GDI objects grew after repeated ignored clock clicks: baseline=' + str(gdi_baseline) +
            ', after_tray=' + str(gdi_after_tray) + ', before_reversals=' + str(gdi_before_reversals) +
            ', after_reversals=' + str(gdi_after_reversals))
        send(app, 0x312, 3)
        pump(1.2)
        assert u.WindowFromPoint(w.POINT(cx, empty_y)) == probe, 'No-player panel hotkey opened a removed module home'
        select_context_item(app, cx, cy, 8)  # Settings
        pump(1.2)
        capture_host(rect, 'artifacts/settings.png')
        click_at(cx, rect.top + round(125 * scale))
        pump()
        assert json.loads(settings_path.read_text(encoding='utf-8'))['speed'] == 1.5, 'Settings speed was not saved'
        click_at(cx, rect.top + round(175 * scale))
        pump()
        assert json.loads(settings_path.read_text(encoding='utf-8'))['reducedMotion'] is True, 'Reduced motion was not saved'
        click_at(close_x, close_y)
        pump()
        assert u.WindowFromPoint(w.POINT(cx, empty_y)) == probe, 'Reduced-motion collapse failed'
        if 'Debug' in str(executable):
            select_context_item(app, cx, cy, 13)  # Recreate graphics device
            pump(1)
            assert u.WindowFromPoint(w.POINT(cx, cy)) == app, 'Device recreation broke hit testing'
        select_context_item(app, cx, cy, 0)  # Recenter
        assert u.GetForegroundWindow() == foreground, 'Context menu stole foreground focus'
        select_context_item(app, cx, cy, 5)  # Exit
        assert process.wait(timeout=5) == 0, 'Application did not exit cleanly'
        print(json.dumps({'result': 'PASS', 'dpi': dpi, 'idle_sample_seconds': round(idle_duration, 3),
                         'idle_cpu_ms': idle_cpu_ms, 'gdi_objects': {'baseline': gdi_baseline,
                            'after_tray': gdi_after_tray, 'repeated_tray_peak': tray_gdi_peak,
                            'settled_after_repeated_tray': tray_gdi_settled,
                            'before_reversals': gdi_before_reversals, 'after_reversals': gdi_after_reversals},
                         'checks': [
            'borderless/topmost/toolwindow/noactivate styles', 'no startup or click focus theft',
            'capsule input', 'real cross-process clicks through empty host and rounded corner',
            'dark translucent capsule and transparent background pixels', 'hide/show message routing',
            'version-4 tray mouse and keyboard callbacks restore a hidden island',
            'tray menu show/hide, settings, and foreground restoration',
            'stable GDI resources across 40 repeated tray menus',
            'spring expansion and close button', 'fixed host size during animation',
            '100 clock clicks keep removed panels closed', 'settings JSON persistence', 'reduced motion',
            'context menu recenter and exit', 'clean exit']}, ensure_ascii=False))
    finally:
        if process and process.poll() is None:
            if app:
                u.PostMessageW(app, 0x10, 0, 0)
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.terminate()
                process.wait(timeout=5)
        if probe:
            u.DestroyWindow(probe)
        if process_handle:
            k.CloseHandle(process_handle)
        u.UnregisterClassW(cls.name, instance)
        g.DeleteObject(brush)
        u.SetCursorPos(original_cursor.x, original_cursor.y)


if __name__ == '__main__':
    main()
