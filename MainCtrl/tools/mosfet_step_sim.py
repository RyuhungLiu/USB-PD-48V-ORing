"""LTC4359 + back-to-back MOS load step model (single channel, sudden load increase from no load / after the bleeder resistor).

Model highlights
- LTC4359: when IN-OUT > 30mV the gate is charged with IGATE(UP) (typ. 10µA); when < 30mV it pulls down with limited strength (regulation).
- The two MOSFETs share the gate: gate capacitance = 2*Cgs(Vgs) + Cgd(VIN side, vs fixed VIN) + Cgd(VOUT side).
  Cgs is piecewise from the datasheet Qg(th)/Qgs/Qg (0->Vth, Vth->Vplateau, Vplateau->10V).
  Cgd(V) = C0/sqrt(1+V/1V), C0 back-calculated by integrating Qgd (at the datasheet test voltage).
- Channel current: VIN-side MOS square law Id = K*(Vgs-Vth)^2 (K back-calculated from gfs@100A), linear region limited by RDS(on);
  VOUT-side MOS conducts through the body diode while its gate is off (0.7V + 2mΩ), RDS(on) once on.
- Source-follower Miller coupling: when the Vds of the VIN-side MOS rises, Cgd pushes the gate (relative to source) upward.
- Output: C = 435µF (back-calculated from the measured slope), load current ramps at 0.01A/µs, optional 2kΩ bleeder resistor.
- Charger: VIN = 47.85V, internal resistance 0.09Ω (measured input sag of about 184mV at 2A).
"""
import math, sys

FETS = {
    "IPT015N10N5": dict(vth=3.0, qth=36e-9, qgs=53e-9, vpl=4.4, qg=169e-9, qgd=34e-9, vtest=50.0, gfs=280.0, rds=1.3e-3),
    "IPT059N15N3": dict(vth=3.0, qth=16e-9, qgs=29e-9, vpl=5.4, qg=69e-9, qgd=11e-9, vtest=75.0, gfs=172.0, rds=5.1e-3),
}

def fet_model(p, n=2):
    # Cgs segments (per device): 0..Vth, Vth..Vpl, Vpl..10V; n devices share the gate
    c1 = p["qth"] / p["vth"]
    c2 = (p["qgs"] - p["qth"]) / (p["vpl"] - p["vth"])
    c3 = (p["qg"] - p["qgs"] - p["qgd"]) / (10.0 - p["vpl"])     # after the plateau (Miller charge removed)
    def cgs(v): return n * (c1 if v < p["vth"] else c2 if v < p["vpl"] else c3)
    c0 = p["qgd"] / (2.0 * (math.sqrt(1.0 + p["vtest"]) - 1.0))
    def cgd(vds): return c0 / math.sqrt(1.0 + max(vds, 0.0))
    K = p["gfs"] ** 2 / (4 * 100.0)
    return cgs, cgd, K

def path_current(p, K, vgs, vtot):
    """Current at total path drop vtot (VIN-VOUT): VIN-side channel (saturation/linear) + VOUT side (channel or body diode, whichever is smaller)."""
    if vtot <= 0: return 0.0, 0.0, 0.0
    vov = vgs - p["vth"]
    def i_in(vds):
        if vov <= 0: return 0.0
        isat = K * vov * vov
        if vds >= vov: return isat
        return min(isat, vds / (1.0 / (2 * K * vov) + p["rds"]))
    def v_out(i):
        if i <= 0: return 0.0
        vd = 0.62 + 0.025 * math.log1p(i / 0.01) + i * 2e-3
        if vov > 0: return min(i * (1.0 / (2 * K * vov) + p["rds"]), vd)
        return vd
    lo, hi = 0.0, vtot
    for _ in range(40):
        mid = (lo + hi) / 2
        if mid + v_out(i_in(mid)) > vtot: hi = mid
        else: lo = mid
    i = i_in(lo)
    return i, lo, vtot - lo

def run(fet, i_step, bleeder=True, dv0=0.0, rbl=2000.0, i_up=10e-6, cout=435e-6, rs=0.09, vin0=47.85, tmax=20e-3, dt=0.5e-6, slew=0.01e6):
    p = FETS[fet]; cgs, cgd, K = fet_model(p)
    rb = rbl if bleeder else 1e12
    # Initial steady state: bleeder current only (or 0), gate parked just where it conducts that current
    i0 = vin0 / rb
    if i0 > 1e-6:
        vgs = p["vth"]
        while True:
            i, vin_fet, vout_fet = path_current(p, K, vgs, 0.030)
            if i >= i0 or vgs > 9: break
            vgs += 0.0005
        vgs -= dv0                                  # correction: actual subthreshold conduction parks the gate lower than the square-law model
    else:
        vgs = 0.0                                   # long no-load period: gate regulated all the way down
    vout = vin0 - 0.030 if i0 > 1e-6 else vin0 - 0.030
    vds_in_prev = 0.015; t = 0.0
    vmin, tmin, e_fet, pmax = vout, 0.0, 0.0, 0.0
    t_rec = None; tr = []
    while t < tmax:
        il = min(i_step, slew * t)
        iload = il + vout / rb
        # charger terminal voltage
        # solve i = path(vin0 - rs*i - vout): bisection (larger i -> lower vin, smaller path current)
        lo_i, hi_i = 0.0, max(0.0, (vin0 - vout) / rs)
        for _ in range(40):
            mid = (lo_i + hi_i) / 2
            if path_current(p, K, vgs, vin0 - rs * mid - vout)[0] > mid: lo_i = mid
            else: hi_i = mid
        vin = vin0 - rs * lo_i
        ipath, vds_in, vds_out = path_current(p, K, vgs, vin - vout)
        # gate: charge when IN-OUT > 30mV, otherwise pull down at 10x strength back to regulation
        err = (vin - vout) - 0.030
        ig = i_up if err > 0.005 else (i_up * err / 0.005 if err > -0.005 else -10 * i_up)
        dvds = vds_in - vds_in_prev; vds_in_prev = vds_in
        c_in = cgd(vds_in); c_out = cgd(vout - (vin - vds_in))
        dvgs = (ig * dt + c_in * dvds) / (cgs(vgs) + c_in + c_out)
        vgs = max(0.0, min(12.0, vgs + dvgs))
        vout += (ipath - iload) / cout * dt
        pw = ipath * (vin - vout); e_fet += pw * dt; pmax = max(pmax, pw)
        if vout < vmin: vmin, tmin = vout, t
        if t_rec is None and t > tmin + 50e-6 and t > 0.2e-3 and vout > vin - 0.5: t_rec = t
        t += dt
    return dict(vmin=vmin, dip=vin0 - 0.030 - vmin, tmin=tmin * 1e3, trec=(t_rec * 1e3 if t_rec else None), e=e_fet, pmax=pmax)

if __name__ == "__main__":
    print("calibration (IPT015N10N5): no bleeder 0->2A, VOUT at 5ms")
    r = run("IPT015N10N5", 2.0, bleeder=False, tmax=5e-3)
    print(f"  model {r['vmin']:.1f} V  vs measured 24.9 V")
    print("fit dv0 with 2k bleeder (measured: 2A dip 3.0V@0.69ms rec~2.3ms; 4A dip 3.0V@0.72ms rec~2.5ms)")
    for dv0 in (0.0, 0.1, 0.2, 0.3, 0.4, 0.5):
        a = run("IPT015N10N5", 2.0, True, dv0, tmax=8e-3); b = run("IPT015N10N5", 4.0, True, dv0, tmax=8e-3)
        print(f"  dv0={dv0:.1f}V  2A: dip {a['dip']:.2f}V @{a['tmin']:.2f}ms rec {a['trec']}  |  4A: dip {b['dip']:.2f}V @{b['tmin']:.2f}ms rec {b['trec']}", flush=True)
