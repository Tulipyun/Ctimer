"""Exercise the actual GUI auto-scheduling path against a local NTP reference.

The application restricts test input to its own foreground window. No public
NTP server or other application's input is used by this test.
"""
import argparse
import json
import pathlib
import socket
import struct
import subprocess
import threading
import time
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--executable", type=pathlib.Path, default=ROOT / "dist" / "Ctimer.exe")
args = parser.parse_args()
executable = args.executable.resolve()
if not executable.is_file():
    parser.error(f"Executable not found: {executable}")
(ROOT / "build").mkdir(exist_ok=True)
OUTPUT = pathlib.Path(tempfile.mkdtemp(prefix="ui-verification-", dir=ROOT / "build"))
processes = []
server = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
server.bind(("127.0.0.1", 0))
server.settimeout(0.2)
stop = threading.Event()
requests = []


def ntp_stamp():
    value = time.time_ns()
    seconds, nanoseconds = divmod(value, 1_000_000_000)
    return struct.pack("!II", (seconds + 2_208_988_800) & 0xFFFFFFFF,
                       nanoseconds * (1 << 32) // 1_000_000_000)


def respond():
    while not stop.is_set():
        try:
            request, peer = server.recvfrom(1024)
        except socket.timeout:
            continue
        if len(request) != 48:
            continue
        received = ntp_stamp()
        requests.append(time.monotonic())
        response = bytearray(48)
        response[0:4] = bytes([0x24, 1, 6, 236])
        response[24:32] = request[40:48]
        response[32:40] = received
        response[40:48] = ntp_stamp()
        server.sendto(response, peer)


config = OUTPUT / "Ctimer.ini"
config.write_text(
    "[General]\nAutoSync=1\nAutoSchedule=1\nSound=0\nTopmost=0\n"
    "SourceCatalogVersion=3\nAutoSystemClock=0\nHoldMs=20\nIntervalMs=100\nRepetitions=1\n"
    f"[Sources]\n127.0.0.1:{server.getsockname()[1]} | loopback | 10\n",
    encoding="utf-8",
)
thread = threading.Thread(target=respond)
thread.start()
try:
    process = subprocess.Popen([str(executable),
                                "--config", str(config), "--auto-smoke-test"])
    processes.append(process)
    code = process.wait(timeout=55)
    report = json.loads((OUTPUT / "logs" / "input-smoke.json").read_text(encoding="utf-8"))
    report["ntp_requests"] = len(requests)
    report["poll_intervals_seconds"] = [round(b-a, 3) for a,b in zip(requests, requests[1:])]
    print(json.dumps(report))
    assert code == 0 and report["passed"]
    assert report["automatic"] and report["preparation_notices"] == 1
    assert report["sync_resumed"] and report["down"] == report["up"] == 1
    assert report["next_hour_scheduled"]
    assert all(9.8 <= interval <= 10.5 for interval in report["poll_intervals_seconds"])
    assert requests
    restored = subprocess.Popen([str(executable),
                                 "--config", str(config), "--restore-smoke-test"])
    processes.append(restored)
    assert restored.wait(timeout=20) == 0
    restore_report = json.loads((OUTPUT / "logs" / "restore-smoke.json").read_text(encoding="utf-8"))
    print(json.dumps(restore_report))
    assert restore_report["restored"] and restore_report["automatic_armed"]
    assert restore_report["action_kind"] == 0
finally:
    for process in processes:
        if process.poll() is None:
            process.terminate()
            process.wait(timeout=5)
    stop.set()
    thread.join(timeout=2)
    server.close()
