"""Neutral base render: scene-linear (Rec.2020 primaries, 0.18 = mid grey) -> Rec.709 code values (gamma 2.4).
The same curve table is embedded in the C++ plugin."""
import numpy as np
import colour
from scipy.interpolate import PchipInterpolator

# stops relative to 18% grey -> Rec.709 (BT.1886 gamma 2.4) code value
CURVE_POINTS = [(-8.0, 0.000), (-6.5, 0.020), (-5.0, 0.065), (-3.5, 0.150), (-2.0, 0.262), (-1.0, 0.345),
                (0.0, 0.430), (1.0, 0.520), (2.0, 0.612), (3.0, 0.705), (4.0, 0.790), (5.0, 0.865),
                (6.0, 0.925), (7.0, 0.965), (8.5, 0.990), (10.0, 1.000)]
STOP_MIN, STOP_MAX, CURVE_N = -10.0, 12.0, 4096
_x, _y = zip(*CURVE_POINTS)
_pchip = PchipInterpolator(_x, _y, extrapolate=False)
CURVE_STOPS = np.linspace(STOP_MIN, STOP_MAX, CURVE_N)
CURVE_TABLE = np.nan_to_num(_pchip(CURVE_STOPS), nan=0.0)
CURVE_TABLE[CURVE_STOPS > _x[-1]] = 1.0
CURVE_TABLE[CURVE_STOPS < _x[0]] = 0.0

M_2020_TO_709 = colour.matrix_RGB_to_RGB(colour.RGB_COLOURSPACES['ITU-R BT.2020'], colour.RGB_COLOURSPACES['ITU-R BT.709'])
M_709_TO_2020 = colour.matrix_RGB_to_RGB(colour.RGB_COLOURSPACES['ITU-R BT.709'], colour.RGB_COLOURSPACES['ITU-R BT.2020'])
Y2020 = np.array([0.2627, 0.6780, 0.0593])
Y709 = np.array([0.2126, 0.7152, 0.0722])


def curve(stops):
    t = (np.clip(stops, STOP_MIN, STOP_MAX) - STOP_MIN) / (STOP_MAX - STOP_MIN) * (CURVE_N - 1)
    i = np.minimum(np.floor(t).astype(int), CURVE_N - 2)
    f = t - i
    return CURVE_TABLE[i] * (1 - f) + CURVE_TABLE[i + 1] * f


def desat_to_nonneg(rgb, wY):
    """Pull colours with a negative channel toward their luminance until min channel = 0."""
    Y = rgb @ wY
    mn = rgb.min(axis=-1)
    out = rgb.copy()
    neg = mn < 0
    Yn = np.maximum(Y[neg], 0.0)
    t = np.where(Yn > 0, Yn / np.maximum(Yn - mn[neg], 1e-12), 0.0)
    out[neg] = Yn[:, None] + t[:, None] * (rgb[neg] - Yn[:, None])
    out[neg][Yn <= 0] = 0.0
    return np.clip(out, 0.0, None)


def base_render(lin2020, exposure=0.0, contrast=1.0):
    x = lin2020 * (2.0 ** exposure)
    x = desat_to_nonneg(x, Y2020)
    stops = np.log2(np.maximum(x, 1e-10) / 0.18) * contrast
    code = curve(stops)
    code = np.where(x <= 0, 0.0, code)
    disp = code ** 2.4
    d709 = disp @ M_2020_TO_709.T
    d709 = desat_to_nonneg(d709, Y709)
    # keep within display range by desaturating toward luminance, not clipping per channel
    mx = d709.max(axis=-1)
    Y = np.clip(d709 @ Y709, 0, 1)
    over = mx > 1.0
    t = np.where(over, (1.0 - Y) / np.maximum(mx - Y, 1e-12), 1.0)
    d709 = np.where(over[..., None], Y[..., None] + t[..., None] * (d709 - Y[..., None]), d709)
    return np.clip(d709, 0, 1) ** (1 / 2.4)
