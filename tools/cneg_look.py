"""Cinematic Neg look: a Fujifilm Classic Negative + recipe emulation as a colour transform.

Input and output are display-referred sRGB values in [0, 1] (what a .cube LUT sees).

Recipe being emulated (Udo Wuerth, "Cinematic Neg"):
  Film simulation Classic Negative, Grain off, Color Chrome Effect Strong,
  Color Chrome FX Blue Weak, WB Auto R+1 B-5, DR400, Tone curve H-2 S-1,
  Color 0, Sharpness -2, High ISO NR -4, Clarity -2, Exposure comp 0 to +1.

Pipeline
  1. sRGB -> linear -> OkLab.
  2. Hue-dependent hue shift / chroma scale / lightness (smooth periodic splines over hue),
     global desaturation, Color Chrome (darker, denser saturated colours; weaker on blues).
  3. Gamut map back into sRGB by reducing chroma at constant lightness and hue.
  4. Per-channel display-space tone curves: soft "hard-tonality" midtones, deep blacks,
     compressed highlights (DR400 + H-2), green-tinted shadows and mids, cream highlights
     (Classic Neg crossover + the R+1/B-5 white balance shift).
"""
import numpy as np
from scipy.interpolate import PchipInterpolator, CubicSpline

# ----------------------------------------------------------------------------- colour maths
_M1 = np.array([[0.4122214708, 0.5363325363, 0.0514459929],
                [0.2119034982, 0.6806995451, 0.1073969566],
                [0.0883024619, 0.2817188376, 0.6299787005]])
_M2 = np.array([[0.2104542553, 0.7936177850, -0.0040720468],
                [1.9779984951, -2.4285922050, 0.4505937099],
                [0.0259040371, 0.7827717662, -0.8086757660]])
_M1i = np.linalg.inv(_M1)
_M2i = np.linalg.inv(_M2)


def srgb_to_lin(x):
    x = np.asarray(x, dtype=np.float64)
    return np.where(x <= 0.04045, x / 12.92, ((np.clip(x, 0, None) + 0.055) / 1.055) ** 2.4)


def lin_to_srgb(x):
    x = np.asarray(x, dtype=np.float64)
    xc = np.clip(x, 0, None)
    return np.where(xc <= 0.0031308, 12.92 * xc, 1.055 * xc ** (1 / 2.4) - 0.055)


def lin_to_oklab(rgb):
    lms = rgb @ _M1.T
    return np.cbrt(lms) @ _M2.T


def oklab_to_lin(lab):
    lms_ = lab @ _M2i.T
    return (lms_ ** 3) @ _M1i.T


def smoothstep(x, e0, e1):
    t = np.clip((x - e0) / (e1 - e0), 0.0, 1.0)
    return t * t * (3 - 2 * t)


# ----------------------------------------------------------------------------- parameters
# Hue nodes are OkLCh hue angles in degrees.
#   ~20 crimson, 29 red, 45-60 orange/skin, 90-101 gold/yellow, 127-142 foliage/green,
#   ~187 teal, ~207 cyan, 250-257 sky, 264 blue, 303 purple, 346 magenta.
DEFAULT = dict(
    global_sat=0.86,
    # White balance shift R+1 / B-5, as linear channel gains
    wb=(1.0, 0.98, 0.863),
    # node hue: (hue shift deg, chroma scale, color-chrome strength, tone-dependent hue shift,
    #            high-chroma compression k)
    # tone-dependent shift is applied with +1 in shadows and -1 in highlights
    nodes={
        20:  (-2.0, 1.02, 0.15, -4.0, 0.3),   # crimson: deep; magenta-ish in shadow, orange in light
        35:  ( 0.0, 1.05, 0.16, -4.0, 0.3),   # red-orange (stickers, signs): rich and dense
        55:  ( 1.0, 1.02, 0.12, -2.0, 0.3),   # orange / skin
        75:  (-1.0, 1.04, 0.10,  0.0, 0.5),   # amber / peach (sunlit surfaces)
        100: (-5.0, 0.96, 0.10,  0.0, 0.6),   # yellow -> golden, keeps some punch
        125: ( 2.0, 0.70, 0.10, 10.0, 1.2),   # yellow-green: olive in light, teal in shadow
        150: ( 8.0, 0.52, 0.12, 12.0, 1.5),   # green -> muted teal-green
        185: ( 4.0, 0.70, 0.08,  4.0, 2.0),   # teal
        210: (-2.0, 0.80, 0.05,  0.0, 2.0),   # cyan
        235: (-8.0, 0.76, 0.03,  0.0, 2.5),   # azure
        255: (-14.0, 0.74, 0.04, 0.0, 3.0),   # sky blue -> cyan-azure (Classic Neg skies)
        268: (-8.0, 0.84, 0.06,  0.0, 2.5),   # blue -> deep navy (FX Blue weak)
        300: ( 6.0, 0.72, 0.06,  0.0, 1.5),   # purple
        335: ( 8.0, 0.80, 0.08,  0.0, 1.0),   # magenta -> toward red
    },
    tone_hue_pivot=(0.60, 0.25),    # t = clip((pivot - L) / span, -1, 1)
    chroma_gate=(0.012, 0.045),     # below this OkLab chroma, saturation changes fade out
    hue_gate=(0.02, 0.085),         # hue shifts fade out for pale colours (keeps pale skies azure)
    compress_from=0.05,             # chroma above which the per-hue compression k acts
    chrome_range=(0.06, 0.20),      # chroma range over which Color Chrome darkening ramps in
    hi_desat=0.30,                  # extra desaturation in the brightest tones
    lo_desat=0.20,                  # extra desaturation in the deepest shadows
    # Master tone curve in display space (x -> y)
    master=[(0.00, 0.010), (0.05, 0.034), (0.10, 0.072), (0.20, 0.166), (0.35, 0.331),
            (0.50, 0.502), (0.65, 0.668), (0.80, 0.812), (0.90, 0.882), (0.96, 0.921), (1.00, 0.945)],
    # Neutral-axis targets (8-bit): input grey -> output RGB. Per-channel curve offsets are
    # derived from these after the WB stage, so greys land exactly here (green-tinted
    # shadows/mids, cream highlights, as measured on the reference photos).
    neutral=[(0, (1.5, 4.0, 2.0)), (8, (4.5, 9.0, 5.0)), (16, (9.0, 15.0, 10.0)), (32, (22.0, 29.0, 23.5)),
             (64, (53.0, 61.0, 56.0)), (96, (89.0, 97.0, 92.0)), (128, (126.0, 133.0, 128.0)),
             (160, (163.0, 168.0, 163.0)), (192, (198.0, 200.0, 192.0)), (224, (226.0, 223.0, 212.0)),
             (240, (238.0, 233.0, 220.0)), (255, (247.0, 240.0, 227.0))],
    curve_strength=1.0,
)


def _periodic_spline(nodes, index):
    hs = sorted(nodes)
    xs = np.array(hs + [hs[0] + 360.0])
    ys = np.array([nodes[h][index] for h in hs] + [nodes[hs[0]][index]])
    return CubicSpline(xs, ys, bc_type='periodic'), hs[0]


def _eval_periodic(spl_and_start, h):
    spl, start = spl_and_start
    hh = (h - start) % 360.0 + start
    return spl(hh)


class Look:
    def __init__(self, params=None):
        p = dict(DEFAULT)
        if params:
            p.update(params)
        self.p = p
        self.s_dh = _periodic_spline(p['nodes'], 0)
        self.s_sat = _periodic_spline(p['nodes'], 1)
        self.s_cc = _periodic_spline(p['nodes'], 2)
        self.s_dht = _periodic_spline(p['nodes'], 3)
        self.s_k = _periodic_spline(p['nodes'], 4)
        mx, my = zip(*p['master'])
        self.master = PchipInterpolator(mx, my)
        self._derive_offsets()

    def _derive_offsets(self):
        # Push neutral greys through the colour stage, then solve per-channel offsets so the
        # tone stage maps them onto the neutral targets.
        p = self.p
        g = np.array([v for v, _ in p['neutral']], dtype=np.float64) / 255.0
        tgt = np.array([t for _, t in p['neutral']], dtype=np.float64)
        x = self.color(np.repeat(g[:, None], 3, axis=1))
        self.offs = []
        for c in range(3):
            xc = x[:, c]
            off = tgt[:, c] - self.master(xc) * 255.0
            xs, idx = np.unique(np.round(xc, 9), return_index=True)
            xs = np.concatenate([[0.0], xs, [1.0]]) if xs[0] > 0 else np.concatenate([xs, [1.0]])
            offv = off[idx]
            offv = np.concatenate([[offv[0]], offv, [offv[-1]]]) if len(offv) + 2 == len(xs) else np.concatenate([offv, [offv[-1]]])
            # drop duplicates created by clipping at 1.0
            keep = np.concatenate([[True], np.diff(xs) > 1e-6])
            self.offs.append(PchipInterpolator(xs[keep], offv[keep]))

    # -- stage 1-3: colour in OkLab ------------------------------------------------------
    def color(self, rgb):
        p = self.p
        lin = srgb_to_lin(rgb) * np.array(p['wb'])
        lin = np.clip(lin, 0.0, 1.0)
        lab = lin_to_oklab(lin)
        L, a, b = lab[..., 0], lab[..., 1], lab[..., 2]
        C = np.hypot(a, b)
        h = np.degrees(np.arctan2(b, a)) % 360.0

        gate = smoothstep(C, *p['chroma_gate'])
        piv, span = p['tone_hue_pivot']
        t = np.clip((piv - L) / span, -1.0, 1.0)
        hgate = smoothstep(C, *p['hue_gate'])
        dh = (_eval_periodic(self.s_dh, h) + _eval_periodic(self.s_dht, h) * t) * hgate
        sat = 1.0 + (_eval_periodic(self.s_sat, h) - 1.0) * gate
        cc = _eval_periodic(self.s_cc, h)

        h2 = h + dh
        k = np.clip(_eval_periodic(self.s_k, h), 0.0, None)
        C2 = C * sat * p['global_sat'] / (1.0 + k * np.clip(C - p['compress_from'], 0.0, None))
        # film-like desaturation at the extremes of the tonal range
        d_yel = (h - 100.0 + 180.0) % 360.0 - 180.0
        w_yel = np.where(np.abs(d_yel) < 45.0, 0.5 * (1 + np.cos(np.pi * d_yel / 45.0)), 0.0)
        C2 *= 1.0 - p['hi_desat'] * smoothstep(L, 0.86, 1.0) * (1.0 - 0.7 * w_yel)
        C2 *= 1.0 - p['lo_desat'] * (1.0 - smoothstep(L, 0.12, 0.40))
        # Color Chrome Effect (Strong) / FX Blue (Weak): darker, denser saturated colours
        L2 = L * (1.0 - cc * smoothstep(C, *p['chrome_range']))

        hr = np.radians(h2)
        lab2 = np.stack([L2, C2 * np.cos(hr), C2 * np.sin(hr)], axis=-1)
        lin2 = self._gamut_map(lab2)
        return lin_to_srgb(np.clip(lin2, 0.0, 1.0))

    @staticmethod
    def _gamut_map(lab):
        lin = oklab_to_lin(lab)
        bad = (lin.min(axis=-1) < -1e-6) | (lin.max(axis=-1) > 1 + 1e-6)
        if not np.any(bad):
            return lin
        L = lab[..., 0][bad]
        ab = lab[..., 1:][bad]
        lo = np.zeros(L.shape)
        hi = np.ones(L.shape)
        for _ in range(18):
            mid = (lo + hi) / 2
            t = oklab_to_lin(np.concatenate([L[:, None], ab * mid[:, None]], axis=-1))
            ok = (t.min(axis=-1) >= -1e-6) & (t.max(axis=-1) <= 1 + 1e-6)
            lo = np.where(ok, mid, lo)
            hi = np.where(ok, hi, mid)
        out = lin.copy()
        out[bad] = oklab_to_lin(np.concatenate([L[:, None], ab * lo[:, None]], axis=-1))
        return out

    # -- stage 4: tone ------------------------------------------------------------------
    def tone(self, rgb):
        k = self.p['curve_strength']
        out = np.empty_like(rgb)
        for c in range(3):
            x = rgb[..., c]
            y = self.master(x) + self.offs[c](x) / 255.0
            out[..., c] = x + k * (y - x)
        return np.clip(out, 0.0, 1.0)

    def __call__(self, rgb):
        rgb = np.clip(np.asarray(rgb, dtype=np.float64), 0.0, 1.0)
        return self.tone(self.color(rgb))


# ----------------------------------------------------------------------------- LUT helpers
def lut_grid(n):
    """Grid in .cube order: red fastest, then green, then blue. Returns (n^3, 3)."""
    v = np.linspace(0.0, 1.0, n)
    b, g, r = np.meshgrid(v, v, v, indexing='ij')
    return np.stack([r.ravel(), g.ravel(), b.ravel()], axis=-1)


def build_lut(look, n):
    grid = lut_grid(n)
    out = look(grid)
    return out.reshape(n, n, n, 3)  # indexed [b, g, r]


def write_cube(path, lut, title):
    n = lut.shape[0]
    with open(path, 'w', newline='\n') as f:
        f.write(f'TITLE "{title}"\n')
        f.write('# Fujifilm Classic Negative "Cinematic Neg" recipe emulation\n')
        f.write('# Input/output: sRGB / Rec.709 display-referred (gamma-encoded) values\n')
        f.write(f'LUT_3D_SIZE {n}\n')
        f.write('DOMAIN_MIN 0.0 0.0 0.0\nDOMAIN_MAX 1.0 1.0 1.0\n\n')
        flat = lut.reshape(-1, 3)
        for r, g, b in flat:
            f.write(f'{r:.6f} {g:.6f} {b:.6f}\n')


def read_cube(path):
    n = None
    vals = []
    with open(path) as f:
        for line in f:
            s = line.strip()
            if not s or s.startswith('#'):
                continue
            if s.startswith('LUT_3D_SIZE'):
                n = int(s.split()[1])
                continue
            if s[0].isalpha():
                continue
            vals.append([float(t) for t in s.split()])
    arr = np.array(vals)
    assert n is not None and arr.shape == (n ** 3, 3), (n, arr.shape)
    return arr.reshape(n, n, n, 3)


def apply_lut(lut, rgb):
    """Tetrahedral interpolation of an [b,g,r]-indexed LUT on rgb in [0,1]."""
    n = lut.shape[0]
    x = np.clip(rgb, 0.0, 1.0) * (n - 1)
    i = np.minimum(np.floor(x).astype(np.int64), n - 2)
    f = x - i
    r0, g0, b0 = i[..., 0], i[..., 1], i[..., 2]
    fr, fg, fb = f[..., 0], f[..., 1], f[..., 2]

    def V(dr, dg, db):
        return lut[b0 + db, g0 + dg, r0 + dr]

    c000 = V(0, 0, 0); c111 = V(1, 1, 1)
    out = np.zeros(rgb.shape)
    conds = [
        (fr >= fg) & (fg >= fb),
        (fr >= fb) & (fb > fg),
        (fb > fr) & (fr >= fg),
        (fg > fr) & (fr >= fb),
        (fg >= fb) & (fb > fr),
        (fb > fg) & (fg > fr),
    ]
    # each case: (first axis, second axis) ordering of fractional parts
    for cond, (A, B) in zip(conds, [((1, 0, 0), (1, 1, 0)), ((1, 0, 0), (1, 0, 1)), ((0, 0, 1), (1, 0, 1)),
                                    ((0, 1, 0), (1, 1, 0)), ((0, 1, 0), (0, 1, 1)), ((0, 0, 1), (0, 1, 1))]):
        if not np.any(cond):
            continue
        fa = np.stack([fr, fg, fb], -1)
        # largest, middle, smallest fractions in this tetrahedron
        order = {((1, 0, 0), (1, 1, 0)): (0, 1, 2), ((1, 0, 0), (1, 0, 1)): (0, 2, 1),
                 ((0, 0, 1), (1, 0, 1)): (2, 0, 1), ((0, 1, 0), (1, 1, 0)): (1, 0, 2),
                 ((0, 1, 0), (0, 1, 1)): (1, 2, 0), ((0, 0, 1), (0, 1, 1)): (2, 1, 0)}[(A, B)]
        t1 = fa[..., order[0]][cond][:, None]
        t2 = fa[..., order[1]][cond][:, None]
        t3 = fa[..., order[2]][cond][:, None]
        cA = V(*A)[cond]; cB = V(*B)[cond]
        out[cond] = (1 - t1) * c000[cond] + (t1 - t2) * cA + (t2 - t3) * cB + t3 * c111[cond]
    return out
