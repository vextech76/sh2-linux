#include "gs/rasterizer.h"

#include <algorithm>
#include <cmath>

namespace lantern::gs {
namespace {
constexpr uint64_t bits(uint64_t v, unsigned lo, unsigned n) { return (v >> lo) & ((1ull << n) - 1); }
constexpr int clamp255(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

uint32_t expand16(uint32_t c, uint64_t texa) {
  const uint32_t r = (c & 0x1F) << 3, g = ((c >> 5) & 0x1F) << 3, b = ((c >> 10) & 0x1F) << 3;
  const bool aem = bits(texa, 15, 1);
  uint32_t a = (c & 0x8000) ? static_cast<uint32_t>(bits(texa, 32, 8)) : static_cast<uint32_t>(bits(texa, 0, 8));
  if (aem && (c & 0x7FFF) == 0) a = 0;
  return r | (g << 8) | (b << 16) | (a << 24);
}

uint32_t wrap(int c, uint32_t size, uint32_t mode, uint32_t minv, uint32_t maxv) {
  switch (mode) {
    case 0: return static_cast<uint32_t>(c) & (size - 1);                                // repeat
    case 1: return static_cast<uint32_t>(std::clamp(c, 0, static_cast<int>(size) - 1));  // clamp
    case 2: return static_cast<uint32_t>(std::clamp(c, static_cast<int>(minv), static_cast<int>(maxv)));
    default: return (static_cast<uint32_t>(c) & minv) | maxv;  // region repeat
  }
}
}  // namespace

struct Rasterizer::Attr {
  float r, g, b, a, z, s, t, q, u, v, f;
};

void Rasterizer::shade(Gs& gs, int x, int y, const Attr& at) {
  const uint64_t prim = gs.reg(0x00);
  const uint64_t mode = (gs.reg(0x1A) & 1) ? prim : gs.reg(0x1B);  // PRMODECONT.AC
  const int ctx = static_cast<int>(bits(mode, 9, 1));
  const uint64_t frame = gs.reg(static_cast<uint8_t>(0x4C + ctx)), zbuf = gs.reg(static_cast<uint8_t>(0x4E + ctx)),
                 test = gs.reg(static_cast<uint8_t>(0x47 + ctx)), alpha = gs.reg(static_cast<uint8_t>(0x42 + ctx)),
                 tex0 = gs.reg(static_cast<uint8_t>(0x06 + ctx)), clampr = gs.reg(static_cast<uint8_t>(0x08 + ctx)),
                 fba = gs.reg(static_cast<uint8_t>(0x4A + ctx));
  ++stats_.pixels_tested;
  int r = static_cast<int>(at.r), g = static_cast<int>(at.g), b = static_cast<int>(at.b), a = static_cast<int>(at.a);

  // ---- texture ----
  if (bits(mode, 4, 1)) {
    const uint32_t tbp = static_cast<uint32_t>(bits(tex0, 0, 14)), tbw = static_cast<uint32_t>(bits(tex0, 14, 6)),
                   psm = static_cast<uint32_t>(bits(tex0, 20, 6));
    const uint32_t tw = 1u << std::min<uint32_t>(10, static_cast<uint32_t>(bits(tex0, 26, 4))),
                   th = 1u << std::min<uint32_t>(10, static_cast<uint32_t>(bits(tex0, 30, 4)));
    float fu, fv;
    if (bits(mode, 8, 1)) {  // FST: UV in 1/16 texel units
      fu = at.u / 16.0f;
      fv = at.v / 16.0f;
    } else {
      const float q = at.q != 0.0f ? at.q : 1.0f;
      fu = at.s / q * static_cast<float>(tw);
      fv = at.t / q * static_cast<float>(th);
    }
    const uint64_t texa = gs.reg(0x3B);
    const uint32_t wms = static_cast<uint32_t>(bits(clampr, 0, 2)), wmt = static_cast<uint32_t>(bits(clampr, 2, 2));
    const uint32_t minu = static_cast<uint32_t>(bits(clampr, 4, 10)),
                   maxu = static_cast<uint32_t>(bits(clampr, 14, 10));
    const uint32_t minv = static_cast<uint32_t>(bits(clampr, 24, 10)),
                   maxv = static_cast<uint32_t>(bits(clampr, 34, 10));
    auto fetch = [&](int iu, int iv) -> uint32_t {
      const uint32_t tu = wrap(iu, tw, wms, minu, maxu), tv = wrap(iv, th, wmt, minv, maxv);
      switch (psm) {
        case kCt32: return gs.mem().read_pixel(kCt32, tbp, tbw, tu, tv);
        case kCt24: {
          const uint32_t rgb = gs.mem().read_pixel(kCt24, tbp, tbw, tu, tv);
          const bool aem = bits(texa, 15, 1);
          return rgb | ((aem && rgb == 0) ? 0 : static_cast<uint32_t>(bits(texa, 0, 8)) << 24);
        }
        case kCt16:
        case kCt16s: return expand16(gs.mem().read_pixel(psm, tbp, tbw, tu, tv), texa);
        default: {
          const uint32_t idx = gs.mem().read_index(psm, tbp, tbw, tu, tv);
          return gs.clut_is16() ? expand16(gs.clut(idx), texa) : gs.clut(idx);
        }
      }
    };
    // TEX1.MMAG (bit 5): 0 = nearest, 1 = bilinear (texel centres at +0.5). Minification and
    // mipmapping are not modelled yet; the magnification filter is applied throughout.
    const uint64_t tex1 = gs.reg(static_cast<uint8_t>(0x14 + ctx));
    uint32_t c;
    if (bits(tex1, 5, 1)) {
      const float bu = fu - 0.5f, bv = fv - 0.5f;
      const int iu = static_cast<int>(std::floor(bu)), iv = static_cast<int>(std::floor(bv));
      const float au = bu - static_cast<float>(iu), av = bv - static_cast<float>(iv);
      const uint32_t c00 = fetch(iu, iv), c10 = fetch(iu + 1, iv), c01 = fetch(iu, iv + 1), c11 = fetch(iu + 1, iv + 1);
      c = 0;
      for (int k = 0; k < 4; ++k) {
        auto ch = [&](uint32_t v) { return static_cast<float>((v >> (8 * k)) & 0xFF); };
        const float top = ch(c00) + (ch(c10) - ch(c00)) * au, bot = ch(c01) + (ch(c11) - ch(c01)) * au;
        c |= static_cast<uint32_t>(std::lround(top + (bot - top) * av)) << (8 * k);
      }
    } else {
      c = fetch(static_cast<int>(std::floor(fu)), static_cast<int>(std::floor(fv)));
    }
    const int tr = c & 0xFF, tg = (c >> 8) & 0xFF, tb = (c >> 16) & 0xFF, ta = (c >> 24) & 0xFF;
    const bool tcc = bits(tex0, 34, 1);
    switch (bits(tex0, 35, 2)) {
      case 0:  // MODULATE
        r = clamp255((tr * r) >> 7);
        g = clamp255((tg * g) >> 7);
        b = clamp255((tb * b) >> 7);
        if (tcc) a = clamp255((ta * a) >> 7);
        break;
      case 1:  // DECAL
        r = tr;
        g = tg;
        b = tb;
        if (tcc) a = ta;
        break;
      default: {  // HIGHLIGHT / HIGHLIGHT2
        const bool hl2 = bits(tex0, 35, 2) == 3;
        r = clamp255(((tr * r) >> 7) + a);
        g = clamp255(((tg * g) >> 7) + a);
        b = clamp255(((tb * b) >> 7) + a);
        if (tcc) a = hl2 ? ta : clamp255(ta + a);
        break;
      }
    }
  }
  // ---- fog ----
  if (bits(mode, 5, 1)) {
    const uint64_t fc = gs.reg(0x3D);
    const int f = static_cast<int>(at.f);
    r = (f * r + (255 - f) * static_cast<int>(bits(fc, 0, 8))) >> 8;
    g = (f * g + (255 - f) * static_cast<int>(bits(fc, 8, 8))) >> 8;
    b = (f * b + (255 - f) * static_cast<int>(bits(fc, 16, 8))) >> 8;
  }

  // ---- alpha test ----
  bool write_fb = true, write_z = !bits(zbuf, 32, 1), write_alpha = true;
  if (bits(test, 0, 1)) {
    const int aref = static_cast<int>(bits(test, 4, 8));
    bool pass;
    switch (bits(test, 1, 3)) {
      case 0: pass = false; break;
      case 1: pass = true; break;
      case 2: pass = a < aref; break;
      case 3: pass = a <= aref; break;
      case 4: pass = a == aref; break;
      case 5: pass = a >= aref; break;
      case 6: pass = a > aref; break;
      default: pass = a != aref; break;
    }
    if (!pass) {
      switch (bits(test, 12, 2)) {
        case 0: return;                   // KEEP
        case 1: write_z = false; break;   // FB_ONLY
        case 2: write_fb = false; break;  // ZB_ONLY
        default:
          write_z = false;
          write_alpha = false;
          break;  // RGB_ONLY
      }
    }
  }

  const uint32_t fbp = static_cast<uint32_t>(bits(frame, 0, 9)) * 32, fbw = static_cast<uint32_t>(bits(frame, 16, 6)),
                 fpsm = static_cast<uint32_t>(bits(frame, 24, 6));
  const uint32_t fbmsk = static_cast<uint32_t>(frame >> 32);
  const uint32_t ux = static_cast<uint32_t>(x), uy = static_cast<uint32_t>(y);

  // ---- depth test ----
  if (bits(test, 16, 1)) {
    const uint32_t zbp = static_cast<uint32_t>(bits(zbuf, 0, 9)) * 32;
    const uint32_t zpsm = static_cast<uint32_t>(bits(zbuf, 24, 4)) | 0x30;
    const uint32_t zmax = zpsm == kZ32 ? 0xFFFFFFFFu : zpsm == kZ24 ? 0xFFFFFFu : 0xFFFFu;
    const uint32_t z = static_cast<uint32_t>(std::min<double>(at.z, zmax));
    const uint32_t zold = gs.mem().read_pixel(zpsm, zbp, fbw, ux, uy);
    bool pass;
    switch (bits(test, 17, 2)) {
      case 0: pass = false; break;
      case 1: pass = true; break;
      case 2: pass = z >= zold; break;
      default: pass = z > zold; break;
    }
    if (!pass) return;
    if (write_z) gs.mem().write_pixel(zpsm, zbp, fbw, ux, uy, z);
  }
  if (!write_fb) return;

  // ---- blend ----
  const uint32_t old = gs.mem().read_pixel(fpsm, fbp, fbw, ux, uy);
  uint32_t dst = old;
  if (fpsm == kCt16 || fpsm == kCt16s) dst = expand16(old, 0x8000000080ull);
  const int dr = dst & 0xFF, dg = (dst >> 8) & 0xFF, db = (dst >> 16) & 0xFF, da = (dst >> 24) & 0xFF;
  const bool pabe = gs.reg(0x49) & 1;
  if (bits(mode, 6, 1) && !(pabe && a < 0x80)) {
    auto pick = [&](uint32_t sel, int s, int d) { return sel == 0 ? s : sel == 1 ? d : 0; };
    const uint32_t A = static_cast<uint32_t>(bits(alpha, 0, 2)), B = static_cast<uint32_t>(bits(alpha, 2, 2)),
                   C = static_cast<uint32_t>(bits(alpha, 4, 2)), D = static_cast<uint32_t>(bits(alpha, 6, 2));
    const int cf = C == 0 ? a : C == 1 ? da : static_cast<int>(bits(alpha, 32, 8));
    auto blend = [&](int s, int d) { return ((pick(A, s, d) - pick(B, s, d)) * cf >> 7) + pick(D, s, d); };
    r = blend(r, dr);
    g = blend(g, dg);
    b = blend(b, db);
  }
  if (gs.reg(0x46) & 1) {  // COLCLAMP
    r = clamp255(r);
    g = clamp255(g);
    b = clamp255(b);
  } else {
    r &= 0xFF;
    g &= 0xFF;
    b &= 0xFF;
  }
  if (fba & 1) a |= 0x80;
  if (!write_alpha) a = da;
  uint32_t out = static_cast<uint32_t>(r) | (static_cast<uint32_t>(g) << 8) | (static_cast<uint32_t>(b) << 16) |
                 (static_cast<uint32_t>(clamp255(a)) << 24);
  if (fpsm == kCt16 || fpsm == kCt16s) {
    out = (static_cast<uint32_t>(r) >> 3) | ((static_cast<uint32_t>(g) >> 3) << 5) |
          ((static_cast<uint32_t>(b) >> 3) << 10) | (a & 0x80 ? 0x8000u : 0);
    const uint32_t m16 =
        ((fbmsk >> 3) & 0x1F) | (((fbmsk >> 11) & 0x1F) << 5) | (((fbmsk >> 19) & 0x1F) << 10) | ((fbmsk >> 31) << 15);
    out = (out & ~m16) | (old & m16);
  } else {
    out = (out & ~fbmsk) | (old & fbmsk);
  }
  gs.mem().write_pixel(fpsm, fbp, fbw, ux, uy, out);
  ++stats_.pixels_written;
}

void Rasterizer::draw(Gs& gs, const Vertex* v, int count, uint64_t prim) {
  const uint64_t mode = (gs.reg(0x1A) & 1) ? prim : gs.reg(0x1B);
  const int ctx = static_cast<int>(bits(mode, 9, 1));
  const uint64_t xyo = gs.reg(static_cast<uint8_t>(0x18 + ctx)), sc = gs.reg(static_cast<uint8_t>(0x40 + ctx));
  const float ofx = static_cast<float>(bits(xyo, 0, 16)) / 16.0f, ofy = static_cast<float>(bits(xyo, 32, 16)) / 16.0f;
  const int sx0 = static_cast<int>(bits(sc, 0, 11)), sx1 = static_cast<int>(bits(sc, 16, 11)),
            sy0 = static_cast<int>(bits(sc, 32, 11)), sy1 = static_cast<int>(bits(sc, 48, 11));
  const bool gouraud = bits(mode, 3, 1);
  auto attr = [&](const Vertex& p) {
    return Attr{static_cast<float>(p.r),
                static_cast<float>(p.g),
                static_cast<float>(p.b),
                static_cast<float>(p.a),
                static_cast<float>(p.z),
                p.s,
                p.t,
                p.q,
                static_cast<float>(p.u),
                static_cast<float>(p.v),
                static_cast<float>(p.fog)};
  };
  auto px = [&](const Vertex& p) { return static_cast<float>(p.x) / 16.0f - ofx; };
  auto py = [&](const Vertex& p) { return static_cast<float>(p.y) / 16.0f - ofy; };
  const uint32_t type = static_cast<uint32_t>(prim & 7);

  if (type == 6) {  // sprite: axis-aligned rectangle, flat colour from the second vertex
    ++stats_.sprites;
    const Vertex &a = v[0], &b = v[1];
    const float x0 = std::min(px(a), px(b)), x1 = std::max(px(a), px(b)), y0 = std::min(py(a), py(b)),
                y1 = std::max(py(a), py(b));
    const bool swap_x = px(a) > px(b), swap_y = py(a) > py(b);
    Attr A = attr(swap_x ? b : a), B = attr(swap_x ? a : b);
    Attr Ay = attr(swap_y ? b : a), By = attr(swap_y ? a : b);
    const Attr color = attr(b);
    const int ix0 = std::max(sx0, static_cast<int>(std::ceil(x0))),
              ix1 = std::min(sx1 + 1, static_cast<int>(std::ceil(x1)));
    const int iy0 = std::max(sy0, static_cast<int>(std::ceil(y0))),
              iy1 = std::min(sy1 + 1, static_cast<int>(std::ceil(y1)));
    const float wx = std::max(x1 - x0, 1e-6f), wy = std::max(y1 - y0, 1e-6f);
    for (int y = iy0; y < iy1; ++y) {
      const float ty = (static_cast<float>(y) + 0.5f - y0) / wy;  // sample at the pixel centre
      for (int x = ix0; x < ix1; ++x) {
        const float tx = (static_cast<float>(x) + 0.5f - x0) / wx;
        Attr p = color;
        p.u = A.u + (B.u - A.u) * tx;
        p.s = A.s + (B.s - A.s) * tx;
        p.v = Ay.v + (By.v - Ay.v) * ty;
        p.t = Ay.t + (By.t - Ay.t) * ty;
        p.q = b.q;
        p.z = static_cast<float>(b.z);
        shade(gs, x, y, p);
      }
    }
    return;
  }

  if (type == 0 || type == 7) {
    ++stats_.points;
    const int x = static_cast<int>(px(v[0])), y = static_cast<int>(py(v[0]));
    if (x >= sx0 && x <= sx1 && y >= sy0 && y <= sy1) shade(gs, x, y, attr(v[0]));
    return;
  }

  if (type == 1 || type == 2) {  // lines (DDA)
    ++stats_.lines;
    const float x0 = px(v[0]), y0 = py(v[0]), x1 = px(v[1]), y1 = py(v[1]);
    const int steps = static_cast<int>(std::max(std::fabs(x1 - x0), std::fabs(y1 - y0))) + 1;
    const Attr A = attr(v[0]), B = attr(v[1]);
    for (int i = 0; i < steps; ++i) {
      const float t = steps > 1 ? static_cast<float>(i) / static_cast<float>(steps - 1) : 0.0f;
      const int x = static_cast<int>(x0 + (x1 - x0) * t), y = static_cast<int>(y0 + (y1 - y0) * t);
      if (x < sx0 || x > sx1 || y < sy0 || y > sy1) continue;
      Attr p = gouraud
                   ? Attr{A.r + (B.r - A.r) * t, A.g + (B.g - A.g) * t, A.b + (B.b - A.b) * t, A.a + (B.a - A.a) * t,
                          A.z + (B.z - A.z) * t, A.s + (B.s - A.s) * t, A.t + (B.t - A.t) * t, A.q + (B.q - A.q) * t,
                          A.u + (B.u - A.u) * t, A.v + (B.v - A.v) * t, A.f + (B.f - A.f) * t}
                   : B;
      shade(gs, x, y, p);
    }
    return;
  }

  // triangles (list, strip, fan): barycentric scan of the bounding box
  ++stats_.triangles;
  (void)count;
  const float x[3] = {px(v[0]), px(v[1]), px(v[2])}, y[3] = {py(v[0]), py(v[1]), py(v[2])};
  const float area = (x[1] - x[0]) * (y[2] - y[0]) - (y[1] - y[0]) * (x[2] - x[0]);
  if (std::fabs(area) < 1e-6f) return;
  const Attr A[3] = {attr(v[0]), attr(v[1]), attr(v[2])};
  const float sign = area > 0 ? 1.0f : -1.0f;
  // Edge i is opposite vertex i. From the gradient of its (sign-normalised) edge function: a top
  // edge has the inside below it (de/dy > 0), a left edge has the inside to its right (de/dx > 0).
  bool top_left[3];
  for (int i = 0; i < 3; ++i) {
    const int a0 = (i + 1) % 3, a1 = (i + 2) % 3;
    const float gx = (y[a0] - y[a1]) * sign, gy = (x[a1] - x[a0]) * sign;
    top_left[i] = gy > 0 || (gy == 0 && gx > 0);
  }
  const int ix0 = std::max(sx0, static_cast<int>(std::ceil(std::min({x[0], x[1], x[2]}))));
  const int ix1 = std::min(sx1, static_cast<int>(std::floor(std::max({x[0], x[1], x[2]}))));
  const int iy0 = std::max(sy0, static_cast<int>(std::ceil(std::min({y[0], y[1], y[2]}))));
  const int iy1 = std::min(sy1, static_cast<int>(std::floor(std::max({y[0], y[1], y[2]}))));
  for (int yy = iy0; yy <= iy1; ++yy) {
    for (int xx = ix0; xx <= ix1; ++xx) {
      const float fx = static_cast<float>(xx), fy = static_cast<float>(yy);
      // Edge functions (sign-normalised so inside is >= 0) with the top-left fill rule: pixels
      // exactly on an edge belong to it only if it is a top or left edge, so shared edges of
      // adjacent triangles are drawn once (matters for blending).
      const float e0 = ((x[1] - fx) * (y[2] - fy) - (y[1] - fy) * (x[2] - fx)) * sign;
      const float e1 = ((x[2] - fx) * (y[0] - fy) - (y[2] - fy) * (x[0] - fx)) * sign;
      const float e2 = ((x[0] - fx) * (y[1] - fy) - (y[0] - fy) * (x[1] - fx)) * sign;
      if (e0 < 0 || e1 < 0 || e2 < 0) continue;
      if ((e0 == 0 && !top_left[0]) || (e1 == 0 && !top_left[1]) || (e2 == 0 && !top_left[2])) continue;
      const float w0 = e0 * sign / area, w1 = e1 * sign / area, w2 = 1.0f - w0 - w1;
      Attr p;
      auto lerp = [&](float Attr::* m) { return A[0].*m * w0 + A[1].*m * w1 + A[2].*m * w2; };
      p.z = lerp(&Attr::z);
      p.s = lerp(&Attr::s);
      p.t = lerp(&Attr::t);
      p.q = lerp(&Attr::q);
      p.u = lerp(&Attr::u);
      p.v = lerp(&Attr::v);
      p.f = lerp(&Attr::f);
      if (gouraud) {
        p.r = lerp(&Attr::r);
        p.g = lerp(&Attr::g);
        p.b = lerp(&Attr::b);
        p.a = lerp(&Attr::a);
      } else {
        p.r = A[2].r;
        p.g = A[2].g;
        p.b = A[2].b;
        p.a = A[2].a;
      }
      shade(gs, xx, yy, p);
    }
  }
}

}  // namespace lantern::gs
