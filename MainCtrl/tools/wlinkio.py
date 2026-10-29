"""Safe V203 RAM read/write via WCH-Link (shared by v203ctl.py and the various dump scripts).

Rules (established after two incidents on 2026-10-21):
- wlink dump / write-reg halts the CPU and does not resume it automatically. Old scripts only resumed after reading everything, so an exception midway left the V203
  halted; IWDG_STOP was also set at that point (watchdog stops while halted), so the watchdog could not rescue it either: the screen froze and all protection was disabled.
- Therefore: resume immediately after every dump / write-mem; on failure, resume first and retry once; when the process ends (including exceptions)
  resume once more. IWDG_STOP is not cleared at exit (clearing it itself causes a watchdog reset, see _cleanup).
- The watchdog is only 500ms and the halt from a single wlink call may exceed that, so IWDG_STOP is set before the first operation.
  DBGMCU_CR (CSR 0x7C0) can only be written in debug mode and is lost after reset.
"""
import atexit
import os
import re
import subprocess

WLINK = os.path.join(os.path.expandvars(r"%USERPROFILE%\.platformio\packages"), "tool-wlink", "wlink.exe")
DBGMCU_CR, IWDG_STOP = "0x7c0", "0x100"


class WlinkError(RuntimeError):
    pass


def run(*args):
    """Run wlink once, return the output with color codes stripped (errors not checked)."""
    out = subprocess.run([WLINK, *args], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                         text=True, errors="replace").stdout
    return re.sub(r"\x1b\[[0-9;]*m", "", out)


def _ok(out):
    return "Error" not in out


_frozen = False


def resume():
    for _ in range(3):
        if _ok(run("resume")):
            return True
    return False


def _cleanup():
    # Do not clear IWDG_STOP here: write-reg halts the CPU, and after clearing, the watchdog resumes counting immediately; if more than 500ms passes
    # before the next resume it resets (actually happened, 2026-10-22). Every operation already resumes afterwards; IWDG_STOP
    # is left to expire naturally at the next reset
    if not resume():
        print("!! wlink resume failed: V203 may still be halted, check the screen; if necessary replug the WCH-LinkE or power-cycle the whole board")


atexit.register(_cleanup)


def freeze_iwdg():
    global _frozen
    if _frozen:
        return
    out = run("write-reg", DBGMCU_CR, IWDG_STOP)
    resume()
    if not _ok(out):
        raise WlinkError("write-reg DBGMCU_CR: " + out.strip().splitlines()[-1])
    _frozen = True


def _parse(out, n):
    b = bytearray()
    for m in re.finditer(r"^.*?([0-9a-f]{8}):\s+((?:[0-9a-f]{2}\s+)+)", out, re.M):
        b += bytes(int(x, 16) for x in m.group(2).split()[:16])
    return bytes(b[:n])


def dump(addr, n):
    """Read n bytes; resume immediately after reading. On a short read or error retry once; if it still fails raise WlinkError."""
    freeze_iwdg()
    for attempt in range(2):
        out = run("dump", hex(addr), str(n))
        resume()
        b = _parse(out, n)
        if _ok(out) and len(b) == n:
            return b
    raise WlinkError(f"dump 0x{addr:08X} {n}: " + (out.strip().splitlines() or ["no output"])[-1])


def write_mem(addr, value):
    """Write one 32-bit word; resume immediately after writing."""
    freeze_iwdg()
    out = run("write-mem", hex(addr), str(value))
    resume()
    if not _ok(out):
        raise WlinkError(f"write-mem 0x{addr:08X}: " + out.strip().splitlines()[-1])


def uptime_s():
    """Uptime converted from the SysTick count (144MHz), can be used to check whether readings are plausible."""
    return int.from_bytes(dump(0xE000F008, 8), "little") / 144e6
