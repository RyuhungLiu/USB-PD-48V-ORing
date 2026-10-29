# -*- coding: utf-8 -*-
"""
48V10+10A ORing slanted instrument enclosure generator (FreeCAD 1.1)

  "C:\\Program Files\\FreeCAD 1.1\\bin\\freecadcmd.exe" gen_enclosure.py   generates FCStd / STEP / STL and check images
  "C:\\Program Files\\FreeCAD 1.1\\bin\\freecad.exe"    gen_enclosure.py   same as above, plus GUI coloring and an assembly view

Main board outline, M3 holes, and positions of USB-C / XT60 / capacitors and other parts are read directly from the EasyEDA Pro project (EPRO_FILE / PCB_TITLE).
The GMT020-02-8P screen and TTP223 values are from specs and photo measurements; values marked [measured] should be confirmed with calipers, then edit and rerun.

Printing (FDM, 0.2 mm layer height, 0.4 mm nozzle): the STLs under out/print/ are already rotated to print orientation, no supports needed
  1_cover_panel  slanted cover: screen face down on the bed
  2_cover_shelf  shelf cover: shelf top face down on the bed
  3_base         base plate: bottom face down
Each of the two top covers is fastened by 2 M3 self-tapping screws from under the base plate, clamping the main board at the same time.

Coordinates: X along the long edge of the main board (left = USB-C, right = XT60), Y from front to back, Z up, origin at the front-left-bottom corner of the enclosure.
Slanted-face local coordinates (a, s, h): a = X, s up along the slanted face, h along the slanted-face normal (outward positive), h = 0 is the inner surface of the slanted face.
"""
import json
import math
import os
import sys

import FreeCAD as App
import Part
from FreeCAD import Vector as V

# ======================= Source =======================
EPRO_FILE = 'ProPrj_48V10+10A ORing_2026-10-10_en.epro2'
PCB_TITLE = 'PCB1_2'        # REV2; REV1 = PCB1_1 (same outline, hole positions, connector and capacitor positions)

# ======================= Enclosure =======================
TILT_DEG   = 20.0           # angle between slanted face and horizontal (at 20° the total height with the shelf flush is 40.4; 44.9 at 25°)
WALL       = 2.4            # side wall and slanted face thickness
BASE_T     = 3.0            # base plate thickness
CORNER_R   = 5.0            # enclosure vertical corner radius
EDGE_CHAMFER = 0.5          # 45° chamfer on the slanted-face outer edge and shelf top edge = elephant-foot compensation on the bed-side edges (a fillet would overhang at the bed)
BASE_CHAMFER = 0.5          # base plate bottom edge chamfer, offsets first-layer spread (elephant foot)
GAP_L      = 0.4            # main board left edge (USB-C end face) to inner wall
GAP_R      = 1.0            # main board right edge to inner wall
GAP_FB     = 0.6            # main board front / back edge to inner wall
STANDOFF_H = 4.0            # base plate standoff height
PCB_T      = 1.6
CLEAR      = 1.5            # minimum clearance between parts, modules and the enclosure
CABLE_KEEP = 10.0           # height reserved above HDR footprints with soldered pin headers for right-angle plugs and cables
WIRE_KEEP = 4.0             # HDR pads whose pin headers are omitted (listed in OMIT_REFS): height reserved for solder joints and wire bends
OMIT_REFS = ('C12', 'C14', 'H2', 'H4')  # not populated: input 100µF electrolytics (Ø10x16); H2/H4 have no pin headers, wires soldered directly to the pads
BUMP_REFS = ('C21', 'C22')  # output 220µF (Ø13x21): covered by one flat-top shelf instead, so the slanted-face height does not accommodate them
SHELF_FLUSH = True         # shelf top flush with the highest point of the slanted face (back edge), top edges form one line
SHELF_FRONT_CHAMFER = 5.0  # large 45° chamfer on the shelf front top edge, reduces the visual height of the front (exactly 45° when printed upside down)
LIP_H, LIP_T, LIP_CLR = 2.0, 1.6, 0.25      # base plate locating lip

# M3: screws pass from the bottom through the base plate standoffs and main board holes into the top cover bosses (clamping the main board); recommended length in build_log
STANDOFF_D = 6.4                # back holes are only 2.87 from the board edge; any larger would cut into the back wall
SCREW_CLR_D = 3.4
SCREW_HEAD_D, SCREW_HEAD_H = 6.0, 1.8  # counterbore straight section; topped by a 45° cone down to SCREW_CLR_D (avoids bridging overhang)
SCREW_SEAT_Z = 2.1                      # actual seating height of the M3 button head (Ø5.5)
BOSS_D = 5.6                # limited by C21/C22 and the board edge
PILOT_D, PILOT_DEPTH = 2.9, 12.0     # M3 self-tapping pilot hole
RIB_W, RIB_LIFT = 3.0, 2.5  # reinforcing ribs from bosses to front/back walls, bottom RIB_LIFT above the board surface

# USB-C TYPE-C-31-M-12A (board-mount, shell 8.94 x 3.26)
USB_BODY_H = 3.26
USB_HOLE_W, USB_HOLE_H, USB_HOLE_R = 9.6, 4.0, 1.6
USB_RECESS_W, USB_RECESS_H, USB_RECESS_R, USB_RECESS_D = 12.8, 7.2, 2.5, 1.6  # outer relief for the plug overmold

# XT60PW-F (horizontal)
XT60_H = 8.3                # [measured] housing height
XT60_CLR = 0.4

# Screen GMT020-02-8P, portrait coordinates (px to the right, py down), pin header on the top edge
LCD_W, LCD_L, LCD_T = 37.1, 62.0, 6.0
LCD_HOLES = [(4.4, 1.8), (32.7, 1.8), (4.4, 59.95), (32.7, 59.95)]   # [measured] hole diameter about 2.5
LCD_GLASS = (1.85, 5.25, 35.25, 57.5)    # [measured] glass including FPC fold px0, py0, px1, py1
LCD_AA = (18.55, 28.55, 30.6, 40.8)      # active-area center px, py and size; py corrected from two real photos (originally estimated 30.25)
LCD_STACK = 3.2             # [measured] module PCB top surface to glass surface = screen mounting boss length
LCD_PCB_T = 1.6
LCD_WIRE_KEEP = 3.0         # depth reserved for wires soldered directly to the back of the module pin-header end
LCD_BOSS_D, LCD_PILOT_D = 4.2, 2.0       # M2 self-tapping pilot hole
LCD_A0 = 6.0                # module left edge (pin-header side) X
LCD_S0 = None               # position along the slanted face; None = centered between the front and back bosses
WIN_W, WIN_H = 42.0, 33.0   # panel window (landscape): active area 40.8 x 30.6 plus margin
WIN_DA = 0.0                # extra offset of the window center relative to the active-area center
WIN_CHAMFER = 1.0

# TTP223 small board 14.2 x 10.9, sensing copper on the back against the inside of the slanted face
TOUCH_W, TOUCH_H, TOUCH_PCB_T = 14.2, 10.9, 1.6
TOUCH_A = None              # touch center X; None = shelf center (without a shelf: between the screen right edge and the right inner wall)
TOUCH_Y = None              # touch center Y on the shelf; None = center of the gap between the two shelf capacitors
TOUCH_SKIN = 1.0            # remaining panel thickness in the touch area
TOUCH_CLR = 0.2
TOUCH_MARK_D, TOUCH_MARK_DEPTH = 11.0, 0.4   # touch position mark: outer diameter, depth (integer multiple of the 0.2 mm layer height)
TOUCH_MARK_W = 0.6                           # mark made as a thin ring groove, so bed-side printing only needs a 0.6 mm small bridge
TOUCH_KEEP = 5.0            # inward depth reserved for the component side + solder wires

VENTS = True

# Colors (matching real photos: purple glitter top cover, green base plate), only affects GUI display and screenshots
COLOR_COVER = (0.42, 0.30, 0.86)
COLOR_BASE = (0.42, 0.80, 0.28)

# ====================================================
COS, SIN, TAN = (f(math.radians(TILT_DEG)) for f in (math.cos, math.sin, math.tan))


def _here():
    try:
        return os.path.dirname(os.path.abspath(__file__))
    except NameError:
        return os.environ.get('ENCL_DIR', os.getcwd())


HERE = _here()
PROJ = os.path.dirname(HERE)
OUT = os.path.join(HERE, 'out')
_LOG = []


def log(*args):
    """The FreeCAD console uses the system code page, so non-ASCII text is also saved as UTF-8 to out/build_log.txt."""
    msg = ' '.join(str(a) for a in args)
    _LOG.append(msg)
    try:
        print(msg)
    except UnicodeEncodeError:
        print(msg.encode('ascii', 'replace').decode('ascii'))


# ---------------- EasyEDA Pro parsing (shared module epro_pcb.py, also used by ATX_PSU) ----------------
sys.path.insert(0, HERE)
import epro_pcb  # noqa: E402


def load_pcb():
    return epro_pcb.load_pcb(os.path.join(PROJ, EPRO_FILE), PCB_TITLE, {'XT60': XT60_H, 'USB-C': USB_BODY_H})


# ---------------- Geometry tools ----------------
def box(x0, y0, z0, x1, y1, z1):
    return Part.makeBox(x1 - x0, y1 - y0, z1 - z0, V(x0, y0, z0))


def edges_along(shape, axis):
    out = []
    for e in shape.Edges:
        if len(e.Vertexes) == 2:
            d = e.Vertexes[1].Point - e.Vertexes[0].Point
            if d.Length > 1e-6 and abs(d.dot(axis)) / d.Length > 0.999:
                out.append(e)
    return out


def fillet(shape, r, edges, what):
    if r <= 0 or not edges:
        return shape
    try:
        return shape.makeFillet(r, edges)
    except Exception as exc:          # noqa: BLE001 - fillet failure does not affect function
        log('  ! fillet failed (%s): %s' % (what, exc))
        return shape


def chamfer(shape, d, edges, what):
    if d <= 0 or not edges:
        return shape
    try:
        return shape.makeChamfer(d, edges)
    except Exception as exc:          # noqa: BLE001 - chamfer failure does not affect function
        log('  ! chamfer failed (%s): %s' % (what, exc))
        return shape


def rbox(x0, y0, z0, x1, y1, z1, r, axis=V(0, 0, 1)):
    b = box(x0, y0, z0, x1, y1, z1)
    return fillet(b, r, edges_along(b, axis), 'rbox')


def wedge(x0, x1, y0, y1, z_bot, zt0, zt1, r):
    """YZ profile is a trapezoid extruded along X; top face slopes from (y0, zt0) to (y1, zt1), vertical edges filleted with r."""
    prof = Part.makePolygon([V(x0, y0, z_bot), V(x0, y1, z_bot), V(x0, y1, zt1), V(x0, y0, zt0), V(x0, y0, z_bot)])
    s = Part.Face(prof).extrude(V(x1 - x0, 0, 0))
    return fillet(s, r, edges_along(s, V(0, 0, 1)), 'wedge')


def fuse_all(shapes):
    shapes = [s for s in shapes if s is not None and not s.isNull()]
    if not shapes:
        return None
    out = shapes[0].fuse(shapes[1:]) if len(shapes) > 1 else shapes[0]
    try:
        return out.removeSplitter()
    except Exception:                 # noqa: BLE001
        return out


class Panel:
    """Slanted-face local coordinates (a, s, h) -> global."""

    def __init__(self, z0):
        self.z0 = z0
        self.pl = App.Placement(V(0, 0, z0), App.Rotation(V(1, 0, 0), TILT_DEG))

    def put(self, shape):
        s = shape.copy()
        s.Placement = self.pl.multiply(s.Placement)
        return s

    def box(self, a0, a1, s0, s1, h0, h1):
        return self.put(box(a0, s0, h0, a1, s1, h1))

    def cyl(self, a, s, r, h0, h1):
        return self.put(Part.makeCylinder(r, h1 - h0, V(a, s, h0)))

    def pt(self, a, s, h):
        return V(a, s * COS - h * SIN, self.z0 + s * SIN + h * COS)


def local_samples(a0, a1, s0, s1, h0, h1, step=0.5):
    """Sample points on the six faces of a local box -> (x, y, g), global z = z0 + g."""
    import numpy as np
    na, ns, nh = (max(2, int(abs(b - a) / step) + 1) for a, b in ((a0, a1), (s0, s1), (h0, h1)))
    A, Sg, Hg = np.linspace(a0, a1, na), np.linspace(s0, s1, ns), np.linspace(h0, h1, nh)
    pts = []
    for hv in (h0, h1):
        aa, ss = np.meshgrid(A, Sg)
        pts.append((aa.ravel(), ss.ravel(), np.full(aa.size, hv)))
    for sv in (s0, s1):
        aa, hh = np.meshgrid(A, Hg)
        pts.append((aa.ravel(), np.full(aa.size, sv), hh.ravel()))
    a = np.concatenate([p[0] for p in pts])
    s = np.concatenate([p[1] for p in pts])
    h = np.concatenate([p[2] for p in pts])
    return a, s * COS - h * SIN, s * SIN + h * COS


# ---------------- Main flow ----------------
def build():
    import numpy as np
    bd = load_pcb()
    bx, by = WALL + GAP_L, WALL + GAP_FB                 # position of the main board front-left corner in the enclosure
    W = bx + bd['w'] + GAP_R + WALL
    D = by + bd['h'] + GAP_FB + WALL
    z_pcb0 = BASE_T + STANDOFF_H
    z_pcb1 = z_pcb0 + PCB_T

    all_parts = []
    for p in bd['parts']:
        q = dict(p)
        q.update(x1=p['x1'] + bx, x2=p['x2'] + bx, y1=p['y1'] + by, y2=p['y2'] + by, top=z_pcb1 + p['h'])
        all_parts.append(q)
    parts = [p for p in all_parts if p['ref'] not in OMIT_REFS]
    holes = [(x + bx, y + by) for x, y, _ in bd['holes']]
    usb = sorted([p for p in parts if 'USB-C' in p['fp']], key=lambda p: p['y1'])
    xt60 = next(p for p in parts if 'XT60' in p['fp'])

    # Obstacles (part bodies + pin-header cable / solder-wire keep-out), for solving the slanted-face height; BUMP_REFS handled by the right-side shelf
    obst = [(p['ref'], p['x1'], p['x2'], p['y1'], p['y2'], p['top']) for p in parts if p['ref'] not in BUMP_REFS]
    bump_parts = sorted([p for p in parts if p['ref'] in BUMP_REFS], key=lambda p: p['y1'])
    for p in all_parts:
        if p['fp'].upper().startswith('HDR'):
            wired = p['ref'] in OMIT_REFS
            obst.append((p['ref'] + (' solder wires' if wired else ' cables'), p['x1'] - 1.5, p['x2'] + 1.5,
                         p['y1'] - 1.5, p['y2'] + 1.5, z_pcb1 + (WIRE_KEEP if wired else CABLE_KEEP)))

    # Right-side shelf: one flat top covering the BUMP_REFS parts, extended directly to the outer wall if close to it
    shelf = None
    if bump_parts:
        snap = WALL + 8.0
        ix0 = min(p['x1'] for p in bump_parts) - CLEAR
        ix1 = max(p['x2'] for p in bump_parts) + CLEAR
        iy0 = min(p['y1'] for p in bump_parts) - CLEAR
        iy1 = max(p['y2'] for p in bump_parts) + CLEAR
        ix0, iy0 = (WALL if ix0 < snap else ix0), (WALL if iy0 < snap else iy0)
        ix1, iy1 = (W - WALL if ix1 > W - snap else ix1), (D - WALL if iy1 > D - snap else iy1)
        itop = max(p['top'] for p in bump_parts) + CLEAR
        shelf = dict(ix0=ix0, ix1=ix1, iy0=iy0, iy1=iy1, itop=itop, top=itop + WALL,
                     ox0=0 if ix0 == WALL else ix0 - WALL, ox1=W if ix1 == W - WALL else ix1 + WALL,
                     oy0=0 if iy0 == WALL else iy0 - WALL, oy1=D if iy1 == D - WALL else iy1 + WALL)

    # Screen position along the slanted face: centered between the front and back bosses
    front = [h for h in holes if h[1] < D / 2]
    back = [h for h in holes if h[1] >= D / 2]
    s_lo = (max(h[1] for h in front) + BOSS_D / 2) / COS + 1.0
    s_hi = (min(h[1] for h in back) - BOSS_D / 2 - LCD_T * SIN) / COS - 1.0
    s0 = LCD_S0 if LCD_S0 is not None else (s_lo + s_hi - LCD_W) / 2
    if s0 < s_lo - 1e-6 or s0 + LCD_W > s_hi + 1e-6:
        log('  ! screen overlaps bosses: available s = %.1f..%.1f, module needs %.1f' % (s_lo, s_hi, LCD_W))
    a0 = LCD_A0

    def lcd_ap(px, py):                                   # portrait -> slanted-face local (a, s), pin header facing left
        return a0 + py, s0 + px

    win_a, win_s = lcd_ap(LCD_AA[0], LCD_AA[1])
    if shelf:                                             # touch on the shelf top, (ta, ts) are global X, Y
        ta = TOUCH_A if TOUCH_A is not None else (shelf['ox0'] + shelf['ox1']) / 2
        if TOUCH_Y is not None:
            ts = TOUCH_Y
        elif len(bump_parts) >= 2:
            ts = (bump_parts[0]['y2'] + bump_parts[1]['y1']) / 2
        else:
            ts = (shelf['oy0'] + shelf['oy1']) / 2
    else:                                                 # touch on the slanted face, (ta, ts) are slanted-face local (a, s)
        ta = TOUCH_A if TOUCH_A is not None else (a0 + LCD_L + W - WALL) / 2
        ts = win_s

    # Slanted-face inner surface height z0 (at y = 0): keep the slanted face, screen and touch module all CLEAR above the parts
    hdr_s = (LCD_W / 2 - 11.5, LCD_W / 2 + 11.5)
    pa, py_, pg_ = local_samples(WALL, W - WALL, WALL / COS, (D - WALL) / COS, 0, 0)
    if shelf:                                             # no slanted face within the shelf area
        keep = ~((pa >= shelf['ox0']) & (py_ >= shelf['oy0']) & (py_ <= shelf['oy1']))
        pa, py_, pg_ = pa[keep], py_[keep], pg_[keep]
    samples = [
        ('slanted face', (pa, py_, pg_)),
        ('screen', local_samples(a0, a0 + LCD_L, s0, s0 + LCD_W, -LCD_T, 0)),
        ('screen solder wires', local_samples(a0, a0 + 7, s0 + hdr_s[0], s0 + hdr_s[1], -LCD_T - LCD_WIRE_KEEP, -LCD_T)),
    ]
    if not shelf:
        samples.append(('touch', local_samples(ta - TOUCH_W / 2, ta + TOUCH_W / 2, ts - TOUCH_H / 2,
                                               ts + TOUCH_H / 2, -TOUCH_KEEP, 0)))
    z0, why = 0.0, ''
    for name, (x, y, g) in samples:
        for ref, x1, x2, y1, y2, top in obst:
            m = (x >= x1 - CLEAR) & (x <= x2 + CLEAR) & (y >= y1 - CLEAR) & (y <= y2 + CLEAR)
            if m.any():
                req = top + CLEAR - float(np.min(g[m]))
                if req > z0:
                    z0, why = req, '%s above %s' % (name, ref)
    z0 = math.ceil(z0 * 10) / 10
    h_f = z0 + WALL / COS
    h_b = h_f + D * TAN
    pn = Panel(z0)
    if shelf and SHELF_FLUSH and h_b > shelf['top']:
        shelf['top'] = h_b
        shelf['itop'] = h_b - WALL
    log('Enclosure %.1f x %.1f, front height %.1f, back height %.1f, slant %.0f° (determined by %s)' % (W, D, h_f, h_b, TILT_DEG, why))

    # ---------- Outer shape ----------
    outer = wedge(0, W, 0, D, 0, h_f, h_b, CORNER_R)
    topf = [f for f in outer.Faces if f.Surface.__class__.__name__ == 'Plane'
            and f.normalAt(*f.ParameterRange[::2]).dot(V(0, -SIN, COS)) > 0.999]
    if topf:
        # The outer edge is one tangent-continuous loop, so the chamfer propagates all the way round; at the front edge the 25° outward lean of the front wall makes it slightly less than 45°, so only 0.5 mm is used
        outer = chamfer(outer, EDGE_CHAMFER, topf[0].Edges, 'slanted-face outer edge')
    cav = wedge(WALL, W - WALL, WALL, D - WALL, -5, z0 + WALL * TAN, z0 + (D - WALL) * TAN, CORNER_R - WALL)
    base_outer = outer

    # ---------- Right-side shelf: slanted block cut at the shelf inner wall, shelf area only up to the shelf height ----------
    if shelf:
        sh = shelf
        y0c = sh['oy0'] if sh['oy0'] > 0 else -1
        y1c = sh['oy1'] if sh['oy1'] < D else D + 1
        blk = box(sh['ox0'], sh['oy0'], 0, sh['ox1'], sh['oy1'], sh['top'])
        right = [e for e in edges_along(blk, V(0, 0, 1)) if abs(e.Vertexes[0].Point.x - W) < 1e-6]
        blk = fillet(blk, CORNER_R, right, 'shelf outer corner')
        # Inner corner radius < WALL: the fillet stays within the front / back walls of the slanted block, no notch appears in the wall surface
        blk = fillet(blk, WALL - 0.1, [e for e in edges_along(blk, V(0, 0, 1))
                                       if abs(e.Vertexes[0].Point.x - sh['ox0']) < 1e-6], 'shelf inner corner')
        topb = [f for f in blk.Faces if f.Surface.__class__.__name__ == 'Plane'
                and f.normalAt(*f.ParameterRange[::2]).z > 0.999]
        if topb:
            blk = chamfer(blk, EDGE_CHAMFER, topb[0].Edges, 'shelf top edge')
        if SHELF_FRONT_CHAMFER > 0 and sh['oy0'] == 0:   # large front top chamfer: cut away a triangular prism along X
            c = SHELF_FRONT_CHAMFER
            tri = Part.makePolygon([V(sh['ox0'] - 1, -1, sh['top'] - c - 1), V(sh['ox0'] - 1, c + 1, sh['top'] + 1),
                                    V(sh['ox0'] - 1, -1, sh['top'] + 1), V(sh['ox0'] - 1, -1, sh['top'] - c - 1)])
            blk = blk.cut(Part.Face(tri).extrude(V(W - sh['ox0'] + 2, 0, 0)))
        outer = outer.cut(box(sh['ix0'], y0c, -1, W + 1, y1c, h_b + 10)).fuse(blk)
        cav = cav.cut(box(sh['ox0'], y0c, sh['itop'], W + 1, y1c, h_b + 10)).fuse(
            rbox(sh['ix0'], sh['iy0'], -5, sh['ix1'], sh['iy1'], sh['itop'], CORNER_R - WALL))
        if SHELF_FRONT_CHAMFER > 0 and sh['oy0'] == 0:   # inner cavity front top corner inset to match, the chamfer face keeps WALL thickness
            d = SHELF_FRONT_CHAMFER + WALL * math.sqrt(2)
            tri = Part.makePolygon([V(sh['ox0'] - 1, -1, sh['top'] - d - 1), V(sh['ox0'] - 1, d + 1, sh['top'] + 1),
                                    V(sh['ox0'] - 1, -1, sh['top'] + 1), V(sh['ox0'] - 1, -1, sh['top'] - d - 1)])
            cav = cav.cut(Part.Face(tri).extrude(V(W - sh['ox0'] + 2, 0, 0)))
        log('Right-side shelf: X %.1f..%.1f, Y %.1f..%.1f, top height %.1f; slanted face reaches the shelf height at Y≈%.1f'
            % (sh['ox0'], sh['ox1'], sh['oy0'], sh['oy1'], sh['top'], (sh['top'] - h_f) / TAN))
        try:
            outer, cav = outer.removeSplitter(), cav.removeSplitter()
        except Exception:                                 # noqa: BLE001
            pass

    # ---------- Top cover ----------
    cover = outer.cut(cav).common(box(-1, -1, BASE_T, W + 1, D + 1, h_b + 10))
    adds, cuts = [], []
    screw_max = 99.0
    for hx, hy in holes:                                  # bosses + reinforcing ribs
        col = Part.makeCylinder(BOSS_D / 2, h_b, V(hx, hy, z_pcb1))
        if hy < D / 2:
            rib = box(hx - RIB_W / 2, WALL - 0.5, z_pcb1 + RIB_LIFT, hx + RIB_W / 2, hy, h_b)
        else:
            rib = box(hx - RIB_W / 2, hy, z_pcb1 + RIB_LIFT, hx + RIB_W / 2, D - WALL + 0.5, h_b)
        adds.append(col.fuse(rib).common(cav))
        # Pilot hole must not break through the top face: leave at least 1 mm of top skin
        if shelf and hx >= shelf['ox0'] and shelf['oy0'] <= hy <= shelf['oy1']:
            z_in = shelf['itop']
        else:
            z_in = z0 + max(hy - BOSS_D / 2, WALL) * TAN
        pilot_top = min(z_pcb1 + PILOT_DEPTH, z_in + WALL - 1.0)
        cuts.append(Part.makeCylinder(PILOT_D / 2, pilot_top - z_pcb1 + 1, V(hx, hy, z_pcb1 - 1)))
        screw_max = min(screw_max, pilot_top - 0.5 - SCREW_SEAT_Z)
    screw = max([n for n in (6, 8, 10, 12, 14, 16, 18, 20) if n <= screw_max] or [0])
    log('M3 bosses: four identical M3x%d self-tapping screws (max length under head %.1f, engagement in boss %.1f mm)'
        % (screw, screw_max, SCREW_SEAT_Z + screw - z_pcb1))
    for px, py in LCD_HOLES:                              # screen mounting bosses
        a, s = lcd_ap(px, py)
        adds.append(pn.cyl(a, s, LCD_BOSS_D / 2, -LCD_STACK, 0.5).common(cav))
        cuts.append(pn.cyl(a, s, LCD_PILOT_D / 2, -LCD_STACK - 0.1, WALL - 1.0))   # leave 1.0 mm of top skin
    # Window + outer chamfer
    win_c = win_a + WIN_DA
    cuts.append(pn.box(win_c - WIN_W / 2, win_c + WIN_W / 2, win_s - WIN_H / 2, win_s + WIN_H / 2, -1, WALL + 1))
    rect = []
    for hh, grow in ((WALL - WIN_CHAMFER, 0.0), (WALL + 0.01, WIN_CHAMFER + 0.01)):
        w2, h2 = WIN_W / 2 + grow, WIN_H / 2 + grow
        rect.append(Part.makePolygon([pn.pt(win_c + u * w2, win_s + v * h2, hh)
                                      for u, v in ((-1, -1), (1, -1), (1, 1), (-1, 1), (-1, -1))]))
    cuts.append(Part.makeLoft(rect, True))
    # Touch: inner recess locates the module, shallow round mark on the outside
    tw, th = TOUCH_W / 2 + TOUCH_CLR, TOUCH_H / 2 + TOUCH_CLR
    if shelf:
        z_ceil = shelf['itop'] + WALL - TOUCH_SKIN
        cuts.append(box(ta - tw, ts - th, shelf['itop'] - 1, ta + tw, ts + th, z_ceil))
        mark = V(ta, ts, shelf['top'] - TOUCH_MARK_DEPTH)
        cuts.append(Part.makeCylinder(TOUCH_MARK_D / 2, 2, mark).cut(
            Part.makeCylinder(TOUCH_MARK_D / 2 - TOUCH_MARK_W, 2, mark)))
    else:
        cuts.append(pn.box(ta - tw, ta + tw, ts - th, ts + th, -1, WALL - TOUCH_SKIN))
        cuts.append(pn.cyl(ta, ts, TOUCH_MARK_D / 2, WALL - TOUCH_MARK_DEPTH, WALL + 1))
    # USB-C opening + plug relief
    for p in usb:
        yc, zc = (p['y1'] + p['y2']) / 2, z_pcb1 + USB_BODY_H / 2
        cuts.append(rbox(-1, yc - USB_HOLE_W / 2, zc - USB_HOLE_H / 2, WALL + 1, yc + USB_HOLE_W / 2,
                         zc + USB_HOLE_H / 2, USB_HOLE_R, V(1, 0, 0)))
        cuts.append(rbox(-1, yc - USB_RECESS_W / 2, zc - USB_RECESS_H / 2, USB_RECESS_D, yc + USB_RECESS_W / 2,
                         zc + USB_RECESS_H / 2, USB_RECESS_R, V(1, 0, 0)))
    # XT60: U-shaped notch (top cover slides on from above)
    xt_y1, xt_y2 = xt60['y1'] - XT60_CLR, xt60['y2'] + XT60_CLR
    cuts.append(box(W - WALL - 1, xt_y1, BASE_T - 1, W + 1, xt_y2, z_pcb1 + XT60_H + XT60_CLR))
    if VENTS:                                             # vertical vent slots in the back wall: only a 2 mm small bridge when the slanted cover is printed on its side
        vz1 = z0 + (D - WALL) * TAN - 3.0
        vz0 = max(z_pcb1 + 8.0, vz1 - 18.0)
        vx1 = (shelf['ox0'] if shelf else W) - 8.0
        for xc in [25.0 + 5.0 * i for i in range(20) if 25.0 + 5.0 * i <= vx1]:
            cuts.append(rbox(xc - 1, D - WALL - 1, vz0, xc + 1, D + 1, vz1, 0.99, V(0, 1, 0)))
    cover = fuse_all([cover] + adds).cut(fuse_all(cuts))

    # ---------- Split the top cover in two: the shelf region (below the shelf top height) and the remaining slanted cover, each printed without supports ----------
    if shelf:
        reg = box(shelf['ox0'], -1, -1, W + 1, D + 1, shelf['top'])
        cover_shelf, cover_panel = cover.common(reg), cover.cut(reg)
        log('Top cover split into slanted cover %d solid(s), shelf cover %d solid(s)' % (len(cover_panel.Solids), len(cover_shelf.Solids)))
    else:
        cover_shelf, cover_panel = None, cover

    # ---------- Base plate ----------
    base = base_outer.common(box(-1, -1, -1, W + 1, D + 1, BASE_T))
    botf = [f for f in base.Faces if f.Surface.__class__.__name__ == 'Plane'
            and f.normalAt(*f.ParameterRange[::2]).z < -0.999]
    if botf:
        base = chamfer(base, BASE_CHAMFER, botf[0].Edges, 'base plate bottom edge')
    li = WALL + LIP_CLR
    lip = rbox(li, li, BASE_T - 0.01, W - li, D - li, BASE_T + LIP_H, CORNER_R - li).cut(
        rbox(li + LIP_T, li + LIP_T, BASE_T - 1, W - li - LIP_T, D - li - LIP_T, BASE_T + LIP_H + 1,
             max(0.5, CORNER_R - li - LIP_T)))
    badd = [lip, box(W - WALL - 0.2, xt_y1 + 0.2, BASE_T - 0.01, W - 0.05, xt_y2 - 0.2, z_pcb1 - 0.2)]
    bcut = []
    for hx, hy in holes:
        badd.append(Part.makeCylinder(STANDOFF_D / 2, STANDOFF_H + 0.01, V(hx, hy, BASE_T - 0.01)))
        bcut.append(Part.makeCylinder(SCREW_CLR_D / 2, z_pcb0 + 2, V(hx, hy, -1)))
        bcut.append(Part.makeCylinder(SCREW_HEAD_D / 2, SCREW_HEAD_H + 1, V(hx, hy, -1)))
        cone_h = (SCREW_HEAD_D - SCREW_CLR_D) / 2
        bcut.append(Part.makeCone(SCREW_HEAD_D / 2, SCREW_CLR_D / 2, cone_h, V(hx, hy, SCREW_HEAD_H)))
    if VENTS:                                             # intake slots under the power stage
        qs = [p for p in parts if p['fp'].startswith('HSOF')]
        qx1, qx2 = min(p['x1'] for p in qs), max(p['x2'] for p in qs)
        qy1, qy2 = min(p['y1'] for p in qs), max(p['y2'] for p in qs)
        n = int((qx2 - qx1) / 4.5) + 1
        for i in range(n):
            xc = (qx1 + qx2) / 2 + (i - (n - 1) / 2) * 4.5
            bcut.append(rbox(xc - 1, qy1, -1, xc + 1, qy2, BASE_T + 1, 0.99, V(0, 0, 1)))
    base = fuse_all([base] + badd).cut(fuse_all(bcut))

    # ---------- Reference models: main board, screen, touch ----------
    board = rbox(bx, by, z_pcb0, bx + bd['w'], by + bd['h'], z_pcb1, bd['r'])
    board = board.cut(fuse_all([Part.makeCylinder(d / 2, 5, V(x + bx, y + by, z_pcb0 - 1))
                                for x, y, d in bd['holes']]))
    comp_shapes = {}
    for p in parts:
        if p['kind'] == 'cyl':
            s = Part.makeCylinder(p['dia'] / 2, p['h'], V((p['x1'] + p['x2']) / 2, (p['y1'] + p['y2']) / 2, z_pcb1))
        else:
            s = box(p['x1'], p['y1'], z_pcb1, p['x2'], p['y2'], p['top'])
        comp_shapes[p['ref']] = s
    lcd_pcb = pn.box(a0, a0 + LCD_L, s0, s0 + LCD_W, -LCD_STACK - LCD_PCB_T, -LCD_STACK)
    lcd_pcb = lcd_pcb.cut(fuse_all([pn.cyl(*lcd_ap(px, py), 1.25, -LCD_T - 1, 1) for px, py in LCD_HOLES]))
    ga0, gs0 = lcd_ap(LCD_GLASS[0], LCD_GLASS[1])
    ga1, gs1 = lcd_ap(LCD_GLASS[2], LCD_GLASS[3])
    lcd_glass = pn.box(min(ga0, ga1), max(ga0, ga1), min(gs0, gs1), max(gs0, gs1), -LCD_STACK, 0)
    lcd_aa = pn.box(win_a - LCD_AA[3] / 2, win_a + LCD_AA[3] / 2, win_s - LCD_AA[2] / 2, win_s + LCD_AA[2] / 2,
                    0, 0.05)
    lcd_back = pn.box(a0 + 10, a0 + 40, s0 + 6, s0 + LCD_W - 6, -LCD_T, -LCD_STACK - LCD_PCB_T)
    if shelf:
        touch_pcb = box(ta - TOUCH_W / 2, ts - TOUCH_H / 2, z_ceil - TOUCH_PCB_T,
                        ta + TOUCH_W / 2, ts + TOUCH_H / 2, z_ceil)
        touch_ic = box(ta - 1, ts - 2, z_ceil - TOUCH_PCB_T - 1.2, ta + 4, ts + 2, z_ceil - TOUCH_PCB_T)
        for p in parts:                                   # reserve TOUCH_KEEP under the shelf for the component side and solder wires
            if (p['x2'] + CLEAR > ta - TOUCH_W / 2 and p['x1'] - CLEAR < ta + TOUCH_W / 2
                    and p['y2'] + CLEAR > ts - TOUCH_H / 2 and p['y1'] - CLEAR < ts + TOUCH_H / 2
                    and p['top'] + CLEAR > shelf['itop'] - TOUCH_KEEP):
                log('  ! insufficient keep-out under touch: %s top height %.1f' % (p['ref'], p['top']))
    else:
        touch_pcb = pn.box(ta - TOUCH_W / 2, ta + TOUCH_W / 2, ts - TOUCH_H / 2, ts + TOUCH_H / 2,
                           WALL - TOUCH_SKIN - TOUCH_PCB_T, WALL - TOUCH_SKIN)
        touch_ic = pn.box(ta - 1, ta + 4, ts - 2, ts + 2, WALL - TOUCH_SKIN - TOUCH_PCB_T - 1.2,
                          WALL - TOUCH_SKIN - TOUCH_PCB_T)

    return dict(W=W, D=D, h_f=h_f, h_b=h_b, z0=z0, s0=s0, ta=ta, ts=ts, win=(win_a, win_s), why=why, shelf=shelf,
                cover=cover, cover_panel=cover_panel, cover_shelf=cover_shelf,
                base=base, board=board, comps=comp_shapes, parts=parts, pn=pn,
                lcd=dict(pcb=lcd_pcb, glass=lcd_glass, aa=lcd_aa, back=lcd_back),
                touch=dict(pcb=touch_pcb, ic=touch_ic), z_pcb1=z_pcb1)


# ---------------- Checks ----------------
def check(m):
    log('Interference check (overlap volume mm^3):')
    bad = 0
    lcd = Part.makeCompound(list(m['lcd'].values()))
    touch = Part.makeCompound(list(m['touch'].values()))
    pairs = [('top cover', m['cover'], 'main board', m['board']), ('base plate', m['base'], 'main board', m['board']),
             ('top cover', m['cover'], 'base plate', m['base']), ('top cover', m['cover'], 'screen', lcd),
             ('top cover', m['cover'], 'touch', touch)]
    if m['cover_shelf'] is not None:
        pairs.append(('slanted cover', m['cover_panel'], 'shelf cover', m['cover_shelf']))
    for ref, s in m['comps'].items():
        pairs += [('top cover', m['cover'], ref, s), ('screen', lcd, ref, s), ('touch', touch, ref, s)]
    for na, a, nb, b in pairs:
        if not a.BoundBox.intersect(b.BoundBox):
            continue
        c = a.common(b)
        if c.Volume > 0.01:
            bad += 1
            bb = c.BoundBox
            log('  ! %s ∩ %s = %.2f  @ X %.1f..%.1f Y %.1f..%.1f Z %.1f..%.1f'
                % (na, nb, c.Volume, bb.XMin, bb.XMax, bb.YMin, bb.YMax, bb.ZMin, bb.ZMax))
    comps = Part.makeCompound(list(m['comps'].values()))
    for name, s in (('screen', lcd), ('touch', touch), ('top cover', m['cover'])):
        d = s.distToShape(comps)[0]
        log('  %s minimum distance to main board parts %.2f mm' % (name, d))
    log('  %d interference(s) in total' % bad)
    return bad


# ---------------- Output ----------------
def export(m):
    os.makedirs(OUT, exist_ok=True)
    doc = App.newDocument('ORing_Enclosure')
    gui = App.GuiUp

    def add(name, shape, color=None, transp=0):
        o = doc.addObject('Part::Feature', name)
        o.Shape = shape
        if gui and color:
            o.ViewObject.ShapeColor = color
            o.ViewObject.Transparency = transp
        return o

    add('CoverPanel', m['cover_panel'], COLOR_COVER)
    if m['cover_shelf'] is not None:
        add('CoverShelf', m['cover_shelf'], COLOR_COVER)
    add('Base', m['base'], COLOR_BASE)
    add('PCB', m['board'], (0.05, 0.40, 0.15))
    add('PCB_Parts', Part.makeCompound(list(m['comps'].values())), (0.75, 0.70, 0.55))
    add('LCD_PCB', m['lcd']['pcb'], (0.10, 0.25, 0.70))
    add('LCD_Glass', m['lcd']['glass'], (0.05, 0.05, 0.05))
    add('LCD_ActiveArea', m['lcd']['aa'], (0.20, 0.45, 0.90))
    add('LCD_BackParts', m['lcd']['back'], (0.6, 0.6, 0.6))
    add('TTP223', m['touch']['pcb'].fuse(m['touch']['ic']), (0.80, 0.10, 0.10))
    doc.recompute()

    for stale in ('cover.step', 'cover.stl', 'base.stl'):          # outputs of the old single-piece top cover
        if os.path.exists(os.path.join(OUT, stale)):
            os.remove(os.path.join(OUT, stale))
    pieces = [('cover_panel', m['cover_panel']), ('cover_shelf', m['cover_shelf']), ('base', m['base'])]
    pieces = [(n, s) for n, s in pieces if s is not None]
    for name, shp in pieces:                                        # STEP: assembly position
        shp.exportStep(os.path.join(OUT, name + '.step'))
    prints = print_poses(pieces)                                    # STL: print orientation
    os.makedirs(os.path.join(OUT, 'print'), exist_ok=True)
    log('Print overhang check (normal facing down, angle to horizontal < 45°, excluding the bed face):')
    for i, (name, shp) in enumerate(prints, 1):
        mesh_out(shp, os.path.join(OUT, 'print', '%d_%s.stl' % (i, name)))
        overhang_report(name, shp)
    Part.makeCompound([m['cover'], m['base'], m['board']] + list(m['comps'].values())
                      + list(m['lcd'].values()) + list(m['touch'].values())).exportStep(
        os.path.join(OUT, 'assembly.step'))
    doc.saveAs(os.path.join(HERE, 'ORing_Enclosure.FCStd'))
    if gui:
        from PySide import QtCore
        QtCore.QTimer.singleShot(1500, lambda: gui_shots(doc, prints))
    return doc


def gui_shots(doc, prints):
    """GUI screenshots; with ENCL_QUIT=1, FreeCAD exits right after the screenshots (for batch use)."""
    import FreeCADGui as Gui
    view = Gui.getDocument(doc.Name).ActiveView

    def cam(dx, dy, dz):
        z = V(dx, dy, dz).normalize()
        x = V(0, 0, 1).cross(z).normalize()
        y = z.cross(x)
        return App.Placement(App.Matrix(x.x, y.x, z.x, 0, x.y, y.y, z.y, 0, x.z, y.z, z.z, 0, 0, 0, 0, 1)).Rotation

    covers = [o for o in doc.Objects if o.Name.startswith('Cover')]

    def shot(name, d, hide=(), cover_transp=0):
        for o in doc.Objects:
            o.ViewObject.Visibility = o.Name not in hide
        for o in covers:
            o.ViewObject.Transparency = cover_transp
        view.setCameraOrientation(cam(*d), True)      # True = no transition animation, otherwise the capture is taken mid-way
        view.fitAll()
        Gui.updateGui()
        view.saveImage(os.path.join(OUT, name), 1600, 1100, 'White')

    exterior = ()
    shot('gui_front_left.png', (-1, -1.6, 1.1), exterior)   # warm-up: the first shot right after the window is created may be blank
    shot('gui_front_left.png', (-1, -1.6, 1.1), exterior)
    shot('gui_front_right.png', (1, -1.6, 1.1), exterior)
    shot('gui_back.png', (0.8, 1.6, 1.0), exterior)
    shot('gui_bottom.png', (-0.6, -1.0, -1.2), exterior)
    shot('gui_inside.png', (1, -1.6, 1.1), ('LCD_ActiveArea',), 75)
    shot('gui_inside_nocover.png', (-1, -1.4, 1.3), ('CoverPanel', 'CoverShelf', 'LCD_ActiveArea'))
    for o in doc.Objects:
        o.ViewObject.Visibility = o.Name != 'LCD_ActiveArea'
    for o in covers:
        o.ViewObject.Transparency = 0
    view.setCameraOrientation(cam(1, -1.6, 1.1), True)
    view.fitAll()
    doc.save()

    # Print layout: the three pieces side by side in print orientation
    lay = App.newDocument('PrintLayout')
    x = 0.0
    for (name, shp), col in zip(prints, (COLOR_COVER, COLOR_COVER, COLOR_BASE)):
        s = shp.translated(V(x - shp.BoundBox.XMin, 0, 0))
        o = lay.addObject('Part::Feature', name)
        o.Shape = s
        o.ViewObject.ShapeColor = col
        x += shp.BoundBox.XLength + 10
    lay.recompute()
    lview = Gui.getDocument(lay.Name).ActiveView
    lview.setCameraOrientation(cam(0.6, -1.3, 1.0), True)
    lview.fitAll()
    Gui.updateGui()
    lview.saveImage(os.path.join(OUT, 'gui_print_layout.png'), 1600, 1100, 'White')
    App.closeDocument(lay.Name)
    log('GUI screenshots: gui_*.png')
    with open(os.path.join(OUT, 'build_log.txt'), 'w', encoding='utf-8') as f:
        f.write('\n'.join(_LOG) + '\n')
    if os.environ.get('ENCL_QUIT') == '1':
        os._exit(0)


def print_poses(pieces):
    """Rotate to support-free print orientation and place at z = 0: slanted cover screen face down, shelf cover top face down, base plate unchanged."""
    out = []
    for name, shp in pieces:
        s = shp.copy()
        if name == 'cover_panel':
            s.rotate(V(0, 0, 0), V(1, 0, 0), 180 - TILT_DEG)
        elif name == 'cover_shelf':
            s.rotate(V(0, 0, 0), V(1, 0, 0), 180)
        bb = s.BoundBox
        out.append((name, s.translated(V(-bb.XMin, -bb.YMin, -bb.ZMin))))
    return out


def overhang_report(name, shp, limit_deg=45.0):
    """List areas needing support: faces whose normal points down and whose angle to horizontal is less than limit_deg (bed face excluded)."""
    lim = math.cos(math.radians(limit_deg))
    z_bed = shp.BoundBox.ZMin + 0.6                                 # chamfers within 0.6 mm of the bed are elephant-foot compensation
    total, items = 0.0, []
    for f in shp.Faces:
        if f.BoundBox.ZMax < z_bed:
            continue
        area = 0.0
        if f.Surface.__class__.__name__ == 'Plane':
            if f.normalAt(*f.ParameterRange[::2]).z < -lim - 0.01:     # a 45° chamfer itself is printable, not counted
                area = f.Area
        else:
            pts, tris = f.tessellate(0.2)
            for t in tris:
                p0, p1, p2 = pts[t[0]], pts[t[1]], pts[t[2]]
                c = (p0 + p1 + p2) * (1.0 / 3)
                if c.z < z_bed:
                    continue
                try:
                    n = f.normalAt(*f.Surface.parameter(c))
                except Exception:                               # noqa: BLE001
                    continue
                if n.z < -lim - 0.01:
                    area += (p1 - p0).cross(p2 - p0).Length / 2
        if area > 0.05:
            items.append((area, f.BoundBox))
        total += area
    items.sort(key=lambda t: -t[0])
    log('  %s: %.1f mm², %d face(s)' % (name, total, len(items)))
    for area, bb in items[:6]:
        log('    %.1f mm²  size %.1f x %.1f  @ z %.1f' % (area, bb.XLength, bb.YLength, bb.ZMin))


def mesh_out(shape, path):
    import MeshPart
    mesh = MeshPart.meshFromShape(Shape=shape, LinearDeflection=0.03, AngularDeflection=math.radians(8),
                                  Relative=False)
    mesh.write(path)


def dump_preview(m, path):
    """Save the section and top-view data needed for the check images as JSON, handed to render_preview.py for drawing.
    (Loading matplotlib inside freecadcmd conflicts with FreeCAD's expat DLL, so a separate process is used.)"""
    def section(shape, x):
        out = []
        for w in shape.slice(V(1, 0, 0), x):
            pts = w.discretize(Deflection=0.05)
            out.append({'closed': w.isClosed(), 'pts': [[round(p.y, 3), round(p.z, 3)] for p in pts]})
        return out

    lcd_all = Part.makeCompound(list(m['lcd'].values()))
    touch_all = Part.makeCompound(list(m['touch'].values()))
    comps = Part.makeCompound(list(m['comps'].values()))
    sec_items = [('top cover', m['cover'], '#9aa0a8'), ('base plate', m['base'], '#555b62'), ('main board', m['board'], '#1d7a3a'),
                 ('parts', comps, '#c8b07a'), ('screen', lcd_all, '#2747b5'), ('touch', touch_all, '#cc2222')]
    outline = []
    for e in m['base'].common(box(-1, -1, -1, m['W'] + 1, m['D'] + 1, 0.01)).Edges:
        outline.append([[round(p.x, 3), round(p.y, 3)] for p in e.discretize(Deflection=0.05)])
    data = {
        'W': m['W'], 'D': m['D'], 'h_b': m['h_b'],
        'sections': [
            {'title': 'Section X=%.1f (screen center)' % m['win'][0],
             'items': [[n, c, section(s, m['win'][0])] for n, s, c in sec_items]},
            {'title': 'Section X=%.1f (touch center)' % m['ta'],
             'items': [[n, c, section(s, m['ta'])] for n, s, c in sec_items]},
        ],
        'top': {
            'outline': outline,
            'board': [m['board'].BoundBox.XMin, m['board'].BoundBox.YMin,
                      m['board'].BoundBox.XMax, m['board'].BoundBox.YMax],
            'parts': [[p['ref'], p['x1'], p['y1'], p['x2'], p['y2'], p['h']] for p in m['parts']],
            'modules': [[n, c, [s.BoundBox.XMin, s.BoundBox.YMin, s.BoundBox.XMax, s.BoundBox.YMax]]
                        for n, s, c in (('screen', lcd_all, '#2747b5'), ('touch', touch_all, '#cc2222'))],
        },
    }
    with open(path, 'w', encoding='utf-8') as f:
        json.dump(data, f, ensure_ascii=False)


def render(m):
    """The pyexpat bundled with FreeCAD's python cannot be loaded, so the system Python (with matplotlib) is preferred; can be specified with ENCL_PYTHON."""
    import shutil
    import subprocess
    data = os.path.join(OUT, '_preview.json')
    dump_preview(m, data)
    fc_bin = os.path.normcase(os.path.join(App.getHomePath(), 'bin'))
    cands = [os.environ.get('ENCL_PYTHON'), shutil.which('python'), shutil.which('py')]
    cands = [c for c in cands if c and os.path.normcase(os.path.dirname(c)) != fc_bin]
    err = 'system Python not found'
    for py in cands:
        r = subprocess.run([py, os.path.join(HERE, 'render_preview.py'), data, OUT],
                           capture_output=True, text=True, encoding='utf-8', errors='replace')
        if r.returncode == 0:
            os.remove(data)
            log('Check images: preview_section.png / preview_top.png')
            return
        err = (r.stderr or r.stdout).strip()[-600:]
    log('  ! check images failed:', err)


def main():
    m = build()
    log('Screen s0 along slanted face = %.2f, window center (%.1f, %.1f), touch center %s (%.1f, %.1f)'
        % (m['s0'], m['win'][0], m['win'][1], 'shelf top X/Y' if m['shelf'] else 'slanted face a/s', m['ta'], m['ts']))
    check(m)
    export(m)
    if not App.GuiUp:
        render(m)
    log('Output:', OUT)
    with open(os.path.join(OUT, 'build_log.txt'), 'w', encoding='utf-8') as f:
        f.write('\n'.join(_LOG) + '\n')


main()
