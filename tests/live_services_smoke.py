"""Read-only desktop check with real media/audio services enabled.

Opens/closes WinHalo and its panel; never sends media or volume commands.
Run with the desktop unlocked and no other WinHalo instance running.
"""
import ctypes as c
import json
import os
from pathlib import Path
import subprocess
import sys
import time
import window_smoke as desktop
from ctypes import wintypes as w


def main():
    executable = Path(sys.argv[1] if len(sys.argv) > 1 else 'x64/Release/WinHalo.exe').resolve()
    if '--allow-existing' not in sys.argv:
        assert not desktop.u.FindWindowW('WinHalo.NativeIslandWindow', None), 'Close existing WinHalo before testing'
    data = Path('artifacts/live-service-data').resolve()
    data.mkdir(parents=True, exist_ok=True)
    environment = dict(os.environ, WINHALO_DATA_DIR=str(data))
    environment.pop('WINHALO_DISABLE_SERVICES', None)
    foreground = desktop.u.GetForegroundWindow()
    samples = []
    for cycle in range(3):
        process = subprocess.Popen([str(executable)], creationflags=0x08000000, env=environment)
        app = handle = None
        try:
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline:
                app = desktop.owned_window(process.pid)
                if app and desktop.u.IsWindowVisible(app):
                    break
                assert process.poll() is None, 'Application exited during startup'
                desktop.pump()
            assert app and desktop.u.IsWindowVisible(app), 'Live application did not become visible'
            assert desktop.u.GetForegroundWindow() == foreground, 'Live service startup changed focus'
            handle = desktop.k.OpenProcess(0x1000, False, process.pid)
            assert handle
            desktop.pump(2)
            before = desktop.process_cpu(handle)
            desktop.pump(1)
            samples.append(desktop.process_cpu(handle) - before)
            # Services should not continuously render or busy-poll while collapsed.
            assert samples[-1] < 200, 'Live services consumed excessive CPU in a short idle sample'
            desktop.send(app, 0x312, 3)
            desktop.pump(1)
            if cycle == 0:
                rect = w.RECT()
                assert desktop.u.GetWindowRect(app, c.byref(rect))
                desktop.capture_host(rect, 'artifacts/live-services.png')
            desktop.send(app, 0x312, 3)
            desktop.pump(1)
            desktop.send(app, 0x312, 1)
            assert not desktop.u.IsWindowVisible(app)
            desktop.send(app, 0x8015, lp=(1 << 16) | 0x400)
            assert desktop.u.IsWindowVisible(app), 'Tray failed to restore live application'
            assert desktop.u.GetForegroundWindow() == foreground, 'Live panel/tray changed focus'
            desktop.send(app, 0x10)
            assert process.wait(timeout=5) == 0, 'Live service application failed to stop cleanly'
        finally:
            if process.poll() is None:
                if app:
                    desktop.u.PostMessageW(app, 0x10, 0, 0)
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.terminate()
                    process.wait(timeout=5)
            if handle:
                desktop.k.CloseHandle(handle)
    print(json.dumps({'result': 'PASS', 'real_services_enabled': True, 'restart_cycles': 3,
                      'idle_cpu_ms_per_one_second_sample': samples,
                      'media_and_volume_commands_sent': 0}))


if __name__ == '__main__':
    main()
