"""Print free physical RAM (MB) and the number of python/kinoko_host processes; exit 1 if free RAM is below --min-mb.

    python tools/mem.py --min-mb 4000     # use before launching something heavy
"""
import argparse
import ctypes
import subprocess
import sys


class MemStatus(ctypes.Structure):
    _fields_ = [("dwLength", ctypes.c_ulong), ("dwMemoryLoad", ctypes.c_ulong), ("ullTotalPhys", ctypes.c_ulonglong),
                ("ullAvailPhys", ctypes.c_ulonglong), ("ullTotalPageFile", ctypes.c_ulonglong),
                ("ullAvailPageFile", ctypes.c_ulonglong), ("ullTotalVirtual", ctypes.c_ulonglong),
                ("ullAvailVirtual", ctypes.c_ulonglong), ("sullAvailExtendedVirtual", ctypes.c_ulonglong)]


def free_mb():
    st = MemStatus()
    st.dwLength = ctypes.sizeof(MemStatus)
    ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(st))
    return int(st.ullAvailPhys / 2**20), int(st.ullTotalPhys / 2**20)


def count_procs():
    out = subprocess.run(["tasklist", "/FO", "CSV", "/NH"], capture_output=True, text=True).stdout.lower()
    return sum(out.count(name) for name in ('"python.exe"', '"kinoko_host.exe"'))


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--min-mb", type=int, default=0)
    a = ap.parse_args()
    free, total = free_mb()
    print(f"free {free} MB of {total} MB   python+kinoko_host processes: {count_procs()}")
    sys.exit(1 if free < a.min_mb else 0)
