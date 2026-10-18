"""Actual WinHalo UI driven by our uniquely identified, silent SMTC fixture.

Controls go directly to the owned fixture through its stdin, never to a user
player. The app is used only for read-only navigation and rendering checks.
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

class MEMORY_COUNTERS(c.Structure):
    _fields_ = [('size', w.DWORD), ('faults', w.DWORD)] + [(name, c.c_size_t) for name in
        ('peak_working', 'working', 'peak_paged', 'paged', 'peak_nonpaged', 'nonpaged', 'pagefile', 'peak_pagefile', 'private')]


def main():
    u, k, g = desktop.u, desktop.k, desktop.g
    if '--allow-existing' not in sys.argv:
        assert not u.FindWindowW('WinHalo.NativeIslandWindow', None), 'Close existing WinHalo'
    desktop.bind(u, 'FindWindowExW', w.HWND, w.HWND, w.HWND, w.LPCWSTR, w.LPCWSTR)
    desktop.bind(u, 'GetWindowThreadProcessId', w.DWORD, w.HWND, c.POINTER(w.DWORD))
    desktop.bind(u, 'GetWindowTextW', c.c_int, w.HWND, w.LPWSTR, c.c_int)
    def owned_window(pid):
        candidate = None
        while True:
            candidate = u.FindWindowExW(None, candidate, 'WinHalo.NativeIslandWindow', None)
            if not candidate: return None
            owner = w.DWORD(); u.GetWindowThreadProcessId(candidate, c.byref(owner))
            if owner.value == pid: return candidate
    foreground = u.GetForegroundWindow()
    cursor = w.POINT(); u.GetCursorPos(c.byref(cursor))
    data = Path('artifacts/media-ui-data').resolve(); data.mkdir(parents=True, exist_ok=True)
    (data / 'settings.json').write_text(json.dumps({'version': 2, 'autoHideFullscreen': False}), encoding='utf-8')
    fixture = process = app = handle = None
    samples = {}
    try:
        fixture = subprocess.Popen([str(Path(sys.argv[2]).resolve()), '--serve'], stdin=subprocess.PIPE,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, creationflags=0x08000000)
        ready = fixture.stdout.readline().strip()
        assert ready.startswith('READY '), 'Owned SMTC fixture did not register: ' + ready
        environment = dict(os.environ, WINHALO_DATA_DIR=str(data))
        environment.pop('WINHALO_DISABLE_SERVICES', None)
        process = subprocess.Popen([str(Path(sys.argv[1]).resolve())], env=environment, creationflags=0x08000000)
        for _ in range(100):
            app = owned_window(process.pid)
            if app and u.IsWindowVisible(app): break
            assert process.poll() is None
            desktop.pump()
        assert app
        handle = k.OpenProcess(0x410, False, process.pid)
        rect = w.RECT(); assert u.GetWindowRect(app, c.byref(rect))
        scale = u.GetDpiForWindow(app) / 96
        # Discover the session while still in the idle line; a slow service
        # startup must not consume the compact capsule's five-second grace.
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            caption = c.create_unicode_buffer(1024); u.GetWindowTextW(app, caption, len(caption))
            if '[' in caption.value: break
            assert process.poll() is None
            desktop.pump(0.1)
        assert '[' in caption.value, 'Media service did not discover a source while idle'
        desktop.reveal_capsule(app)
        # Keep pointer away from the capsule so the collapsed geometry is deterministic.
        u.SetCursorPos(rect.left + 5, rect.top + 5)
        region = g.CreateRectRgn(0, 0, 0, 0); box = w.RECT()
        try:
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline:
                desktop.pump(0.2)
                u.GetWindowRgn(app, region); g.GetRgnBox(region, c.byref(box))
                if abs((box.right - box.left) - round(300 * scale)) <= 5: break
            desktop.pump(0.8)  # Let the spring settle after asynchronous SMTC discovery.
            u.GetWindowRgn(app, region); g.GetRgnBox(region, c.byref(box))
        finally:
            g.DeleteObject(region)
        assert abs((box.right - box.left) - round(300 * scale)) <= 5, 'Media did not take over collapsed clock; region=' + str((box.left, box.top, box.right, box.bottom))
        def capture(name): desktop.capture_host(rect, 'artifacts/now-playing-' + name + '.png')
        def pixel(x, y):
            dc = u.GetDC(None)
            try: return g.GetPixel(dc, rect.left + round(x * scale), rect.top + round(y * scale))
            finally: u.ReleaseDC(None, dc)
        def command(value):
            fixture.stdin.write(value + '\n'); fixture.stdin.flush()
            assert fixture.stdout.readline().strip() == 'ACK ' + value
            desktop.pump(0.5)
        def choose_fixture():
            # Source selection is read-only. Pin the owned session so another user
            # player nominated by Windows after pause cannot replace this test's UI.
            source = '[' + 'WinHalo.MediaFixture.' + str(fixture.pid) + ']'
            report = subprocess.check_output([str(Path(sys.argv[2]).resolve()), '--inspect'], text=True, encoding='utf-8')
            count = sum(line.startswith('Session: ') for line in report.splitlines())
            if count <= 1: return
            for _ in range(count + 2):
                desktop.click_at(rect.left + round(64 * scale), rect.top + round(176 * scale)); desktop.pump(0.5)
                caption = c.create_unicode_buffer(1024); u.GetWindowTextW(app, caption, len(caption))
                if source in caption.value: return
            raise AssertionError('Could not explicitly select the owned fixture')
        def cpu(name):
            before = desktop.process_cpu(handle); desktop.pump(1)
            samples[name] = desktop.process_cpu(handle) - before
        def bars():
            return [pixel(x, y) for x in (357, 362, 367, 372) for y in range(33, 48, 2)]
        capture('collapsed')
        a = bars(); desktop.pump(0.23); b = bars()
        assert a != b, 'Playing equalizer did not change'
        desktop.click_at(rect.left + round(240 * scale), rect.top + round(40 * scale)); desktop.pump(0.8)
        # Opening media must show the focused player immediately, without selecting a module.
        region = g.CreateRectRgn(0, 0, 0, 0)
        try:
            u.GetWindowRgn(app, region); g.GetRgnBox(region, c.byref(box))
            assert abs(box.bottom - box.top - round(200 * scale)) <= 3, 'Opening music showed the module menu'
        finally: g.DeleteObject(region)
        choose_fixture()
        capture('expanded')
        # Capture just the native player, keeping unrelated desktop content out of the preview.
        player_rect = w.RECT(rect.left + round(30 * scale), rect.top + round(16 * scale),
            rect.left + round(450 * scale), rect.top + round(216 * scale))
        desktop.capture_host(player_rect, 'artifacts/now-playing-player.png')
        cover = pixel(80, 76)
        assert 125 <= (cover & 255) <= 155 and 65 <= ((cover >> 8) & 255) <= 95 and 205 <= ((cover >> 16) & 255) <= 235, 'App did not display our fixture artwork'
        def is_cover(color):
            return all(abs(((color >> shift) & 255) - expected) <= 3 for shift, expected in ((0, 140), (8, 80), (16, 220)))
        # Seek only the owned session and verify its real timeline event reaches the bar.
        before = pixel(270, 131)
        command('seek')
        after = pixel(270, 131)
        assert before != after and ((after >> 8) & 255) > 150, 'Real timeline change did not update progress'
        command('pause'); cpu('paused_detail'); capture('paused')
        assert samples['paused_detail'] < 100, 'Paused media detail kept rendering continuously'
        # Read-only lifecycle test: explicit hotkey closes the player without opening its app.
        desktop.send(app, 0x312, 3); desktop.pump(0.8)
        def footprint():
            region = g.CreateRectRgn(0, 0, 0, 0); area = w.RECT()
            try:
                u.GetWindowRgn(app, region); g.GetRgnBox(region, c.byref(area))
                return (area.left, area.top, area.right, area.bottom)
            finally: g.DeleteObject(region)
        closed = footprint()
        # After hover collapse the cursor can stay inside the capsule; use real input for re-entry too.
        desktop.click_at(rect.left + round(240 * scale), rect.top + round(40 * scale)); desktop.pump(0.8)
        capture('reopened')
        assert is_cover(pixel(80, 76)), 'Reopening paused music failed: actual=' + hex(pixel(80, 76)) + ', closed=' + str(closed) + ', reopened=' + str(footprint())
        command('missing-cover'); capture('fallback')
        assert not is_cover(pixel(80, 76)), 'Missing cover did not switch to vector fallback'
        command('play'); cpu('playing_detail')
        desktop.send(app, 0x312, 1); cpu('hidden_playing')
        assert samples['hidden_playing'] < 100, 'Hidden playback kept rendering'
        desktop.bind(k, 'K32GetProcessMemoryInfo', w.BOOL, w.HANDLE, c.POINTER(MEMORY_COUNTERS), w.DWORD)
        memory = MEMORY_COUNTERS(); memory.size = c.sizeof(memory)
        assert k.K32GetProcessMemoryInfo(handle, c.byref(memory), memory.size)
        # Keep WinHalo running across player shutdown and restart. Earlier
        # tests exited the app first and never exercised this lifecycle edge.
        fixture.stdin.write('quit\n'); fixture.stdin.flush(); assert fixture.wait(timeout=5) == 0
        desktop.send(app, 0x312, 1)  # Show.
        # The final session now automatically collapses; another surviving session may keep the player open.
        region = g.CreateRectRgn(0, 0, 0, 0)
        try:
            u.GetWindowRgn(app, region); g.GetRgnBox(region, c.byref(box))
            if box.right - box.left > round(350 * scale): desktop.send(app, 0x312, 3)
        finally: g.DeleteObject(region)
        u.SetCursorPos(rect.left + 5, rect.top + 5)
        def wait_width(width):
            # Recovery may outlive the five-second idle grace period. Reveal the
            # capsule and hold inside it while asynchronous SMTC settles.
            desktop.reveal_capsule(app)
            at = desktop.point_dip(app, 240, 38); u.SetCursorPos(at.x, at.y)
            region = g.CreateRectRgn(0, 0, 0, 0); box = w.RECT()
            try:
                deadline = time.monotonic() + 10
                while time.monotonic() < deadline:
                    assert process.poll() is None, 'Player exit crashed WinHalo'
                    desktop.pump(0.2)
                    u.GetWindowRgn(app, region); g.GetRgnBox(region, c.byref(box))
                    if abs(box.right - box.left - round(width * scale)) <= round(11 * scale): return
                print(subprocess.check_output([str(Path(sys.argv[2]).resolve()), '--inspect'], text=True, encoding='utf-8'))
                raise AssertionError('Media lifecycle geometry did not settle: expected=' + str(width) +
                    ', bounds=' + str((box.left, box.top, box.right, box.bottom)))
            finally:
                g.DeleteObject(region)
        def remaining_sessions():
            # Read-only inspection; another user player may remain registered.
            report = subprocess.check_output([str(Path(sys.argv[2]).resolve()), '--inspect'], text=True, encoding='utf-8')
            return [int(line.rsplit('status=', 1)[1]) for line in report.splitlines()
                if line.startswith('Session: ') and 'status=' in line]
        remaining = remaining_sessions()
        wait_width(300 if any(status > 0 for status in remaining) else 180)
        # Switching to another player can keep the same width while its
        # asynchronous metadata/artwork is still loading (bounded at 2.5 s).
        desktop.pump(3)
        cpu('after_player_exit')
        if 4 not in remaining:
            assert samples['after_player_exit'] < 100, 'Exited player left continuous rendering'
        fixture = subprocess.Popen([str(Path(sys.argv[2]).resolve()), '--serve'], stdin=subprocess.PIPE,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, creationflags=0x08000000)
        assert fixture.stdout.readline().strip().startswith('READY '), 'Restarted fixture did not register'
        wait_width(300)
        # Other real players can remain Windows' current session after restart.
        # Select our new identity through the read-only source badge before
        # asserting its artwork; never send media controls to another player.
        desktop.send(app, 0x312, 3); desktop.pump(0.8)
        choose_fixture()
        desktop.send(app, 0x312, 3); desktop.pump(0.8)
        wait_width(300)
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            desktop.pump(0.2)
            restarted_cover = pixel(120, 40)
            if is_cover(restarted_cover): break
        assert is_cover(restarted_cover), 'Restarted fixture artwork did not replace the exited session'
        fixture.terminate(); fixture.wait(timeout=5)  # Only our owned silent fixture.
        remaining = remaining_sessions()
        wait_width(300 if any(status > 0 for status in remaining) else 180)
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            assert process.poll() is None, 'Abrupt player exit crashed WinHalo'
            desktop.pump(0.2)
            if not is_cover(pixel(120, 40)): break
        assert not is_cover(pixel(120, 40)), 'Abrupt player exit retained old artwork'
        desktop.send(app, 0x10); assert process.wait(timeout=5) == 0
        assert u.GetForegroundWindow() == foreground, 'Read-only media UI stole focus'
        print(json.dumps({'result': 'PASS', 'owned_silent_fixture': True, 'app_media_commands_sent': 0,
            'cpu_ms_per_one_second_sample': samples,
            'memory_mb_at_end': {'working_set': round(memory.working / 1048576, 2), 'private': round(memory.private / 1048576, 2)},
            'checks': ['real SMTC takeover', 'direct focused player expansion', 'animated equalizer', 'cover and title metadata',
                'real timeline seek event', 'paused and hidden rendering stop', 'missing-cover fallback',
                'player exit falls back to clock or remaining session', 'player restart restores media',
                'abrupt player exit keeps app alive and clears old artwork']}))
    finally:
        if process and process.poll() is None:
            if app: u.PostMessageW(app, 0x10, 0, 0)
            try: process.wait(timeout=5)
            except subprocess.TimeoutExpired: process.terminate(); process.wait(timeout=5)
        if fixture and fixture.poll() is None:
            fixture.stdin.write('quit\n'); fixture.stdin.flush()
            try: fixture.wait(timeout=5)
            except subprocess.TimeoutExpired: fixture.terminate(); fixture.wait(timeout=5)
        if handle: k.CloseHandle(handle)
        u.SetCursorPos(cursor.x, cursor.y)
        if foreground: u.SetForegroundWindow(foreground)


if __name__ == '__main__': main()
