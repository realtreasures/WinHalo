"""Two owned silent players: selection, exact command routing, app restoration and outside-click pass-through."""
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
        assert not u.FindWindowW('WinHalo.NativeIslandWindow', None), 'Close existing WinHalo for interactive regression'
    desktop.bind(u, 'FindWindowExW', w.HWND, w.HWND, w.HWND, w.LPCWSTR, w.LPCWSTR)
    desktop.bind(u, 'GetWindowThreadProcessId', w.DWORD, w.HWND, c.POINTER(w.DWORD))
    def owned_window(pid):
        candidate = None
        while True:
            candidate = u.FindWindowExW(None, candidate, 'WinHalo.NativeIslandWindow', None)
            if not candidate: return None
            owner = w.DWORD(); u.GetWindowThreadProcessId(candidate, c.byref(owner))
            if owner.value == pid: return candidate
    desktop.bind(u, 'GetWindowTextW', c.c_int, w.HWND, w.LPWSTR, c.c_int)
    desktop.bind(u, 'SetForegroundWindow', w.BOOL, w.HWND)
    desktop.bind(u, 'SetWindowPos', w.BOOL, w.HWND, w.HWND, c.c_int, c.c_int, c.c_int, c.c_int, w.UINT)
    foreground = u.GetForegroundWindow()
    cursor = w.POINT(); u.GetCursorPos(c.byref(cursor))
    data = Path('artifacts/media-switch-data').resolve(); data.mkdir(parents=True, exist_ok=True)
    (data / 'settings.json').write_text(json.dumps({'version': 2, 'autoHideFullscreen': False}), encoding='utf-8')
    env = dict(os.environ, WINHALO_DATA_DIR=str(data)); env.pop('WINHALO_DISABLE_SERVICES', None)
    fixture_path = str(Path(sys.argv[2]).resolve())
    fixtures = []; observer = process = app = probe = handle = None
    brush = g.CreateSolidBrush(0x00E0EAF0); instance = k.GetModuleHandleW(None)
    cls = desktop.WNDCLASS(0, desktop.probe_proc, 0, 0, instance, None, None, brush, None, 'WinHaloMediaSwitchProbe')
    assert u.RegisterClassW(c.byref(cls))
    def start(arg):
        child = subprocess.Popen([fixture_path, arg], env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, text=True, encoding='utf-8', creationflags=0x08000000)
        assert child.stdout.readline().strip().startswith('READY '), 'Fixture failed to register'
        return child
    def request(child, command):
        child.stdin.write(command + '\n'); child.stdin.flush()
        return child.stdout.readline().strip()
    def command(child, value): assert request(child, value) == 'ACK ' + value
    def state(): return json.loads(request(observer, 'state'))
    def wait(predicate, message, seconds=8):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            value = state(); desktop.pump(0.1)
            if predicate(value): return value
        raise AssertionError(message + ': ' + json.dumps(value))
    def stats(child): return json.loads(request(child, 'stats'))
    def stop(child):
        if child and child.poll() is None:
            child.stdin.write('quit\n'); child.stdin.flush()
            try: child.wait(timeout=5)
            except subprocess.TimeoutExpired: child.terminate(); child.wait(timeout=5)
    try:
        fixtures = [start('--serve'), start('--serve-alt')]
        identities = ['WinHalo.MediaFixture.' + str(child.pid) for child in fixtures]
        observer = start('--observe')
        initial = wait(lambda s: all(any(e['source'] == source for e in s['sessions']) for source in identities),
            'Service did not enumerate both owned sessions')
        ids = {e['source']: int(e['id']) for e in initial['sessions'] if e['source'] in identities}
        assert ids[identities[0]] != ids[identities[1]], 'Session IDs were not independent'
        def select(source):
            for _ in range(len(state()['sessions']) + 1):
                value = state()
                if value['source'] == source: return value
                command(observer, 'next ' + str(int(value['id'])))
                wait(lambda s: s['id'] != value['id'], 'Cycle failed')
            raise AssertionError('Owned source could not be selected')
        select(identities[1])
        a = select(identities[0]); assert a['iconBytes'] > 0, 'Desktop software icon was not resolved'
        command(fixtures[1], 'pause'); command(fixtures[1], 'play'); desktop.pump(0.5)
        assert state()['source'] == identities[0], 'Manual selection was stolen by another player'
        command(observer, 'toggle ' + str(ids[identities[0]]) + ' ' + identities[0])
        wait(lambda s: s['source'] == identities[0] and not s['playing'], 'Selected player did not pause')
        assert not stats(fixtures[0])['playing'] and stats(fixtures[1])['playing'], 'Toggle reached the wrong source'
        select(identities[1])
        before_a, before_b = stats(fixtures[0]), stats(fixtures[1])
        # Same expected source with a wrong opaque ID must also be rejected.
        command(observer, 'next-track ' + str(ids[identities[0]]) + ' ' + identities[1])
        wait(lambda s: bool(s['error']), 'Stale session token was not rejected')
        assert stats(fixtures[0]) == before_a and stats(fixtures[1]) == before_b
        command(observer, 'previous ' + str(ids[identities[1]]) + ' ' + identities[1])
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline and stats(fixtures[1])['pressed'] == before_b['pressed']: desktop.pump(0.1)
        assert stats(fixtures[1])['pressed'] == before_b['pressed'] + 1 and stats(fixtures[0]) == before_a
        stop(observer); observer = None

        # Drive the real UI only after identifying its selected source through its native caption.
        process = subprocess.Popen([str(Path(sys.argv[1]).resolve())], env=env, creationflags=0x08000000)
        for _ in range(100):
            app = owned_window(process.pid)
            if app and u.IsWindowVisible(app): break
            assert process.poll() is None; desktop.pump()
        assert app
        rect = w.RECT(); u.GetWindowRect(app, c.byref(rect)); scale = u.GetDpiForWindow(app) / 96
        def click(x, y): desktop.click_at(rect.left + round(x * scale), rect.top + round(y * scale))
        probe = u.CreateWindowExW(0x08000088, cls.name, 'WinHalo media switch probe', 0x90000000,
            rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, None, None, instance, None)
        assert probe
        assert u.SetWindowPos(app, w.HWND(-1), 0, 0, 0, 0, 0x13), 'Restore island above the input probe'
        def caption():
            value = c.create_unicode_buffer(1024); u.GetWindowTextW(app, value, len(value)); return value.value
        def selected():
            text = caption()
            return next((source for source in identities if '[' + source + ']' in text), None)
        def size():
            region = g.CreateRectRgn(0, 0, 0, 0); box = w.RECT()
            try:
                u.GetWindowRgn(app, region); g.GetRgnBox(region, c.byref(box))
                return (box.right - box.left) / scale, (box.bottom - box.top) / scale
            finally: g.DeleteObject(region)
        def wait_ui(predicate, message):
            deadline = time.monotonic() + 8
            while time.monotonic() < deadline:
                assert process.poll() is None, 'WinHalo exited during UI regression'
                desktop.pump(0.1)
                if predicate(): return
            raise AssertionError(message + ': ' + caption())
        wait_ui(lambda: size()[0] < 350 and '[' in caption(), 'Media UI did not discover a session')
        desktop.reveal_capsule(app)
        click(240, 40); desktop.pump(0.8)
        assert abs(size()[1] - 200) < 3
        for _ in range(12):
            if selected(): break
            click(64, 176); desktop.pump(0.5)
        source = selected(); assert source, 'UI could not select an owned source'
        target = identities.index(source); other = 1 - target
        original = stats(fixtures[target]); other_before = stats(fixtures[other])
        before_focus = u.GetForegroundWindow()
        click(240, 176)
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline and stats(fixtures[target])['playing'] == original['playing']: desktop.pump(0.1)
        assert stats(fixtures[target])['playing'] != original['playing'] and stats(fixtures[other]) == other_before
        assert u.GetForegroundWindow() == before_focus, 'Playback control stole keyboard focus'
        assert abs(size()[1] - 200) < 3, 'Playback control collapsed the player'
        # The old close-arrow area is now ordinary card body: it restores exactly the selected app.
        click(425, 35); desktop.pump(0.5)
        software_window = u.FindWindowW('WinHalo.OwnedMediaFixture', source)
        assert software_window and u.GetForegroundWindow() == software_window, 'Card did not open its selected software: expected=' + str(software_window) + ', visible=' + str(u.IsWindowVisible(software_window)) + ', actual=' + str(u.GetForegroundWindow()) + ', caption=' + caption()
        wait_ui(lambda: size()[0] < 350 and size()[1] < 90, 'Opening software did not collapse the card into its capsule')
        assert selected() == source, 'Opening software changed the selected player'
        software_focus = u.GetForegroundWindow()
        click(240, 40); desktop.pump(0.8)
        assert abs(size()[1] - 200) < 3 and selected() == source, 'Capsule did not reopen the same player'
        assert u.GetForegroundWindow() == software_focus, 'Reopening the capsule stole keyboard focus'
        click(64, 176); desktop.pump(0.5)
        assert caption().find('[' + source + ']') < 0, 'Software icon did not change the selected source'
        assert u.GetForegroundWindow() == software_focus, 'Switching source opened another app'
        assert abs(size()[1] - 200) < 3, 'Switching source collapsed the player'
        # Moving out preserves the card; clicking out collapses it and reaches the probe.
        u.SetCursorPos(rect.left + round(240 * scale), rect.top + round(270 * scale)); desktop.pump(0.3)
        assert abs(size()[1] - 200) < 3, 'Mouse leave collapsed the player before a click'
        desktop.clicks.clear(); click(240, 270); desktop.pump(0.8)
        assert desktop.clicks and size()[0] < 350, 'Outside click failed: clicks=' + str(desktop.clicks) + ', size=' + str(size()) + ', probe=' + str(probe) + ', hit=' + str(u.WindowFromPoint(w.POINT(rect.left + round(240 * scale), rect.top + round(270 * scale))))
        assert u.GetForegroundWindow() == software_focus, 'Outside collapse stole focus'
        handle = k.OpenProcess(0x410, False, process.pid); assert handle
        baseline = u.GetGuiResources(handle, 0)
        for _ in range(8):
            click(240, 40); desktop.pump(0.4)
            assert size()[0] > 400
            click(240, 270); desktop.pump(0.4)
            assert size()[0] < 350
        assert u.GetGuiResources(handle, 0) <= baseline + 2, 'Source tooltip or outside-click lifecycle leaked GDI objects'
        click(240, 40); desktop.pump(0.8)
        for _ in range(12):
            if selected(): break
            click(64, 176); desktop.pump(0.5)
        chosen = selected(); assert chosen
        player_rect = w.RECT(rect.left + round(30 * scale), rect.top + round(16 * scale),
            rect.left + round(450 * scale), rect.top + round(216 * scale))
        desktop.capture_host(player_rect, 'artifacts/now-playing-multi-player.png')
        stop(fixtures[identities.index(chosen)])
        wait_ui(lambda: '[' + chosen + ']' not in caption(), 'Exited selected source remained attached')
        assert process.poll() is None
        stop(fixtures[1 - identities.index(chosen)]); desktop.pump(1)
        report = subprocess.check_output([fixture_path, '--inspect'], text=True, encoding='utf-8')
        if not any(line.startswith('Session: ') and 'status=0' not in line for line in report.splitlines()):
            wait_ui(lambda: size()[0] < 200, 'Last session exit did not restore clock capsule')
        desktop.send(app, 0x10); assert process.wait(timeout=5) == 0
        print(json.dumps({'result': 'PASS', 'owned_players': 2,
            'checks': ['unique session IDs', 'real desktop app icon', 'manual selection persists',
                'source and stale-ID command guards', 'independent playback and previous control',
                'card restores selected software and collapses into capsule', 'capsule reopens same player',
                'playback and source controls keep card expanded', 'source icon switches without app activation',
                'mouse leave stays open', 'outside click collapses and reaches underlying window',
                '8 observer/tooltip layout cycles with bounded GDI resources', 'selected and final player exit recovery']}))
    finally:
        if process and process.poll() is None:
            if app: u.PostMessageW(app, 0x10, 0, 0)
            try: process.wait(timeout=5)
            except subprocess.TimeoutExpired: process.terminate(); process.wait(timeout=5)
        stop(observer)
        for fixture in fixtures: stop(fixture)
        if probe: u.DestroyWindow(probe)
        if handle: k.CloseHandle(handle)
        u.UnregisterClassW(cls.name, instance); g.DeleteObject(brush)
        u.SetCursorPos(cursor.x, cursor.y)
        if foreground: u.SetForegroundWindow(foreground)


if __name__ == '__main__': main()
