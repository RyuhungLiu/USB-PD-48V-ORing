import mosfet_step_sim as M, sys, json
DV0 = 0.35
rows = []
cases = [("2k bleeder", True, 2.0, 10e-6, 8e-3), ("2k bleeder", True, 4.0, 10e-6, 8e-3), ("2k bleeder", True, 6.0, 10e-6, 8e-3),
         ("2k bleeder, IGATE 6uA", True, 4.0, 6e-6, 8e-3),
         ("no bleeder", False, 2.0, 10e-6, 20e-3), ("no bleeder", False, 4.0, 10e-6, 20e-3)]
for tag, bl, amp, iup, tmax in cases:
    out = {}
    for fet in ("IPT015N10N5", "IPT059N15N3"):
        r = M.run(fet, amp, bl, DV0 if bl else 0.0, i_up=iup, tmax=tmax)
        out[fet] = r
    a, b = out["IPT015N10N5"], out["IPT059N15N3"]
    rows.append(dict(tag=tag, amp=amp, old=a, new=b))
    f = lambda r: f"dip {r['dip']:5.2f}V @{r['tmin']:5.2f}ms rec {('%.2f' % r['trec']) if r['trec'] else '  -  '}ms  E {r['e']*1e3:7.1f}mJ  Pmax {r['pmax']:6.1f}W"
    print(f"{tag:22s} 0->{amp:.0f}A | old {f(a)} | new {f(b)}", flush=True)
json.dump(rows, open("mosfet_step_cmp.json", "w"), indent=1)
