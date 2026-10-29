"""Remotely operate the V203 via WCH-Link (firmware v0.0.79+, debug command mailbox in src/app/dbgcmd.c).

Usage (from the MainCtrl directory):
  python tools/v203ctl.py on 1          turn on CH1 (1 / 2)
  python tools/v203ctl.py off 2         turn off CH2
  python tools/v203ctl.py clear 1       clear the CH1 latched fault
  python tools/v203ctl.py alloff        turn off both channels
  python tools/v203ctl.py replug 2      CH2 software replug (X035 2.2+)
  python tools/v203ctl.py share off|normal|manual|follow
  python tools/v203ctl.py learn start|stop|save|status   MANUAL feed-forward learning (v0.0.86+, while current sharing is running)
  python tools/v203ctl.py volt 48000 [1|2|both]   switch the target voltage (default: both channels)
  python tools/v203ctl.py status        on/off / latch / last shutdown reason for both channels
  python tools/v203ctl.py reset         V203 software reset (v0.0.84+, for verifying warm-reset recovery)

The mailbox address is taken from the dbg_cmd symbol in .pio/build/MainCtrl_RELEASE/firmware.elf; it must match the firmware on the board.
Note: every WCH-Link connection changes the V203 PLL to x15; the firmware fixes it within about 10ms, during which the X035 UART may show a little garbage.
WCH-Link reads/writes go through tools/wlinkio.py: resume immediately after each operation, and on exit (including exceptions) clear IWDG_STOP and resume,
to avoid the V203 hanging in the halted state (actually happened, 2026-10-21).
"""
import os
import re
import struct
import subprocess
import sys
import time

from wlinkio import WlinkError, dump, uptime_s, write_mem

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ELF = os.path.join(ROOT, ".pio", "build", "MainCtrl_RELEASE", "firmware.elf")
PIO_PKG = os.path.expandvars(r"%USERPROFILE%\.platformio\packages")
NM = os.path.join(PIO_PKG, "toolchain-riscv", "bin", "riscv-wch-elf-nm.exe")

MAGIC, SIG = 0x444D4344, 0xC0DE0000
CMD = {"path": 1, "clear": 2, "alloff": 3, "replug": 4, "share": 5, "volt": 6, "reset": 7, "learn": 8}
FIT = ["OK", "NOT LEARNING", "TOO FEW BINS (need 3)", "CURRENT SPAN < 2A", "SOURCE CAPS UNKNOWN"]
RESET_KEY = 0x21545352      # "RST!"
DENY = ["OK", "PD LINK OFFLINE", "HW REV UNKNOWN", "FAULT LATCHED", "NO I-SENSE", "VIN OUT OF RANGE",
        "VIN != PD CONTRACT", "VOLTAGE SWITCHING", "OTHER CH NO POWER", "X035 FW < 2.2"]


def sym(pattern, ram=False, size=False):
    """Find an address by symbol name (regex); ram=True only takes those in RAM (LTO renames same-name static variables to name.lto_priv.N).
    With size=True returns (address, size)."""
    out = subprocess.run([NM, "-S", ELF], stdout=subprocess.PIPE, text=True).stdout
    for m in re.finditer(rf"^([0-9a-f]+) ([0-9a-f]+) \w ({pattern})$", out, re.M):
        a = int(m.group(1), 16)
        if not ram or a >= 0x20000000:
            return (a, int(m.group(2), 16)) if size else a
    raise SystemExit(f"symbol {pattern} not found in {ELF}")


def send(cmd, arg):
    base = sym("dbg_cmd")
    magic, _, _, ack, _ = struct.unpack("<5I", dump(base, 20))
    if magic != MAGIC:
        raise SystemExit(f"dbg_cmd magic 0x{magic:08X} != DCMD: firmware on the board is not this ELF (or older than v0.0.79)")
    seq = ((ack >> 8) & 0xFF) % 255 + 1
    write_mem(base + 4, arg)
    write_mem(base + 8, SIG | (seq << 8) | cmd)
    for _ in range(20):
        time.sleep(0.1)
        _, _, req, ack, count = struct.unpack("<5I", dump(base, 20))
        if (ack >> 8) & 0xFF == seq and req == 0:
            return ack & 0xFF
    raise SystemExit("no ack within 2s (main loop not running?)")


def reset():
    """Software reset: dbg_cmd is re-initialized after reset, so do not wait for ack; confirm by uptime instead."""
    base = sym("dbg_cmd")
    magic, _, _, ack, _ = struct.unpack("<5I", dump(base, 20))
    if magic != MAGIC:
        raise SystemExit(f"dbg_cmd magic 0x{magic:08X} != DCMD: firmware on the board is not this ELF")
    before = uptime_s()
    seq = ((ack >> 8) & 0xFF) % 255 + 1
    write_mem(base + 4, RESET_KEY)
    write_mem(base + 8, SIG | (seq << 8) | CMD["reset"])
    time.sleep(2.0)
    after = uptime_s()
    print(f"reset: uptime {before:.1f}s -> {after:.1f}s", "OK" if after < before and after < 5 else "NOT RESET?")


def learn_status():
    base, size = sym("share_learn", size=True)
    d = dump(base, size)
    magic, active, result, bins, valid, a, k, resid, v0 = struct.unpack_from("<IBBBBhhHH", d, 0)
    if magic != 0x314E524C:
        raise SystemExit(f"share_learn magic 0x{magic:08X} != LRN1")
    res = "-" if result == 0xFF else FIT[result] if result < len(FIT) else result
    print(f"learning={'ON' if active else 'off'} last save={res}")
    print(f"profile: {'valid' if valid else 'none'}  dOff(CH1-CH2) = {a} mV + {k} mV/A x I_total"
          f"  (bins {bins}, max resid {resid} mV, center {v0} mV)")
    for b in range(16, size, 12):
        sum_ma, sum_mv, n = struct.unpack_from("<iiH", d, b)
        if n:
            print(f"  bin {(b - 16) // 12 * 0.5:4.1f}A  n={n:4d}  I={sum_ma / n / 1000:6.2f}A  dOff={sum_mv / n:+7.0f} mV")


def status():
    (prot, psize), hw = sym(r"prot(\.lto_priv\.\d+)?", ram=True, size=True), sym(r"hw\.lto_priv\.\d+", ram=True)
    stride = psize // 2                     # prot_t[CH_COUNT], size varies by version
    d = dump(prot, psize)
    g = dump(0x40010C00, 16)
    hwv = dump(hw, 1)[0]
    outdr = struct.unpack_from("<I", g, 12)[0]
    print(f"board: {['UNKNOWN', 'LM74700', 'LTC4359'][hwv] if hwv < 3 else hwv}")
    for ch, pin in ((0, 9), (1, 8)):
        p = d[ch * stride:(ch + 1) * stride]
        on = ((outdr >> pin) & 1) == (1 if hwv == 2 else 0)
        print(f"CH{ch + 1}: path {'ON ' if on else 'OFF'} want_on={p[0]} latched={p[1]} trips={p[48]}")


def main(a):
    if not a or a[0] in ("-h", "--help"):
        print(__doc__)
        return
    c = a[0]
    if c == "status":
        status()
        return
    if c == "reset":
        reset()
        return
    if c in ("on", "off"):
        r = send(CMD["path"], (int(a[1]) - 1) | (0x100 if c == "on" else 0))
    elif c in ("clear", "replug"):
        r = send(CMD[c], int(a[1]) - 1)
    elif c == "alloff":
        r = send(CMD["alloff"], 0)
    elif c == "share":
        r = send(CMD["share"], ["off", "normal", "manual", "follow"].index(a[1]))
        print("share:", "OK" if r == 0 else "waiting for preconditions" if r == 1 else f"result {r}")
        return
    elif c == "learn":
        if a[1] == "status":
            learn_status()
            return
        r = send(CMD["learn"], ["stop", "start", "save"].index(a[1]))
        print("learn", a[1], "->", FIT[r] if a[1] == "save" and r < len(FIT) else "OK" if r == 0 else f"result {r}")
        return
    elif c == "volt":
        mask = {"1": 1, "2": 2, "both": 3}[a[2] if len(a) > 2 else "both"]
        r = send(CMD["volt"], (mask << 16) | int(a[1]))
        print(f"volt: accepted channel mask {r}")
        return
    else:
        raise SystemExit(f"unknown command {c}")
    print(c, a[1] if len(a) > 1 else "", "->", DENY[r] if r < len(DENY) else f"result {r}")


if __name__ == "__main__":
    try:
        main(sys.argv[1:])
    except WlinkError as e:
        raise SystemExit(f"wlink: {e}")
