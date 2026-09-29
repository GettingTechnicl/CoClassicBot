#!/usr/bin/env python3
"""
passthrough_wrapper.py - runs UNDER pythonw.exe (no console subsystem, so no window ever
flashes) as the Scheduled Task's actual process. Wrapping this way -- rather than a
`cmd /c pythonw ... > log.txt` action -- means the task's own process IS the pass-through
itself, so `Stop-ScheduledTask` kills the real listening process directly instead of
killing a cmd.exe wrapper and orphaning the pass-through still bound to the port.

pythonw has no console to print to, so this redirects stdout/stderr to a log file before
running the real script in place (same process, same globals -- not a subprocess).

Usage: pythonw.exe passthrough_wrapper.py [script_path] [log_path]
Both arguments are optional; defaults match this project's standing paths.
"""
import sys, runpy, os

DEFAULT_SCRIPT = r"C:\Users\TerryGluff\Documents\Claude\CO99\scratchpad\local_socks5_passthrough.py"
DEFAULT_LOG = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "monitor", "passthrough.log")

script = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_SCRIPT
log_path = sys.argv[2] if len(sys.argv) > 2 else os.path.abspath(DEFAULT_LOG)

os.makedirs(os.path.dirname(log_path), exist_ok=True)
log = open(log_path, "a", buffering=1, encoding="utf-8")
sys.stdout = log
sys.stderr = log
print(f"--- passthrough_wrapper: starting pid={os.getpid()} script={script} ---", flush=True)
try:
    runpy.run_path(script, run_name="__main__")
except Exception as e:
    print(f"--- passthrough_wrapper: CRASHED: {e!r} ---", flush=True)
    raise
finally:
    print(f"--- passthrough_wrapper: exiting pid={os.getpid()} ---", flush=True)
