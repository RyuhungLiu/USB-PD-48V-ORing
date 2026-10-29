# -*- coding: utf-8 -*-
"""
ATX_PSU 3D-printed enclosure generator (FreeCAD 1.1): ORing main board + X9-ATX-500 + 2.0" screen + TTP223, standard ATX power supply size.

  "C:\\Program Files\\FreeCAD 1.1\\bin\\freecadcmd.exe" gen_atx_case.py   generates FCStd / STEP / STL and the check log
  "C:\\Program Files\\FreeCAD 1.1\\bin\\freecad.exe"    gen_atx_case.py   same as above, plus coloring and screenshots (ENCL_QUIT=1 closes after the screenshots)

Outline: 150 x 140 x 86 mm (ATX12V PSU Design Guide 1.1 Fig. 4); orientation of the 4 #6-32 hole positions on the rear panel checked against NopSCADlib ATX500.
Coordinates: X from left to right (viewed from outside the rear panel), Y from the rear panel toward the front, Z up; origin at the lower-left corner of the outside of the rear panel.

Printing (out/<variant>/print/, already rotated, no supports needed; variant fan_up = fan mounted facing up, fan_down = fan mounted facing down, screen module rotated 180°):
  1_shell  top shell (purple): top-face fan grille face down on the bed
  2_base   base plate (green): bottom face down
Assembly: 4 #6-32 heat-set brass inserts in the rear panel; M3 countersunk screws through the base plate from below into the top shell and the X9 back plate; ORing main board fastened from above with M3 self-tapping screws.
"""
import math
import os
import sys

import FreeCAD as App
import Part
from FreeCAD import Vector as V

# ======================= Source =======================
EPRO_FILE = 'ProPrj_48V10+10A ORing_2026-10-10_en.epro2'
PCB_TITLE = 'PCB1_2'
OMIT_REFS = ('C12', 'C14', 'H2', 'H4')   # not populated: input 100µF; H2/H4 have no pin headers (wires soldered directly)
WIRE_KEEP = 4.0                          # reserved above pads without pin headers for solder joints and wires

# ======================= ATX outline =======================
ATX_W, ATX_D, ATX_H = 150.0, 140.0, 86.0
# Rear panel #6-32 holes (viewed from outside: x from the left, z from the bottom): the two top corners, lower-left 16 from the bottom, bottom edge 30 from the right
ATX_HOLES = [(6.0, 80.0), (144.0, 80.0), (6.0, 16.0), (120.0, 6.0)]
INSERT_D, INSERT_DEPTH = 4.8, 8.0        # #6-32 heat-set brass insert hole (for inserts about 5 mm OD, 6.4 mm long)
INSERT_BOSS_D, INSERT_BOSS_L = 10.0, 6.5  # insert boss diameter, length inward from the rear panel inner face
REAR_T = 3.0                             # rear panel thickness
WALL = 2.4                               # side wall, front wall, top face thickness
BASE_T = 3.0                             # base plate thickness (green band)
CORNER_R = 3.0                           # vertical edge fillet radius
EDGE_CHAMFER = 0.5                       # 45° chamfer on bed-side edges (elephant-foot compensation)
LIP_H, LIP_T, LIP_CLR = 2.0, 1.6, 0.25

# Top shell and base plate: M3 countersunk (Ø6, 90°) screwed from under the base plate into the bosses along the top shell walls
SHELL_BOSSES = [('L', 50.0), ('L', 120.0), ('R', 72.0), ('R', 102.0), ('F', 30.0)]
BOSS_D, BOSS_H, PILOT_D = 7.0, 10.0, 2.6
SCREW_CLR_D, CSK_D = 3.4, 6.4
FOOT_D, FOOT_DEPTH = 10.0, 0.6           # foot pad recess (3 layers)

# ======================= ORing main board =======================
OR_X_RIGHT = 69.0                        # USB edge facing the rear, the other long edge of the main board at X = 69 (viewed from outside the main board is on the left; 3 mm clearance from the X9)
OR_GAP_REAR = 0.4                        # USB-C end face to the rear panel inner face
OR_STANDOFF_H, OR_STANDOFF_D, OR_PILOT_D = 12.0, 7.0, 2.5   # raised: when mounted upside down with the fan blowing downward, the USB-C clears the opening frame of the PC case
PCB_T = 1.6
USB_BODY_H = 3.26
USB_HOLE_W, USB_HOLE_H, USB_HOLE_R = 9.6, 4.0, 1.6
USB_RECESS_W, USB_RECESS_H, USB_RECESS_R = 12.8, 7.2, 2.5
USB_RECESS_D = REAR_T - 0.8              # outer relief for the plug overmold, leaving 0.8 mm
XT60_H = 8.3

# ======================= X9-ATX-500 =======================
X9_L, X9_W, X9_H = 125.0, 63.0, 31.0     # including heatsink and back plate
# Back plate M3 threaded holes (X9 top-view coordinates u along 125, v along 63, P1 on the v = 63 edge); the 4 chosen are left-right mirror symmetric, please confirm with the real part
X9_HOLES = [(5.8, 57.2), (119.2, 57.2), (5.8, 31.8), (119.2, 31.6)]
# X9 orientation: 0 = P1 long edge toward the right wall, P5 / P6 toward the front; 180 = rotated half a turn about the center (P1 long edge toward the ORing main board, P5 / P6 near the rear panel, J1/J3 toward the front)
X9_ROT = 180
# X9 outline X 72..135; when rotated 180° it moves 2 mm forward so the P5 plug clears the rear panel (120, 6) insert boss (the base plate locating lip is relieved at the X9)
X9_X0, X9_Y_FRONT = 72.0, (137.0 if X9_ROT == 180 else 135.0)
X9_CONN_KEEP = 40.0                      # vertical connectors + plugs + wire bends, keep-out height measured from the base plate
# Connector keep-out zones (X9 top-view coordinates u0, u1, v0, v1, top height; rotated along with X9_ROT)
X9_KEEPS = [('X9 long-edge connectors', 0.0, 125.0, 52.0, 64.0, X9_CONN_KEEP),      # P1 24P, P3, P4
            ('X9 short-edge connectors', -1.0, 11.0, 6.0, 63.0, X9_CONN_KEEP),       # P5 input, P6
            ('X9 fan/UART', 117.0, 125.0, 20.0, 50.0, 25.0)]           # J1, J2, J3
X9_P5 = (6.0, 14.5)                      # P5 input center (top-view coordinates), used to estimate the XT60 -> P5 wire length

# ======================= Fan =======================
FAN, FAN_T, FAN_HOLE, FAN_SCREW_D = 120.0, 25.0, 105.0, 4.4
FAN_C = (75.0, 74.0)
GRILLE_R, HEX, HEX_RIB = 57.0, 10.0, 1.8   # honeycomb hole pitch 10, rib width 1.8 (hole across flats 8.2, prints cleaner on the bed)

# ======================= Screen and touch (rear panel, viewed from outside) =======================
LCD_W, LCD_L, LCD_T = 37.1, 62.0, 6.0
LCD_HOLES = [(4.4, 1.8), (32.7, 1.8), (4.4, 59.95), (32.7, 59.95)]
LCD_GLASS = (1.85, 5.25, 35.25, 57.5)
LCD_AA = (18.55, 28.55, 30.6, 40.8)      # active-area center px, py and size (reused from the two real-photo corrections of the slanted enclosure)
LCD_STACK, LCD_PCB_T = 3.2, 1.6
LCD_BOSS_D, LCD_PILOT_D = 4.2, 2.0
LCD_WIRE_KEEP = 3.0
WIN_CX, WIN_CZ = 62.5, 50.0              # window center (= active-area center); the window and touch group is centered horizontally on the rear panel
WIN_W, WIN_H = 42.0, 33.0                # visible opening; 45° bevel on the outside through the full rear panel thickness (no bridging when printed upside down)
TOUCH_W, TOUCH_H, TOUCH_PCB_T = 14.2, 10.9, 1.6
TOUCH_X = 106.0                          # touch center X; Z at the same height as the window center
# Two top-shell variants: fan up (desktop), fan down (PC case with bottom-mounted PSU; when the whole unit is mounted upside down the rear panel is flipped -> screen module rotated 180°, no firmware change needed)
VARIANTS = (('fan_up', 0), ('fan_down', 180))
TOUCH_SKIN, TOUCH_CLR = 1.0, 0.2
TOUCH_MARK_D, TOUCH_MARK_DEPTH, TOUCH_MARK_W = 11.0, 0.4, 0.6

# ======================= Openings =======================
CABLE_NOTCH = (78.0, 142.0, 46.0, 8.0)   # U-shaped cable notch in the front wall: x0, x1, top Z, top corner fillet radius
REAR_VENT_X = [14.0, 18.5, 23.0, 27.5] + [119.0 + 4.5 * i for i in range(6)]   # vertical vent slots in the rear panel (on both sides of the screen group)
REAR_VENT_Z = (26.0, 70.0)                # lower edge clears the raised USB-C relief slot
FRONT_VENT_X = [12.0 + 5.0 * i for i in range(12)]
FRONT_VENT_Z = (16.0, 66.0)
SLOT_W = 2.0

COLOR_COVER = (0.42, 0.30, 0.86)
COLOR_BASE = (0.42, 0.80, 0.28)


def _here():
    try:
        return os.path.dirname(os.path.abspath(__file__))
    except NameError:
        return os.environ.get('ENCL_DIR', os.getcwd())


HERE = _here()
PROJ = os.path.dirname(os.path.dirname(HERE))
OUT = os.path.join(HERE, 'out')
sys.path.insert(0, os.path.join(PROJ, 'Enclosure'))
import epro_pcb  # noqa: E402

_LOG = []


def log(*args):
    msg = ' '.join(str(a) for a in args)
    _LOG.append(msg)
    try:
        print(msg)
    except UnicodeEncodeError:
        print(msg.encode('ascii', 'replace').decode('ascii'))


def write_log():
    os.makedirs(OUT, exist_ok=True)
    with open(os.path.join(OUT, 'build_log.txt'), 'w', encoding='utf-8') as f:
        f.write('\n'.join(_LOG) + '\n')


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


def _try(fn, shape, r, edges, what):
    if r <= 0 or not edges:
        return shape
    try:
        return fn(r, edges)
    except Exception as exc:                              # noqa: BLE001 - fillet/chamfer failure does not affect function
        log('  ! %s failed: %s' % (what, exc))
        return shape


def fillet(shape, r, edges, what):
    return _try(shape.makeFillet, shape, r, edges, what + ' fillet')


def chamfer(shape, d, edges, what):
    return _try(shape.makeChamfer, shape, d, edges, what + ' chamfer')


def rbox(x0, y0, z0, x1, y1, z1, r, axis=V(0, 0, 1)):
    b = box(x0, y0, z0, x1, y1, z1)
    return fillet(b, r, edges_along(b, axis), 'rbox')


def face_edges(shape, normal):
    for f in shape.Faces:
        if f.Surface.__class__.__name__ == 'Plane' and f.normalAt(*f.ParameterRange[::2]).dot(normal) > 0.999:
            return f.Edges
    return []


def cyl_y(x, z, r, y0, y1):
    return Part.makeCylinder(r, y1 - y0, V(x, y0, z), V(0, 1, 0))


def cyl_z(x, y, r, z0, z1):
    return Part.makeCylinder(r, z1 - z0, V(x, y, z0))


def fuse_all(shapes):
    shapes = [s for s in shapes if s is not None and not s.isNull()]
    if not shapes:
        return None
    out = shapes[0].fuse(shapes[1:]) if len(shapes) > 1 else shapes[0]
    try:
        return out.removeSplitter()
    except Exception:                                     # noqa: BLE001
        return out


def sloped_block(axis, w0, w1, c0, c1, z0, z1):
    """Block against a wall, its top sloping up 45° toward the wall face (when printed upside down the slope faces down but is <= 45°, no support needed).
    axis = 'x': wall normal along X, c0 is the wall face, c1 the outer edge of the block; w0..w1 along Y. For axis = 'y' they are swapped."""
    d = abs(c1 - c0)
    pts = [(c0, z0), (c1, z0), (c1, z1), (c0, z1 + d)]
    if axis == 'x':
        poly = Part.makePolygon([V(c, w0, z) for c, z in pts + pts[:1]])
        return Part.Face(poly).extrude(V(0, w1 - w0, 0))
    poly = Part.makePolygon([V(w0, c, z) for c, z in pts + pts[:1]])
    return Part.Face(poly).extrude(V(w1 - w0, 0, 0))


def csk_hole(x, y):
    """M3 countersunk hole screwed in from under the base plate: through hole + 90° cone (cone opening on the bed face, 45° needs no support)."""
    h = (CSK_D - SCREW_CLR_D) / 2
    return cyl_z(x, y, SCREW_CLR_D / 2, -1, BASE_T + 1).fuse(
        Part.makeCone(CSK_D / 2, SCREW_CLR_D / 2, h, V(x, y, -0.01)))


def rear_frustum(xc, zc, w_in, h_in, grow):
    """Rear panel opening: frustum with inner face w_in x h_in and outer face grown by grow on each side (Y from REAR_T to 0)."""
    wires = []
    for y, g in ((REAR_T + 0.01, 0.0), (-0.01, grow + 0.01)):
        w2, h2 = w_in / 2 + g, h_in / 2 + g
        wires.append(Part.makePolygon([V(xc + u * w2, y, zc + v * h2)
                                       for u, v in ((-1, -1), (1, -1), (1, 1), (-1, 1), (-1, -1))]))
    return Part.makeLoft(wires, True)


# ---------------- Main flow ----------------
def build(rot):
    W, D, H = ATX_W, ATX_D, ATX_H
    z_pcb0 = BASE_T + OR_STANDOFF_H
    z_pcb1 = z_pcb0 + PCB_T
    or_y0 = REAR_T + OR_GAP_REAR
    bd = epro_pcb.load_pcb(os.path.join(PROJ, EPRO_FILE), PCB_TITLE, {'XT60': XT60_H, 'USB-C': USB_BODY_H})

    def or_box(p):                                        # main board coordinates (bx along 91, by along 56) -> model (rotated 90° counterclockwise)
        return OR_X_RIGHT - p['y2'], OR_X_RIGHT - p['y1'], or_y0 + p['x1'], or_y0 + p['x2']

    parts, keeps = [], []
    for p in bd['parts']:
        x1, x2, y1, y2 = or_box(p)
        q = dict(p, x1=x1, x2=x2, y1=y1, y2=y2, top=z_pcb1 + p['h'])
        if p['ref'] in OMIT_REFS:
            if p['fp'].upper().startswith('HDR'):
                keeps.append((p['ref'] + ' solder wires', box(x1 - 1.5, y1 - 1.5, z_pcb1, x2 + 1.5, y2 + 1.5, z_pcb1 + WIRE_KEEP)))
            continue
        parts.append(q)
    or_holes = [(OR_X_RIGHT - y, or_y0 + x) for x, y, _ in bd['holes']]
    usb = [p for p in parts if 'USB-C' in p['fp']]

    def x9(u, v):                                         # X9 top view (u, v) -> model (x, y); 180° is point-symmetric about the outline center
        if X9_ROT == 180:
            u, v = X9_L - u, X9_W - v
        return X9_X0 + v, X9_Y_FRONT - u

    def x9_box(u0, u1, v0, v1, z0, z1):
        (xa, ya), (xb, yb) = x9(u0, v0), x9(u1, v1)
        return box(min(xa, xb), min(ya, yb), z0, max(xa, xb), max(ya, yb), z1)

    x9_holes = [x9(u, v) for u, v in X9_HOLES]
    x9_y0 = X9_Y_FRONT - X9_L

    # ---------- Outer shape and inner cavity ----------
    outer = rbox(0, 0, 0, W, D, H, CORNER_R)
    outer = chamfer(outer, EDGE_CHAMFER, face_edges(outer, V(0, 0, 1)), 'top edge')
    cav = rbox(WALL, REAR_T, -1, W - WALL, D - WALL, H - WALL, max(0.5, CORNER_R - WALL))
    shell = outer.cut(cav).cut(box(-1, REAR_T, -1, W + 1, D + 1, BASE_T))   # side and front walls start from the base plate top edge; the rear panel goes down to the floor

    adds, cuts = [], []
    # Rear panel #6-32 insert bosses: made as wall-attached columns running all the way to the top face, so nothing overhangs when printed upside down
    r = INSERT_BOSS_D / 2
    for hx, hz in ATX_HOLES:                              # sloped-top blocks, top tucked into the rear panel at 45° (the two top-edge ones merge directly into the top face)
        blk = sloped_block('y', hx - r, hx + r, REAR_T - 0.5, REAR_T + INSERT_BOSS_L, hz - r, hz + r)
        adds.append(blk.common(box(0.01, 0, 0, W - 0.01, D, H - 0.01)))
        cuts.append(cyl_y(hx, hz, INSERT_D / 2, -1, INSERT_DEPTH))
    # Bosses along the top shell walls (sitting on the base plate)
    boss_xy, boss_blk = [], []
    zb0, zb1 = BASE_T, BASE_T + BOSS_H
    for side, t in SHELL_BOSSES:                          # sloped-top block bosses, sitting on the base plate
        if side == 'L':
            bx_, by_ = WALL + BOSS_D / 2, t
            blk = sloped_block('x', t - BOSS_D / 2, t + BOSS_D / 2, WALL - 0.5, WALL + BOSS_D, zb0, zb1)
        elif side == 'R':
            bx_, by_ = W - WALL - BOSS_D / 2, t
            blk = sloped_block('x', t - BOSS_D / 2, t + BOSS_D / 2, W - WALL + 0.5, W - WALL - BOSS_D, zb0, zb1)
        else:
            bx_, by_ = t, D - WALL - BOSS_D / 2
            blk = sloped_block('y', t - BOSS_D / 2, t + BOSS_D / 2, D - WALL + 0.5, D - WALL - BOSS_D, zb0, zb1)
        boss_xy.append((bx_, by_))
        boss_blk.append(blk)
        adds.append(blk)
        cuts.append(cyl_z(bx_, by_, PILOT_D / 2, BASE_T - 1, BASE_T + BOSS_H - 1.0))
    # USB-C
    for p in usb:
        xc, zc = (p['x1'] + p['x2']) / 2, z_pcb1 + USB_BODY_H / 2
        cuts.append(rbox(xc - USB_HOLE_W / 2, -1, zc - USB_HOLE_H / 2, xc + USB_HOLE_W / 2, REAR_T + 1,
                         zc + USB_HOLE_H / 2, USB_HOLE_R, V(0, 1, 0)))
        cuts.append(rbox(xc - USB_RECESS_W / 2, -1, zc - USB_RECESS_H / 2, xc + USB_RECESS_W / 2, USB_RECESS_D,
                         zc + USB_RECESS_H / 2, USB_RECESS_R, V(0, 1, 0)))

    # Screen: local (a, s, h) -> (a, REAR_T - h, s); h = 0 is the rear panel inner face, outward positive
    a0, s0 = WIN_CX - LCD_AA[1], WIN_CZ - LCD_AA[0]

    def lcd_ap(px, py):                                   # module portrait coordinates -> rear panel (x, z); when rotated 180° point-symmetric about the window center
        a, s = a0 + py, s0 + px
        return (2 * WIN_CX - a, 2 * WIN_CZ - s) if rot == 180 else (a, s)

    def lbox(x0, x1, z0, z1, h0, h1):
        return box(x0, REAR_T - h1, z0, x1, REAR_T - h0, z1)

    def mbox(px0, py0, px1, py1, h0, h1):                 # box in module coordinates -> box on the rear panel
        (xa, za), (xb, zb) = lcd_ap(px0, py0), lcd_ap(px1, py1)
        return lbox(min(xa, xb), max(xa, xb), min(za, zb), max(za, zb), h0, h1)

    win_x, win_z = WIN_CX, WIN_CZ
    cuts.append(rear_frustum(win_x, win_z, WIN_W, WIN_H, REAR_T))
    for px, py in LCD_HOLES:
        a, s = lcd_ap(px, py)
        adds.append(cyl_y(a, s, LCD_BOSS_D / 2, REAR_T - 0.5, REAR_T + LCD_STACK))
        cuts.append(cyl_y(a, s, LCD_PILOT_D / 2, 1.0, REAR_T + LCD_STACK + 0.1))
    # Touch
    tz = win_z
    cuts.append(box(TOUCH_X - TOUCH_W / 2 - TOUCH_CLR, TOUCH_SKIN, tz - TOUCH_H / 2 - TOUCH_CLR,
                    TOUCH_X + TOUCH_W / 2 + TOUCH_CLR, REAR_T + 1, tz + TOUCH_H / 2 + TOUCH_CLR))
    cuts.append(cyl_y(TOUCH_X, tz, TOUCH_MARK_D / 2, -1, TOUCH_MARK_DEPTH).cut(
        cyl_y(TOUCH_X, tz, TOUCH_MARK_D / 2 - TOUCH_MARK_W, -2, TOUCH_MARK_DEPTH + 1)))
    # Vent slots, cable exit
    for x in REAR_VENT_X:
        cuts.append(rbox(x - SLOT_W / 2, -1, REAR_VENT_Z[0], x + SLOT_W / 2, REAR_T + 1, REAR_VENT_Z[1],
                         SLOT_W / 2 - 0.01, V(0, 1, 0)))
    for x in FRONT_VENT_X:
        cuts.append(rbox(x - SLOT_W / 2, D - WALL - 1, FRONT_VENT_Z[0], x + SLOT_W / 2, D + 1, FRONT_VENT_Z[1],
                         SLOT_W / 2 - 0.01, V(0, 1, 0)))
    nx0, nx1, nz, nr = CABLE_NOTCH
    cuts.append(rbox(nx0, D - WALL - 1, -nr - 1, nx1, D + 1, nz, nr, V(0, 1, 0)))
    # Top face: 120 mm fan grille (honeycomb) and screw holes
    fx, fy = FAN_C
    hexes = []
    r_cell = (HEX - HEX_RIB) / math.sqrt(3)               # hexagonal hole circumradius (across flats = HEX - HEX_RIB)
    row_h = HEX * math.sqrt(3) / 2
    for j in range(-12, 13):
        for i in range(-12, 13):
            cx = fx + i * HEX + (HEX / 2 if j % 2 else 0)
            cy = fy + j * row_h
            if math.hypot(cx - fx, cy - fy) > GRILLE_R - HEX * 0.75:   # trim the edge, avoiding isolated holes at both ends of the middle row
                continue
            poly = Part.makePolygon([V(cx + r_cell * math.cos(math.radians(30 + 60 * k)),
                                       cy + r_cell * math.sin(math.radians(30 + 60 * k)), H - WALL - 1)
                                     for k in range(7)])
            hexes.append(Part.Face(poly).extrude(V(0, 0, WALL + 2)))
    for sx in (-1, 1):
        for sy in (-1, 1):
            cuts.append(cyl_z(fx + sx * FAN_HOLE / 2, fy + sy * FAN_HOLE / 2, FAN_SCREW_D / 2, H - WALL - 1, H + 1))
    shell = fuse_all([shell] + adds).cut(fuse_all(cuts)).cut(Part.makeCompound(hexes))
    log('Top shell %d solid(s), %d honeycomb hole(s)' % (len(shell.Solids), len(hexes)))

    # ---------- Base plate ----------
    base = outer.common(box(-1, REAR_T + LIP_CLR, -1, W + 1, D + 1, BASE_T))
    base = chamfer(base, EDGE_CHAMFER, face_edges(base, V(0, 0, -1)), 'bottom edge')
    li = LIP_CLR
    lip = rbox(WALL + li, REAR_T + li, BASE_T - 0.01, W - WALL - li, D - WALL - li, BASE_T + LIP_H,
               max(0.5, CORNER_R - WALL - li)).cut(
        rbox(WALL + li + LIP_T, REAR_T + li + LIP_T, BASE_T - 1, W - WALL - li - LIP_T, D - WALL - li - LIP_T,
             BASE_T + LIP_H + 1, 0.5))
    badd, bcut = [lip], []
    for x, y in or_holes:
        badd.append(cyl_z(x, y, OR_STANDOFF_D / 2, BASE_T - 0.01, z_pcb0))
        bcut.append(cyl_z(x, y, OR_PILOT_D / 2, 1.0, z_pcb0 + 1))
    for x, y in x9_holes + boss_xy:
        bcut.append(csk_hole(x, y))
    for x, y in ((14.0, 16.0), (W - 14.0, 16.0), (14.0, D - 14.0), (W - 14.0, D - 14.0)):
        bcut.append(cyl_z(x, y, FOOT_D / 2, -1, FOOT_DEPTH))
    for hx, hz in ATX_HOLES:                              # relief for the rear panel insert bosses that reach down to the base plate height
        if hz - INSERT_BOSS_D / 2 < BASE_T + LIP_H:
            bcut.append(box(hx - INSERT_BOSS_D / 2 - 0.4, 0, -1, hx + INSERT_BOSS_D / 2 + 0.4,
                            REAR_T + INSERT_BOSS_L + 0.4, BASE_T + LIP_H + 1))
    base = fuse_all([base] + badd).cut(fuse_all(bcut))
    # The lip is relieved where it meets the top shell bosses
    relief = []
    for blk in boss_blk:
        bb = blk.BoundBox
        relief.append(box(bb.XMin - 0.3, bb.YMin - 0.3, BASE_T, bb.XMax + 0.3, bb.YMax + 0.3, BASE_T + LIP_H + 1))
    relief.append(box(X9_X0 - 0.3, x9_y0 - 0.3, BASE_T, X9_X0 + X9_W + 0.3, X9_Y_FRONT + 0.3, BASE_T + LIP_H + 1))
    base = base.cut(fuse_all(relief))

    # ---------- Reference models ----------
    board = rbox(OR_X_RIGHT - bd['h'], or_y0, z_pcb0, OR_X_RIGHT, or_y0 + bd['w'], z_pcb1, bd['r'])
    board = board.cut(fuse_all([cyl_z(x, y, 1.65, z_pcb0 - 1, z_pcb1 + 1) for x, y in or_holes]))
    comps = {}
    for p in parts:
        if p['kind'] == 'cyl':
            comps[p['ref']] = cyl_z((p['x1'] + p['x2']) / 2, (p['y1'] + p['y2']) / 2, p['dia'] / 2, z_pcb1, p['top'])
        else:
            comps[p['ref']] = box(p['x1'], p['y1'], z_pcb1, p['x2'], p['y2'], p['top'])
    x9_body = box(X9_X0, x9_y0, BASE_T, X9_X0 + X9_W, X9_Y_FRONT, BASE_T + X9_H)
    zc = BASE_T + 4.0                                     # X9 back plate + PCB top surface
    keeps += [(n, x9_box(u0, u1, v0, v1, zc, BASE_T + top)) for n, u0, u1, v0, v1, top in X9_KEEPS]
    xt60 = next(p for p in parts if 'XT60' in p['fp'])
    xt_face = V((xt60['x1'] + xt60['x2']) / 2, xt60['y2'], z_pcb1 + XT60_H / 2)
    p5 = x9(*X9_P5)
    p5_pt = V(p5[0], p5[1], BASE_T + 12)
    fan = box(fx - FAN / 2, fy - FAN / 2, H - WALL - FAN_T, fx + FAN / 2, fy + FAN / 2, H - WALL)
    lcd = {
        'pcb': mbox(0, 0, LCD_W, LCD_L, -LCD_STACK - LCD_PCB_T, -LCD_STACK),
        'glass': mbox(LCD_GLASS[0], LCD_GLASS[1], LCD_GLASS[2], LCD_GLASS[3], -LCD_STACK, 0),
        'aa': lbox(win_x - LCD_AA[3] / 2, win_x + LCD_AA[3] / 2, win_z - LCD_AA[2] / 2, win_z + LCD_AA[2] / 2,
                   0, 0.05),
        'back': mbox(6, 10, LCD_W - 6, 40, -LCD_T, -LCD_STACK - LCD_PCB_T),
    }
    lcd['pcb'] = lcd['pcb'].cut(fuse_all([cyl_y(*lcd_ap(px, py), 1.25, 0, REAR_T + 20) for px, py in LCD_HOLES]))
    keeps.append(('screen solder wires', mbox(7, 0, LCD_W - 7, 7, -LCD_T - LCD_WIRE_KEEP, -LCD_T)))
    t_y0 = TOUCH_SKIN                                     # sensing face against the recess bottom (TOUCH_SKIN from the outer surface)
    touch = box(TOUCH_X - TOUCH_W / 2, t_y0, tz - TOUCH_H / 2, TOUCH_X + TOUCH_W / 2, t_y0 + TOUCH_PCB_T,
                tz + TOUCH_H / 2)
    touch_ic = box(TOUCH_X - 1, t_y0 + TOUCH_PCB_T, tz - 2, TOUCH_X + 4, t_y0 + TOUCH_PCB_T + 1.2, tz + 2)
    keeps.append(('touch solder wires', box(TOUCH_X - TOUCH_W / 2, t_y0 + TOUCH_PCB_T, tz - TOUCH_H / 2,
                                TOUCH_X + TOUCH_W / 2, t_y0 + TOUCH_PCB_T + 5.0, tz + TOUCH_H / 2)))
    log('Enclosure %.0f x %.0f x %.0f; ORing main board X %.1f..%.1f; X9 X %.1f..%.1f Y %.1f..%.1f; fan center (%.0f, %.0f)'
        % (W, D, H, OR_X_RIGHT - bd['h'], OR_X_RIGHT, X9_X0, X9_X0 + X9_W, x9_y0, X9_Y_FRONT, fx, fy))
    log('X9 %d°: base plate holes %s; P5 input at (%.0f, %.0f), XT60 end face at (%.0f, %.0f), straight-line distance %.0f mm (recommended wire length >= %.0f mm)'
        % (X9_ROT, ', '.join('(%.1f, %.1f)' % h for h in x9_holes), p5[0], p5[1], xt_face.x, xt_face.y,
           (p5_pt - xt_face).Length, abs(p5_pt.x - xt_face.x) + abs(p5_pt.y - xt_face.y) + 40))
    hdr = lcd_ap(LCD_W / 2, 0)[0]
    log('Rear panel: USB-C X %s, Z %.1f; window center (%.1f, %.1f); touch (%.1f, %.1f); screen module %d°, pin header on the %s side'
        % (', '.join('%.1f' % ((p['x1'] + p['x2']) / 2) for p in usb), z_pcb1 + USB_BODY_H / 2, win_x, win_z,
           TOUCH_X, tz, rot, 'left' if hdr < win_x else 'right'))
    inner = box(WALL, REAR_T, BASE_T, W - WALL, D - WALL, H - WALL)
    keeps = [(n, k.common(inner)) for n, k in keeps]
    return dict(shell=shell, base=base, board=board, comps=comps, x9=x9_body, fan=fan, lcd=lcd,
                touch=dict(pcb=touch, ic=touch_ic), keeps=keeps)


# ---------------- Checks ----------------
def check(m):
    log('Interference check (overlap volume mm^3, listed only if > 0.01):')
    bad = 0
    lcd = Part.makeCompound(list(m['lcd'].values()))
    touch = Part.makeCompound(list(m['touch'].values()))
    solids = [('main board', m['board']), ('X9', m['x9']), ('fan', m['fan']), ('screen', lcd), ('touch', touch)]
    solids += list(m['comps'].items())
    keeps = m['keeps']
    pairs = [('top shell', m['shell'], 'base plate', m['base'])]
    for n, s in solids + keeps:
        pairs += [('top shell', m['shell'], n, s), ('base plate', m['base'], n, s)]
    movers = [('X9', m['x9']), ('fan', m['fan']), ('screen', lcd), ('touch', touch)]
    for i, (na, a) in enumerate(movers):                  # modules against each other, modules against main board parts
        for nb, b in movers[i + 1:] + [('main board', m['board'])] + list(m['comps'].items()):
            pairs.append((na, a, nb, b))
    for nk, k in keeps:                                   # keep-out zones are checked only against other modules, not against their own module, main board parts or each other
        for nb, b in movers:
            if not nk.startswith(nb):
                pairs.append((nk, k, nb, b))
        if nk.startswith('X9'):                           # X9 connectors (face the main board when rotated 180°) are additionally checked against the main board and parts
            pairs += [(nk, k, 'main board', m['board'])] + [(nk, k, nb, b) for nb, b in m['comps'].items()]
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
    for name, s in (('screen', lcd), ('fan', m['fan']), ('X9', m['x9'])):
        log('  %s minimum distance to main board parts %.2f mm' % (name, s.distToShape(comps)[0]))
    log('  %d interference(s) in total' % bad)


# ---------------- Output ----------------
def print_poses(pieces):
    out = []
    for name, shp in pieces:
        s = shp.copy()
        if name == 'shell':
            s.rotate(V(0, 0, 0), V(1, 0, 0), 180)          # top face down
        bb = s.BoundBox
        out.append((name, s.translated(V(-bb.XMin, -bb.YMin, -bb.ZMin))))
    return out


def overhang_report(name, shp, limit_deg=45.0):
    lim = math.cos(math.radians(limit_deg))
    z_bed = shp.BoundBox.ZMin + 0.6
    total, items = 0.0, []
    for f in shp.Faces:
        if f.BoundBox.ZMax < z_bed:
            continue
        area = 0.0
        if f.Surface.__class__.__name__ == 'Plane':
            if f.normalAt(*f.ParameterRange[::2]).z < -lim - 0.01:
                area = f.Area
        else:
            pts, tris = f.tessellate(0.3)
            for t in tris:
                p0, p1, p2 = pts[t[0]], pts[t[1]], pts[t[2]]
                c = (p0 + p1 + p2) * (1.0 / 3)
                if c.z < z_bed:
                    continue
                try:
                    n = f.normalAt(*f.Surface.parameter(c))
                except Exception:                           # noqa: BLE001
                    continue
                if n.z < -lim - 0.01:
                    area += (p1 - p0).cross(p2 - p0).Length / 2
        if area > 0.05:
            items.append((area, f.BoundBox))
        total += area
    items.sort(key=lambda t: -t[0])
    log('  %s: %.1f mm², %d face(s)' % (name, total, len(items)))
    for area, bb in items[:8]:
        log('    %.1f mm²  %.1f x %.1f x %.1f  @ (%.1f, %.1f, %.1f)'
            % (area, bb.XLength, bb.YLength, bb.ZLength, bb.Center.x, bb.Center.y, bb.ZMin))


def mesh_out(shape, path):
    import MeshPart
    MeshPart.meshFromShape(Shape=shape, LinearDeflection=0.03, AngularDeflection=math.radians(8),
                           Relative=False).write(path)


def export(m, variant):
    od = os.path.join(OUT, variant)
    os.makedirs(os.path.join(od, 'print'), exist_ok=True)
    pieces = [('shell', m['shell']), ('base', m['base'])]
    for name, shp in pieces:
        shp.exportStep(os.path.join(od, name + '.step'))
    prints = print_poses(pieces)
    log('Print overhang check (normal facing down, angle to horizontal < 45°, excluding faces within 0.6 mm of the bed):')
    for i, (name, shp) in enumerate(prints, 1):
        mesh_out(shp, os.path.join(od, 'print', '%d_%s.stl' % (i, name)))
        overhang_report(name, shp)
    ref = [m['board'], m['x9'], m['fan']] + list(m['comps'].values()) + list(m['lcd'].values()) \
        + list(m['touch'].values())
    Part.makeCompound([m['shell'], m['base']] + ref).exportStep(os.path.join(od, 'assembly.step'))

    doc = App.newDocument('ORing_ATX_PSU_' + variant)
    gui = App.GuiUp

    def add(name, shape, color, transp=0):
        o = doc.addObject('Part::Feature', name)
        o.Shape = shape
        if gui:
            o.ViewObject.ShapeColor = color
            o.ViewObject.Transparency = transp
        return o

    add('Shell', m['shell'], COLOR_COVER)
    add('Base', m['base'], COLOR_BASE)
    add('ORing_PCB', m['board'], (0.05, 0.40, 0.15))
    add('ORing_Parts', Part.makeCompound(list(m['comps'].values())), (0.75, 0.70, 0.55))
    add('X9_ATX_500', m['x9'], (0.55, 0.55, 0.58))
    add('X9_Connectors', Part.makeCompound([k for n, k in m['keeps'] if n.startswith('X9')]), (0.95, 0.75, 0.10), 55)
    add('Fan_120', m['fan'], (0.15, 0.15, 0.17), 40)
    add('LCD_PCB', m['lcd']['pcb'], (0.10, 0.25, 0.70))
    add('LCD_Glass', m['lcd']['glass'], (0.05, 0.05, 0.05))
    add('LCD_ActiveArea', m['lcd']['aa'], (0.20, 0.45, 0.90))
    add('LCD_BackParts', m['lcd']['back'], (0.6, 0.6, 0.6))
    add('TTP223', m['touch']['pcb'].fuse(m['touch']['ic']), (0.80, 0.10, 0.10))
    doc.recompute()
    doc.saveAs(os.path.join(HERE, 'ORing_ATX_PSU_%s.FCStd' % variant))
    return doc, prints, od


def gui_shots(doc, prints, od):
    import FreeCADGui as Gui
    view = Gui.getDocument(doc.Name).ActiveView
    view.setCameraType('Orthographic')                    # a perspective camera gets clipped by the near plane after fitAll

    def cam(dx, dy, dz):
        z = V(dx, dy, dz).normalize()
        x = V(0, 0, 1).cross(z).normalize()
        y = z.cross(x)
        return App.Placement(App.Matrix(x.x, y.x, z.x, 0, x.y, y.y, z.y, 0, x.z, y.z, z.z, 0, 0, 0, 0, 1)).Rotation

    def shot(name, d, hide=(), shell_transp=0):
        for o in doc.Objects:
            o.ViewObject.Visibility = o.Name not in hide
        doc.getObject('Shell').ViewObject.Transparency = shell_transp
        view.setCameraOrientation(cam(*d), True)
        view.fitAll()
        Gui.updateGui()
        view.fitAll()
        view.saveImage(os.path.join(od, name), 1600, 1100, 'White')

    shot('gui_rear.png', (-0.7, -1.6, 0.8))                 # warm-up: the first shot may be blank
    shot('gui_rear.png', (-0.7, -1.6, 0.8))
    shot('gui_front_top.png', (0.9, 1.4, 1.4))
    shot('gui_bottom.png', (0.6, 0.8, -1.3))
    shot('gui_inside.png', (-0.9, -1.2, 1.4), ('LCD_ActiveArea',), 80)
    shot('gui_inside_noshell.png', (0.9, -1.0, 1.4), ('Shell', 'LCD_ActiveArea', 'Fan_120'))
    for o in doc.Objects:
        o.ViewObject.Visibility = o.Name != 'LCD_ActiveArea'
    doc.getObject('Shell').ViewObject.Transparency = 0
    view.setCameraOrientation(cam(-0.7, -1.6, 0.8), True)
    view.fitAll()
    doc.save()

    lay = App.newDocument('PrintLayout')
    x = 0.0
    for (name, shp), col in zip(prints, (COLOR_COVER, COLOR_BASE)):
        o = lay.addObject('Part::Feature', name)
        o.Shape = shp.translated(V(x - shp.BoundBox.XMin, 0, 0))
        o.ViewObject.ShapeColor = col
        x += shp.BoundBox.XLength + 15
    lay.recompute()
    lview = Gui.getDocument(lay.Name).ActiveView
    lview.setCameraType('Orthographic')
    lview.setCameraOrientation(cam(0.6, -1.3, 1.0), True)
    lview.fitAll()
    Gui.updateGui()
    lview.saveImage(os.path.join(od, 'gui_print_layout.png'), 1600, 1100, 'White')
    App.closeDocument(lay.Name)


def run_gui(jobs):
    for doc, prints, od in jobs:
        gui_shots(doc, prints, od)
    log('GUI screenshots: <variant>/gui_*.png')
    write_log()
    if os.environ.get('ENCL_QUIT') == '1':
        os._exit(0)


def clean_stale():
    """Remove outputs left in the out/ root from the single-variant era (now one subfolder per variant)."""
    old = [os.path.join(OUT, n) for n in ('shell.step', 'base.step', 'assembly.step')]
    old += [os.path.join(OUT, 'print', n) for n in ('1_shell.stl', '2_base.stl')]
    if os.path.isdir(OUT):
        old += [os.path.join(OUT, n) for n in os.listdir(OUT) if n.startswith('gui_')]
    old.append(os.path.join(HERE, 'ORing_ATX_PSU.FCStd'))
    for f in old:
        if os.path.isfile(f):
            os.remove(f)
    pdir = os.path.join(OUT, 'print')
    if os.path.isdir(pdir) and not os.listdir(pdir):
        os.rmdir(pdir)


def main():
    clean_stale()
    jobs = []
    for variant, rot in VARIANTS:
        log('==== %s (screen module %d°) ====' % (variant, rot))
        m = build(rot)
        check(m)
        jobs.append(export(m, variant))
    log('Output:', OUT)
    write_log()
    if App.GuiUp:
        from PySide import QtCore
        QtCore.QTimer.singleShot(1500, lambda: run_gui(jobs))


main()
