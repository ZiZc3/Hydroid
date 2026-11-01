#!/usr/bin/env python3
# live_log.py — the console's log as it is written: Hydroid serves it on port 9025 (app_main.c,
# log_server). Shows the lines that matter in colour (fps, slow file calls, stalls, crashes,
# missing calls, Java not found, games picked) and keeps every line in logs/live-<time>.log.
# Reconnects by itself (the app closed, started again).
#
#   wsl python3 tools/live_log.py              (the lines that matter)
#   wsl python3 tools/live_log.py --all        (every line)
#   wsl python3 tools/live_log.py --grep fps   (only lines with "fps")
#   python3 tools/live_log.py --host <console IP> --port 9025

import os, re, socket, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
LOGS = os.path.join(os.path.dirname(HERE), 'logs')


def arg(name, default):
    if name in sys.argv:
        i = sys.argv.index(name)
        if i + 1 < len(sys.argv):
            return sys.argv[i + 1]
    return default


HOST = arg('--host', os.environ.get('PS5', ''))
PORT = int(arg('--port', '9025'))
ALL = '--all' in sys.argv
GREP = arg('--grep', None)

RED, YEL, GRN, CYA, MAG, DIM, BLD, RST = '\033[91m', '\033[93m', '\033[92m', '\033[96m', '\033[95m', '\033[2m', '\033[1m', '\033[0m'
RULES = [  # (pattern, colour) - first match wins
    (re.compile(r'CRASH|crash:|SIGSEGV|abort: C#|Abort message|, stopping'), RED),
    (re.compile(r'launcher: .* picked|Android started|==== launch'), BLD + CYA),
    (re.compile(r'missing import called'), MAG),
    (re.compile(r'NOT FOUND'), CYA),
    (re.compile(r'^files: .*took|slow file call|write-behind: .*took|failed write|not moved'), YEL),
    (re.compile(r'first frame|to the fast disk|big file\(s\)|cores: .*thread|live log'), GRN),
]
FPS = re.compile(r'^(unity|godot|launcher): ([\d.]+) fps')


def colour(line):
    m = FPS.match(line)
    if m:
        f = float(m.group(2))
        return GRN if f >= 55 else YEL if f >= 30 else RED
    for rx, c in RULES:
        if rx.search(line):
            return c
    return None


def main():
    os.makedirs(LOGS, exist_ok=True)
    path = os.path.join(LOGS, time.strftime('live-%Y%m%d-%H%M%S.log'))
    out = open(path, 'a', encoding='utf-8', errors='replace')
    print(f'{DIM}live log from {HOST}:{PORT} -> {path} (Ctrl+C to stop){RST}', flush=True)
    waiting = False
    while True:
        try:
            s = socket.create_connection((HOST, PORT), timeout=3)
        except OSError:
            if not waiting:
                print(f'{DIM}waiting for Hydroid on {HOST}:{PORT} (start the app) ...{RST}', flush=True)
                waiting = True
            time.sleep(2)
            continue
        waiting = False
        s.settimeout(None)
        print(f'{GRN}connected{RST}', flush=True)
        buf = b''
        try:
            while True:
                data = s.recv(65536)
                if not data:
                    break
                buf += data
                *lines, buf = buf.split(b'\n')
                for raw in lines:
                    line = raw.decode('utf-8', 'replace').rstrip('\r')
                    out.write(line + '\n')
                    if GREP is not None:
                        if GREP in line:
                            print(line, flush=True)
                        continue
                    c = colour(line)
                    if c:
                        print(f'{c}{line[:220]}{RST}', flush=True)
                    elif ALL:
                        print(f'{DIM}{line[:220]}{RST}', flush=True)
                out.flush()
        except OSError:
            pass
        finally:
            s.close()
        print(f'{DIM}disconnected (the app closed?){RST}', flush=True)


if __name__ == '__main__':
    try:
        main()
    except KeyboardInterrupt:
        pass
