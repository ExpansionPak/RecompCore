// SPDX-License-Identifier: GPL-3.0-or-later
#include "dolruntime/gxcore/gxcore.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <limits>

namespace dolruntime::gxcore {

namespace {

namespace ar = dolruntime::aurora_recomp;

std::uint32_t bits(std::uint32_t value, std::uint32_t count,
                   std::uint32_t shift) {
  return (value >> shift) & ((1u << count) - 1u);
}

// FogParam0/FogParam3 FloatValue (Dolphin BPMemory.cpp): mant(11)/exp(8)/sign(1)
// re-expanded to an IEEE-754 f32 (mantissa scaled from 11 to 23 bits).
float fog_param_float(std::uint32_t raw) {
  const std::uint32_t integral =
      (bits(raw, 1, 19) << 31) | (bits(raw, 8, 11) << 23) | (bits(raw, 11, 0) << 12);
  float f;
  std::memcpy(&f, &integral, sizeof f);
  return f;
}

// --- WGSL emission -----------------------------------------------------------

void emit(std::string& out, const char* text) { out += text; }

void emitf(std::string& out, const char* fmt, ...) {
  char buffer[512];
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(buffer, sizeof buffer, fmt, args);
  va_end(args);
  out += buffer;
}

} // namespace

namespace {

// --- Integer TEV fragment (Dolphin PixelShaderGen port) ----------------------
//
// Tables mirror PixelShaderGen.cpp; GLSL int3/int4 become WGSL vec3i/vec4i.
// WGSL shifts require a matching-width unsigned shift operand (no scalar-on-
// vector broadcast), so rgb shifts use vec3u(Nu) while alpha shifts use Nu.

const char* const kTevCInput[16] = {
    "prev.rgb",           "prev.aaa",           "c0.rgb",   "c0.aaa",
    "c1.rgb",             "c1.aaa",             "c2.rgb",   "c2.aaa",
    "textemp.rgb",        "textemp.aaa",        "rastemp.rgb", "rastemp.aaa",
    "vec3i(255,255,255)", "vec3i(128,128,128)", "konsttemp.rgb", "vec3i(0,0,0)"};
const char* const kTevAInput[8] = {"prev.a",    "c0.a",       "c1.a",
                                   "c2.a",      "textemp.a",  "rastemp.a",
                                   "konsttemp.a", "0"};
// Destination register NAMES (not swizzles): WGSL forbids assigning to a
// multi-component swizzle, so combines write the whole vec4i, reconstructing
// the untouched channel (rgb keeps .a, alpha keeps .rgb).
const char* const kTevOutput[4] = {"prev", "c0", "c1", "c2"};
const char kRgbaSwizzle[4] = {'r', 'g', 'b', 'a'};

const char* const kKselC[32] = {
    "vec3i(255,255,255)", "vec3i(223,223,223)", "vec3i(191,191,191)",
    "vec3i(159,159,159)", "vec3i(128,128,128)", "vec3i(96,96,96)",
    "vec3i(64,64,64)",    "vec3i(32,32,32)",    "vec3i(0,0,0)",
    "vec3i(0,0,0)",       "vec3i(0,0,0)",       "vec3i(0,0,0)",
    "psc.kcolors[0].rgb", "psc.kcolors[1].rgb", "psc.kcolors[2].rgb",
    "psc.kcolors[3].rgb", "psc.kcolors[0].rrr", "psc.kcolors[1].rrr",
    "psc.kcolors[2].rrr", "psc.kcolors[3].rrr", "psc.kcolors[0].ggg",
    "psc.kcolors[1].ggg", "psc.kcolors[2].ggg", "psc.kcolors[3].ggg",
    "psc.kcolors[0].bbb", "psc.kcolors[1].bbb", "psc.kcolors[2].bbb",
    "psc.kcolors[3].bbb", "psc.kcolors[0].aaa", "psc.kcolors[1].aaa",
    "psc.kcolors[2].aaa", "psc.kcolors[3].aaa"};
const char* const kKselA[32] = {
    "255",  "223",  "191",  "159",  "128",  "96",   "64",   "32",
    "0",    "0",    "0",    "0",    "0",    "0",    "0",    "0",
    "psc.kcolors[0].r", "psc.kcolors[1].r", "psc.kcolors[2].r", "psc.kcolors[3].r",
    "psc.kcolors[0].g", "psc.kcolors[1].g", "psc.kcolors[2].g", "psc.kcolors[3].g",
    "psc.kcolors[0].b", "psc.kcolors[1].b", "psc.kcolors[2].b", "psc.kcolors[3].b",
    "psc.kcolors[0].a", "psc.kcolors[1].a", "psc.kcolors[2].a", "psc.kcolors[3].a"};

const char* const kAlphaLogic[4] = {" && ", " || ", " != ", " == "};

// Dolphin tev_alpha_funcs_table: prev.a <cmp> ref (Never/Always take no ref).
std::string alpha_cond(std::uint8_t comp, const char* ref) {
  switch (comp) {
  case 0: return "(false)";
  case 1: return std::string("(prev.a < ") + ref + ")";
  case 2: return std::string("(prev.a == ") + ref + ")";
  case 3: return std::string("(prev.a <= ") + ref + ")";
  case 4: return std::string("(prev.a > ") + ref + ")";
  case 5: return std::string("(prev.a != ") + ref + ")";
  case 6: return std::string("(prev.a >= ") + ref + ")";
  default: return "(true)";
  }
}

bool cc_uses(const TevStageKey& s, std::uint8_t arg) {
  return s.cc_a == arg || s.cc_b == arg || s.cc_c == arg || s.cc_d == arg;
}
bool ac_uses(const TevStageKey& s, std::uint8_t arg) {
  return s.ac_a == arg || s.ac_b == arg || s.ac_c == arg || s.ac_d == arg;
}

// Shift token that WGSL accepts for the given width. vec: vec3u operand.
std::string shl(bool vec, unsigned n) {
  char b[32];
  std::snprintf(b, sizeof b, vec ? " << vec3u(%uu)" : " << %uu", n);
  return b;
}
std::string shr(bool vec, unsigned n) {
  char b[32];
  std::snprintf(b, sizeof b, vec ? " >> vec3u(%uu)" : " >> %uu", n);
  return b;
}

// One "regular" combine (Dolphin WriteTevRegular): (d+bias)*scale +/- lerp,
// with the GC scale-fold-into-lerp and rounding bias. `comp` selects rgb/a.
std::string tev_regular(bool vec, const char* comp, std::uint8_t bias,
                        std::uint8_t op, std::uint8_t scale) {
  const std::string a = std::string("tevin_a.") + comp;
  const std::string b = std::string("tevin_b.") + comp;
  const std::string c = std::string("tevin_c.") + comp;
  const std::string d = std::string("tevin_d.") + comp;
  const char* biasstr = bias == 1 ? " + 128" : bias == 2 ? " - 128" : "";
  const char* opstr = op == 1 ? "-" : "+";
  const bool div2 = scale == 3;
  const unsigned sh = scale == 1 ? 1u : scale == 2 ? 2u : 0u; // Scale2/4 shift
  const char* lerpbias = div2 ? "" : (op == 1 ? " + 127" : " + 128");

  // lerp = (a<<8) + (b-a)*(c + (c>>7))   [c: 0..255 -> 0..256]
  std::string lerp = "((" + a + shl(vec, 8) + ") + (" + b + " - " + a +
                     ") * (" + c + " + (" + c + shr(vec, 7) + ")))";
  // fold scale into the lerp and add rounding bias (skipped for Divide2)
  if (!div2 && sh != 0)
    lerp = "((" + lerp + shl(vec, sh) + ")" + lerpbias + ")";
  else if (!div2)
    lerp = "(" + lerp + lerpbias + ")";
  lerp = "(" + lerp + shr(vec, 8) + ")";

  std::string dterm = "(" + d + biasstr + ")";
  if (!div2 && sh != 0)
    dterm = "(" + dterm + shl(vec, sh) + ")";
  std::string res = "(" + dterm + " " + opstr + " " + lerp + ")";
  if (div2)
    res = "(" + res + shr(vec, 1) + ")";
  return res;
}

// Compare-mode combine (bias==Compare): tevin_d + compare(a,b)?c:0.
std::string tev_compare(bool vec, std::uint8_t comparison,
                        std::uint8_t compare_mode) {
  const char* cmp = comparison == 1 ? "==" : ">"; // TevComparison EQ==1
  const char* comp = vec ? "rgb" : "a";
  const std::string d = std::string("tevin_d.") + comp;
  std::string zero = vec ? "vec3i(0,0,0)" : "0";
  std::string cval = vec ? "tevin_c.rgb" : "tevin_c.a";
  std::string cond;
  switch (compare_mode) {
  case 0: // R8
    cond = std::string("(tevin_a.r ") + cmp + " tevin_b.r)";
    break;
  case 1: // GR16
    cond = std::string("((tevin_a.r + tevin_a.g*256) ") + cmp +
           " (tevin_b.r + tevin_b.g*256))";
    break;
  case 2: // BGR24
    cond = std::string("((tevin_a.r + tevin_a.g*256 + tevin_a.b*65536) ") +
           cmp + " (tevin_b.r + tevin_b.g*256 + tevin_b.b*65536))";
    break;
  default: // RGB8/A8 componentwise
    if (comparison == 1)
      return "(" + d + " + (vec3i(1,1,1) - sign(abs(tevin_a.rgb - "
                       "tevin_b.rgb))) * tevin_c.rgb)";
    return "(" + d + " + max(sign(tevin_a.rgb - tevin_b.rgb), vec3i(0,0,0)) "
                     "* tevin_c.rgb)";
  }
  if (compare_mode == 3 && !vec) { // A8 uses .a comparison
    cond = std::string("(tevin_a.a ") + cmp + " tevin_b.a)";
  }
  return "(" + d + " + select(" + zero + ", " + cval + ", " + cond + "))";
}

// Fog (Dolphin PixelShaderGen WriteFog + PixelShaderManager fog constants).
// Applied to prev.rgb after the alpha test, exactly Dolphin's order. GC zCoord
// is the 24-bit screen depth (0=near, 0xFFFFFF=far); our substrate runs
// reversed-Z so in.pos.z is near=1/far=0 → zCoord = (1-in.pos.z)*2^24 (same as
// graphics/aurora lib/gx/shader.cpp:1501). Only called when fog_fsel != Off.
void emit_fog(std::string& out, const ShaderKey& key) {
  const auto fsel = static_cast<FogType>(key.fog_fsel);
  if (fsel == FogType::Off)
    return;
  emit(out, "    // Fog (Dolphin WriteFog)\n"
            "    var zCoord = i32((1.0 - in.pos.z) * 16777216.0);\n"
            "    zCoord = clamp(zCoord, 0, 16777215);\n");
  if (static_cast<FogProjection>(key.fog_proj) == FogProjection::Perspective) {
    // ze = A / (B - (Zs >> B_SHF))
    emit(out, "    var ze = (psc.fogf.x * 16777216.0) / "
              "f32(psc.fogi.y - (zCoord >> u32(psc.fogi.w)));\n");
  } else {
    // ze = A * Zs (orthographic; no B_SHF)
    emit(out, "    var ze = psc.fogf.x * f32(zCoord) / 16777216.0;\n");
  }
  // x_adjust = sqrt((x-center)^2 + k^2)/k, ze *= x_adjust.
  if (key.fog_range != 0) {
    emit(out,
         "    let offset = (2.0 * (in.pos.x / psc.fogf.w)) - 1.0 - psc.fogf.z;\n"
         "    let floatindex = clamp(9.0 - abs(offset) * 9.0, 0.0, 9.0);\n"
         "    let indexlower = u32(floatindex);\n"
         "    let indexupper = indexlower + 1u;\n"
         "    let klower = psc.fogrange[indexlower >> 2u][indexlower & 3u];\n"
         "    let kupper = psc.fogrange[indexupper >> 2u][indexupper & 3u];\n"
         "    let k = mix(klower, kupper, fract(floatindex));\n"
         "    let x_adjust = sqrt(offset * offset + k * k) / k;\n"
         "    ze = ze * x_adjust;\n");
  }
  emit(out, "    var fogv = clamp(ze - psc.fogf.y, 0.0, 1.0);\n");
  switch (fsel) {
  case FogType::Exp:
    emit(out, "    fogv = 1.0 - exp2(-8.0 * fogv);\n");
    break;
  case FogType::ExpSq:
    emit(out, "    fogv = 1.0 - exp2(-8.0 * fogv * fogv);\n");
    break;
  case FogType::BackwardsExp:
    emit(out, "    fogv = exp2(-8.0 * (1.0 - fogv));\n");
    break;
  case FogType::BackwardsExpSq:
    emit(out, "    fogv = 1.0 - fogv;\n"
              "    fogv = exp2(-8.0 * fogv * fogv);\n");
    break;
  case FogType::Linear:
  default:
    break; // linear: fogv unchanged
  }
  // Integer blend: prev.rgb = (prev.rgb*(256-ifog) + fogcolor*ifog) >> 8.
  emit(out, "    let ifog = i32(round(fogv * 256.0));\n"
            "    prev = vec4i((prev.rgb * (256 - ifog) + psc.fogcolor.rgb * "
            "ifog) >> vec3u(8u), prev.a);\n");
}

void emit_tev_fragment(std::string& out, const ShaderKey& key) {
  // Single-texmap (<=1 distinct texmap) samples tex0 exactly as before; a
  // multi-texmap TEV (e.g. THP YUV Y/U/V on texmap 0/1/2) samples tex{texmap}.
  const bool multi_texmap = texmap_popcount(used_texmap_mask(key)) > 1u;
  emit(out, "@fragment\nfn fs_main(in: VertexOut) -> @location(0) vec4f {\n");
  emit(out, "    var prev = psc.colors[0];\n"
            "    var c0 = psc.colors[1];\n"
            "    var c1 = psc.colors[2];\n"
            "    var c2 = psc.colors[3];\n"
            "    var rastemp = vec4i(0,0,0,0);\n"
            "    var rawtextemp = vec4i(0,0,0,0);\n"
            "    var textemp = vec4i(0,0,0,0);\n"
            "    var konsttemp = vec4i(0,0,0,0);\n"
            "    var tevin_a = vec4i(0,0,0,0);\n"
            "    var tevin_b = vec4i(0,0,0,0);\n"
            "    var tevin_c = vec4i(0,0,0,0);\n"
            "    var tevin_d = vec4i(0,0,0,0);\n"
            "    let col0i = vec4i(round(in.color0 * 255.0));\n"
            "    let col1i = vec4i(round(in.color1 * 255.0));\n");

  for (std::uint32_t n = 0; n < key.num_tev_stages; ++n) {
    const TevStageKey& s = key.tev_stages[n];
    emitf(out, "    // TEV stage %u\n", n);

    // Rasterized color, if referenced (RASC=10/RASA=11 color; RASA=5 alpha).
    const bool uses_ras =
        cc_uses(s, 10) || cc_uses(s, 11) || ac_uses(s, 5);
    if (uses_ras) {
      const char* src = s.tevorders_colorchan == 0
                            ? "col0i"
                            : s.tevorders_colorchan == 1 ? "col1i"
                                                         : "vec4i(0,0,0,0)";
      emitf(out, "    rastemp = %s.%c%c%c%c;\n", src,
            kRgbaSwizzle[s.ras_swap[0]], kRgbaSwizzle[s.ras_swap[1]],
            kRgbaSwizzle[s.ras_swap[2]], kRgbaSwizzle[s.ras_swap[3]]);
    }

    // Texture sample for this stage. texunit is 0 on the single-texmap fast path
    // (byte-identical output) or the stage's texmap when >1 texmap is combined.
    std::uint32_t texcoord = s.tevorders_texcoord;
    if (texcoord >= key.num_tex_gens)
      texcoord = 0;
    const std::uint32_t texunit = multi_texmap ? (s.tevorders_texmap & 7u) : 0u;
    if (s.tevorders_enable != 0 && key.num_tex_gens > 0 && key.textured != 0) {
      const bool proj =
          texcoord < kMaxTexGens && key.tex_gens[texcoord].projection != 0u;
      if (proj)
        emitf(out,
              "    { let uv = in.uv%u.xy / max(in.uv%u.z, 1e-6); "
              "rawtextemp = vec4i(round(textureSample(tex%u, samp%u, uv) * "
              "255.0)); }\n",
              texcoord, texcoord, texunit, texunit);
      else
        emitf(out,
              "    { let uv = in.uv%u.xy; rawtextemp = "
              "vec4i(round(textureSample(tex%u, samp%u, uv) * 255.0)); }\n",
              texcoord, texunit, texunit);
      emitf(out, "    textemp = rawtextemp.%c%c%c%c;\n",
            kRgbaSwizzle[s.tex_swap[0]], kRgbaSwizzle[s.tex_swap[1]],
            kRgbaSwizzle[s.tex_swap[2]], kRgbaSwizzle[s.tex_swap[3]]);
    } else if (key.num_tex_gens == 0) {
      emit(out, "    textemp = vec4i(0,0,0,0);\n");
    } else {
      emit(out, "    textemp = vec4i(255,255,255,255);\n");
    }

    // Konst, if referenced (KONST=14 color / 6 alpha).
    if (cc_uses(s, 14) || ac_uses(s, 6))
      emitf(out, "    konsttemp = vec4i(%s, %s);\n", kKselC[s.ksel_kc],
            kKselA[s.ksel_ka]);

    // tevin_{a,b,c} masked to 0..255; tevin_d keeps the 10-bit range.
    emitf(out, "    tevin_a = vec4i(%s, %s) & vec4i(255,255,255,255);\n",
          kTevCInput[s.cc_a], kTevAInput[s.ac_a]);
    emitf(out, "    tevin_b = vec4i(%s, %s) & vec4i(255,255,255,255);\n",
          kTevCInput[s.cc_b], kTevAInput[s.ac_b]);
    emitf(out, "    tevin_c = vec4i(%s, %s) & vec4i(255,255,255,255);\n",
          kTevCInput[s.cc_c], kTevAInput[s.ac_c]);
    emitf(out, "    tevin_d = vec4i(%s, %s);\n", kTevCInput[s.cc_d],
          kTevAInput[s.ac_d]);

    // Color combine.
    std::string crgb = s.cc_bias == 3
                           ? tev_compare(true, s.cc_op, s.cc_scale)
                           : tev_regular(true, "rgb", s.cc_bias, s.cc_op,
                                         s.cc_scale);
    const char* clo = s.cc_clamp ? "vec3i(0,0,0)" : "vec3i(-1024,-1024,-1024)";
    const char* chi =
        s.cc_clamp ? "vec3i(255,255,255)" : "vec3i(1023,1023,1023)";
    const char* cdest = kTevOutput[s.cc_dest];
    emitf(out, "    %s = vec4i(clamp(%s, %s, %s), %s.a);\n", cdest,
          crgb.c_str(), clo, chi, cdest);

    // Alpha combine.
    std::string aexp = s.ac_bias == 3
                           ? tev_compare(false, s.ac_op, s.ac_scale)
                           : tev_regular(false, "a", s.ac_bias, s.ac_op,
                                         s.ac_scale);
    const char* alo = s.ac_clamp ? "0" : "-1024";
    const char* ahi = s.ac_clamp ? "255" : "1023";
    const char* adest = kTevOutput[s.ac_dest];
    emitf(out, "    %s = vec4i(%s.rgb, clamp(%s, %s, %s));\n", adest, adest,
          aexp.c_str(), alo, ahi);
  }

  // The last stage's destination register is what reaches the framebuffer.
  const TevStageKey& last = key.tev_stages[key.num_tev_stages - 1];
  if (last.cc_dest != 0)
    emitf(out, "    prev = vec4i(%s.rgb, prev.a);\n", kTevOutput[last.cc_dest]);
  if (last.ac_dest != 0)
    emitf(out, "    prev = vec4i(prev.rgb, %s.a);\n", kTevOutput[last.ac_dest]);

  // Alpha test (Dolphin WriteAlphaTest): discard on failure.
  const std::string c0 = alpha_cond(key.alpha_comp0, "psc.alpha_ref.x");
  const std::string c1 = alpha_cond(key.alpha_comp1, "psc.alpha_ref.y");
  emitf(out, "    if (!( %s%s%s )) { discard; }\n", c0.c_str(),
        kAlphaLogic[key.alpha_logic], c1.c_str());

  // Fog runs after the alpha test (Dolphin order), modifying prev.rgb only.
  emit_fog(out, key);

  emit(out, "    return vec4f(prev) / 255.0;\n}\n");
}

// --- Integer lighting (Dolphin LightingShaderGen port) -----------------------
//
// GenerateLightShader for one light: emits the attenuation setup then the
// diffuse accumulation into `lacc` (int4). GLSL's `int3/float3` become WGSL
// `vec3i/vec3f`; GLSL `a ? b : c` becomes `select(c, b, a)`. WGSL forbids
// assigning to a multi-component swizzle (`lacc.rgb += ...`), so the rgb pass
// rebuilds the whole vector; the alpha pass writes the single `.a` component.
void emit_light(std::string& out, const LightChanKey& ch, int i, bool alpha) {
  const auto attn = static_cast<AttenuationFunc>(ch.attnfunc);
  const auto diff = static_cast<DiffuseFunc>(ch.diffusefunc);
  emitf(out, "        { // light %d\n", i);
  switch (attn) {
  case AttenuationFunc::None:
  case AttenuationFunc::Dir:
    emitf(out, "            ldir = normalize(vsc.lights[%d].pos.xyz - pos);\n", i);
    emit(out, "            attn = 1.0;\n"
              "            if (length(ldir) == 0.0) { ldir = _normal; }\n");
    break;
  case AttenuationFunc::Spec:
    emitf(out, "            ldir = normalize(vsc.lights[%d].pos.xyz - pos);\n", i);
    emitf(out,
          "            attn = select(0.0, max(0.0, dot(_normal, "
          "vsc.lights[%d].dir.xyz)), dot(_normal, ldir) >= 0.0);\n",
          i);
    emitf(out, "            cosAttn = vsc.lights[%d].cosatt.xyz;\n", i);
    if (diff == DiffuseFunc::None)
      emitf(out, "            distAttn = vsc.lights[%d].distatt.xyz;\n", i);
    else
      emitf(out, "            distAttn = normalize(vsc.lights[%d].distatt.xyz);\n", i);
    emit(out, "            attn = max(0.0, dot(cosAttn, vec3f(1.0, attn, "
              "attn*attn))) / dot(distAttn, vec3f(1.0, attn, attn*attn));\n");
    break;
  case AttenuationFunc::Spot:
    emitf(out, "            ldir = vsc.lights[%d].pos.xyz - pos;\n", i);
    emit(out, "            dist2 = dot(ldir, ldir);\n"
              "            dist = sqrt(dist2);\n"
              "            ldir = ldir / dist;\n");
    emitf(out, "            attn = max(0.0, dot(ldir, vsc.lights[%d].dir.xyz));\n", i);
    emitf(out,
          "            attn = max(0.0, vsc.lights[%d].cosatt.x + "
          "vsc.lights[%d].cosatt.y*attn + vsc.lights[%d].cosatt.z*attn*attn) / "
          "dot(vsc.lights[%d].distatt.xyz, vec3f(1.0, dist, dist2));\n",
          i, i, i, i);
    break;
  }
  // Diffuse term: attn * <diffuse> * color. Sign uses raw dot, Clamp max(0,dot),
  // None omits the dot (LightingShaderGen.cpp GenerateLightShader).
  const char* dterm = diff == DiffuseFunc::Sign  ? "(dot(ldir, _normal)) * "
                      : diff == DiffuseFunc::Clamp ? "max(0.0, dot(ldir, _normal)) * "
                                                   : "";
  if (!alpha) {
    emitf(out,
          "            lacc = lacc + vec4i(vec3i(round(attn * %svec3f("
          "vsc.lights[%d].color.rgb))), 0);\n",
          dterm, i);
  } else {
    emitf(out,
          "            lacc.a = lacc.a + i32(round(attn * %sf32("
          "vsc.lights[%d].color.a)));\n",
          dterm, i);
  }
  emit(out, "        }\n");
}

// GenerateLightingShaderHeader for one color channel j (litchan j = rgb,
// j+2 = alpha). Returns a WGSL function calc_lighting_chn{j}.
void emit_lighting_chan(std::string& out, const ShaderKey& key,
                        std::uint32_t j) {
  const LightChanKey& col = key.litchan[j];
  const LightChanKey& alp = key.litchan[j + 2];
  emitf(out,
        "fn calc_lighting_chn%u(base_color: vec4f, pos: vec3f, _normal: vec3f)"
        " -> vec4f {\n",
        j);
  emit(out, "    var lacc: vec4i;\n"
            "    var mat: vec4i;\n"
            "    var ldir: vec3f;\n    var cosAttn: vec3f;\n"
            "    var distAttn: vec3f;\n"
            "    var dist: f32;\n    var dist2: f32;\n    var attn: f32;\n");
  // mat rgb.
  if (static_cast<MatSource>(col.matsource) == MatSource::Vertex)
    emit(out, "    mat = vec4i(round(base_color * 255.0));\n");
  else
    emitf(out, "    mat = vsc.materials[%u];\n", j + 2);
  // lacc rgb (ambient / disabled-lighting seed).
  if (col.enablelighting != 0) {
    if (static_cast<MatSource>(col.ambsource) == MatSource::Vertex)
      emit(out, "    lacc = vec4i(round(base_color * 255.0));\n");
    else
      emitf(out, "    lacc = vsc.materials[%u];\n", j);
  } else {
    emit(out, "    lacc = vec4i(255, 255, 255, 255);\n");
  }
  // mat.w when the alpha material source differs from the color source.
  if (alp.matsource != col.matsource) {
    if (static_cast<MatSource>(alp.matsource) == MatSource::Vertex)
      emit(out, "    mat.w = i32(round(base_color.w * 255.0));\n");
    else
      emitf(out, "    mat.w = vsc.materials[%u].w;\n", j + 2);
  }
  // lacc.w (alpha ambient / disabled seed).
  if (alp.enablelighting != 0) {
    if (static_cast<MatSource>(alp.ambsource) == MatSource::Vertex)
      emit(out, "    lacc.w = i32(round(base_color.w * 255.0));\n");
    else
      emitf(out, "    lacc.w = vsc.materials[%u].w;\n", j);
  } else {
    emit(out, "    lacc.w = 255;\n");
  }
  // Color (rgb) lights.
  if (col.enablelighting != 0) {
    for (int i = 0; i < 8; ++i)
      if ((col.light_mask & (1u << i)) != 0u)
        emit_light(out, col, i, false);
  }
  // Alpha lights.
  if (alp.enablelighting != 0) {
    for (int i = 0; i < 8; ++i)
      if ((alp.light_mask & (1u << i)) != 0u)
        emit_light(out, alp, i, true);
  }
  emit(out, "    lacc = clamp(lacc, vec4i(0), vec4i(255));\n"
            "    return vec4f((mat * (lacc + (lacc >> vec4u(7)))) >> vec4u(8)) "
            "/ 255.0;\n}\n\n");
}

// True when color channel j (0/1) runs the full integer-lighting path rather
// than raw vertex-color passthrough. Dolphin runs dolphin_calculate_lighting_chn
// for every configured channel; the passthrough shortcut is only valid when the
// result would equal the vertex color, i.e. the channel is unlit AND both its
// color and alpha material sources are the vertex (mat=vertex, lacc=white =>
// output=vertex). A captured channel that enables lighting, or that selects the
// material register for either color or alpha, takes the full path. An
// unconfigured channel (bit clear in chan_captured_mask) stays passthrough even
// though MatSource::Register decodes to 0 — there is no material to apply.
static bool channel_lit_path(const ShaderKey& k, unsigned j) {
  if ((k.chan_captured_mask & (1u << j)) == 0u)
    return false;
  const LightChanKey& col = k.litchan[j];
  const LightChanKey& alp = k.litchan[j + 2];
  return col.enablelighting != 0 || alp.enablelighting != 0 ||
         static_cast<MatSource>(col.matsource) == MatSource::Register ||
         static_cast<MatSource>(alp.matsource) == MatSource::Register;
}

} // namespace

std::string generate_wgsl(const ShaderKey& key) {
  std::string out;
  out.reserve(4096);
  const bool tev = key.tev_valid != 0;
  const bool lit = key.lit_valid != 0;
  // Color texgens (Dolphin VertexShaderGen texgentype switch, TexGenType::Color0/
  // Color1) drive the texcoord from a lit color channel. A Color1 texgen consumes
  // o.color1 even on a non-TEV draw, and an emboss texgen needs the light dir in
  // tangent space, so detect demand up front to widen the varyings/uniform/inputs.
  bool needs_color1 = false;
  bool has_emboss = false;
  for (std::uint32_t i = 0; i < key.num_tex_gens; ++i) {
    const auto t = static_cast<TexGenType>(key.tex_gens[i].texgentype);
    if (t == TexGenType::Color1)
      needs_color1 = true;
    else if (t == TexGenType::EmbossMap)
      has_emboss = true;
  }
  // Emboss transforms the light dir into tangent space, so the lights uniform +
  // the NBT binormal/tangent vertex attrs must be present even on an unlit draw.
  const bool emit_lights = lit || has_emboss;
  const bool emit_nbt = has_emboss; // binormal/tangent vertex inputs required
  // color1 occupies @location(1) whenever it is emitted; texcoords shift up one.
  const bool emit_color1 = tev || needs_color1;
  const std::uint32_t uv_loc_base = emit_color1 ? 2u : 1u;
  // Per-channel: run the full integer-lighting path (vs vertex-color
  // passthrough). Channel 1 only matters when its varying is emitted.
  const bool chan0_lit = channel_lit_path(key, 0u);
  const bool chan1_lit = emit_color1 && channel_lit_path(key, 1u);
  // The lit path reads vsc.materials (register material/ambient) and, when a
  // light is enabled, vsc.lights. Whenever either the light path or a
  // register-material channel is active we must expose the full uniform view
  // (both fields, in the fixed C struct order) so offsets line up.
  const bool needs_uniform_full = emit_lights || chan0_lit || chan1_lit;
  // Vertex-format N/B/T presence. A lit/emboss draw whose format omits an
  // attribute substitutes the cross-draw cached value from the uniform
  // (Dolphin I_CACHED_NORMAL fallback, VertexShaderGen.cpp:607-632) instead of
  // reading a per-vertex input that the decoder left zeroed (normalize(0)=NaN).
  const bool has_normal = key.has_vertex_normal != 0;
  const bool has_binormal = key.has_vertex_binormal != 0;
  const bool has_tangent = key.has_vertex_tangent != 0;
  const char* normal_in = has_normal ? "in.rawnormal" : "vsc.cached_normal.xyz";
  const char* tangent_in =
      has_tangent ? "in.rawtangent" : "vsc.cached_tangent.xyz";
  const char* binormal_in =
      has_binormal ? "in.rawbinormal" : "vsc.cached_binormal.xyz";
  const bool uses_cached_normal = (chan0_lit || chan1_lit) && lit && !has_normal;
  const bool uses_cached_tangent = has_emboss && !has_tangent;
  const bool uses_cached_binormal = has_emboss && !has_binormal;
  // Declare all three cached fields together when any is referenced so the WGSL
  // struct offsets stay aligned with the C VertexShaderConstants tail (they are
  // laid out normal, tangent, binormal — skipping one mid-run would misplace a
  // later one). Only emitted when actually used, so formats that carry the
  // attribute keep byte-identical goldens.
  const bool uses_cached =
      uses_cached_normal || uses_cached_tangent || uses_cached_binormal;

  emit(out, "// gxcore generated shader (Dolphin VertexShaderGen shape)\n");
  if (needs_uniform_full)
    emit(out, "struct Light {\n"
              "    color: vec4i,\n"
              "    cosatt: vec4f,\n"
              "    distatt: vec4f,\n"
              "    pos: vec4f,\n"
              "    dir: vec4f,\n"
              "};\n");
  emit(out, "struct VertexShaderConstants {\n"
            "    posnormalmatrix: array<vec4f, 6>,\n"
            "    projection: array<vec4f, 4>,\n"
            "    texmatrices: array<vec4f, 24>,\n"
            "    transformmatrices: array<vec4f, 64>,\n");
  // The lit/emboss/register-material path binds the full uniform buffer (lights
  // + materials); the plain path declares only the leading fields (a smaller
  // WGSL struct over the same buffer is valid), keeping every existing golden
  // byte-identical.
  if (needs_uniform_full)
    emit(out, "    lights: array<Light, 8>,\n"
              "    materials: array<vec4i, 4>,\n");
  if (uses_cached)
    emit(out, "    cached_normal: vec4f,\n"
              "    cached_tangent: vec4f,\n"
              "    cached_binormal: vec4f,\n");
  emit(out, "};\n"
            "@group(1) @binding(0) var<uniform> vsc: VertexShaderConstants;\n");
  if (tev) {
    // PixelShaderConstants (Dolphin I_COLORS/I_KCOLORS/I_ALPHA). Integer TEV.
    // group(2) here (texture shifts to group(3)) so an untextured TEV draw
    // leaves no gap in the pipeline's bind-group indices.
    emit(out, "struct PixelShaderConstants {\n"
              "    colors: array<vec4i, 4>,\n"
              "    kcolors: array<vec4i, 4>,\n"
              "    alpha_ref: vec4i,\n"
              "    fogcolor: vec4i,\n"
              "    fogi: vec4i,\n"
              "    fogf: vec4f,\n"
              "    fogrange: array<vec4f, 3>,\n"
              "};\n"
              "@group(2) @binding(0) var<uniform> psc: PixelShaderConstants;\n");
  }
  if (key.textured != 0) {
    const std::uint32_t tex_group = tev ? 3u : 2u;
    const std::uint32_t used = used_texmap_mask(key);
    if (texmap_popcount(used) <= 1u) {
      emitf(out,
            "@group(%u) @binding(0) var tex0: texture_2d<f32>;\n"
            "@group(%u) @binding(1) var samp0: sampler;\n",
            tex_group, tex_group);
    } else {
      // Multi-texmap: texmap t -> binding 2t (texture) / 2t+1 (sampler), matching
      // the substrate bind group built from the same used-texmap set (63/Mfin).
      for (std::uint32_t t = 0; t < 8u; ++t) {
        if ((used & (1u << t)) == 0u)
          continue;
        emitf(out,
              "@group(%u) @binding(%u) var tex%u: texture_2d<f32>;\n"
              "@group(%u) @binding(%u) var samp%u: sampler;\n",
              tex_group, 2u * t, t, tex_group, 2u * t + 1u, t);
      }
    }
  }

  // Fixed vertex input layout (kVertexStrideBytes); unused inputs are ignored.
  emit(out, "struct VertexIn {\n"
            "    @location(0) rawpos: vec3f,\n"
            "    @location(1) posmtx: u32,\n"
            "    @location(2) rawcolor0: vec4f,\n"
            "    @location(3) rawcolor1: vec4f,\n"
            "    @location(4) rawtex0: vec2f,\n"
            "    @location(5) rawtex1: vec2f,\n"
            "    @location(6) rawtex2: vec2f,\n"
            "    @location(7) rawtex3: vec2f,\n");
  if ((lit || emit_nbt) && has_normal)
    emit(out, "    @location(8) rawnormal: vec3f,\n");
  if (key.has_tex_mtx_idx != 0)
    emit(out, "    @location(9) texmtxidx: u32,\n");
  if (emit_nbt && has_binormal)
    emit(out, "    @location(10) rawbinormal: vec3f,\n");
  if (emit_nbt && has_tangent)
    emit(out, "    @location(11) rawtangent: vec3f,\n");
  emit(out, "};\n");

  emit(out, "struct VertexOut {\n"
            "    @builtin(position) pos: vec4f,\n"
            "    @location(0) color0: vec4f,\n");
  if (emit_color1)
    emit(out, "    @location(1) color1: vec4f,\n");
  for (std::uint32_t i = 0; i < key.num_tex_gens; ++i)
    emitf(out, "    @location(%u) uv%u: vec3f,\n", uv_loc_base + i, i);
  emit(out, "};\n\n");

  // Integer lighting header (Dolphin GenerateLightingShaderHeader): one function
  // per color channel that takes the full path (lit or register-material).
  if (chan0_lit)
    emit_lighting_chan(out, key, 0u);
  if (chan1_lit)
    emit_lighting_chan(out, key, 1u);

  emit(out, "@vertex\nfn vs_main(in: VertexIn) -> VertexOut {\n"
            "    var o: VertexOut;\n");
  if (key.has_pos_mtx_idx != 0) {
    emit(out, "    let posidx = i32(in.posmtx);\n"
              "    let p0 = vsc.transformmatrices[posidx];\n"
              "    let p1 = vsc.transformmatrices[posidx + 1];\n"
              "    let p2 = vsc.transformmatrices[posidx + 2];\n");
  } else {
    emit(out, "    let p0 = vsc.posnormalmatrix[0];\n"
              "    let p1 = vsc.posnormalmatrix[1];\n"
              "    let p2 = vsc.posnormalmatrix[2];\n");
  }
  emit(out, "    let pos4 = vec4f(in.rawpos, 1.0);\n"
            "    let viewpos = vec4f(dot(p0, pos4), dot(p1, pos4), "
            "dot(p2, pos4), 1.0);\n"
            "    var clip = vec4f(dot(vsc.projection[0], viewpos), "
            "dot(vsc.projection[1], viewpos), dot(vsc.projection[2], "
            "viewpos), dot(vsc.projection[3], viewpos));\n"
            // GC clip z lands in [-w, 0]; the substrate runs reversed-Z
            // (graphics/aurora lib/gx/gx.hpp UseReversedZ), so flip.
            "    clip.z = -clip.z;\n"
            "    o.pos = clip;\n");
  // Vertex colors (Dolphin VertexShaderGen WriteVertexBody). First select each
  // channel's vertex color with the GX presence rules: channel 0 prefers
  // rawcolor0, falls back to rawcolor1 when only color1 is present; channel 1
  // uses rawcolor1 only when BOTH colors are present; the missing value is white
  // (Dolphin's default missing_color_value). Then every channel runs
  // dolphin_calculate_lighting_chn when it takes the full path (which internally
  // applies the register material/ambient and any enabled lights), else the
  // vertex color passes straight through. numColorChans zeroes unused channels.
  const bool use_color1 = key.has_color0 != 0 && key.has_color1 != 0;
  const char* vc0 = key.has_color0 != 0   ? "in.rawcolor0"
                    : key.has_color1 != 0 ? "in.rawcolor1"
                                          : "vec4f(1.0)";
  const char* vc1 = use_color1 ? "in.rawcolor1" : "vec4f(1.0)";
  if (chan0_lit || chan1_lit) {
    // View-space normal (posnormalmatrix rows 3-5). When no channel enables a
    // light the input carries no normal (register-material-only path), so seed a
    // constant — dolphin_calculate_lighting_chn only reads it inside a light and
    // there are none, leaving the material term intact.
    if (lit)
      emitf(out, "    let _normal = normalize(vec3f("
                 "dot(vsc.posnormalmatrix[3].xyz, %s), "
                 "dot(vsc.posnormalmatrix[4].xyz, %s), "
                 "dot(vsc.posnormalmatrix[5].xyz, %s)));\n",
            normal_in, normal_in, normal_in);
    else
      emit(out, "    let _normal = vec3f(0.0, 0.0, 1.0);\n");
  }
  if (chan0_lit)
    emitf(out, "    o.color0 = calc_lighting_chn0(%s, viewpos.xyz, _normal);\n",
          vc0);
  else
    emitf(out, "    o.color0 = %s;\n", vc0);
  if (emit_color1) {
    if (chan1_lit)
      emitf(out,
            "    o.color1 = calc_lighting_chn1(%s, viewpos.xyz, _normal);\n",
            vc1);
    else
      emitf(out, "    o.color1 = %s;\n", vc1);
  }
  // numColorChans gates channel output (Dolphin WriteVertexBody), regardless of
  // whether the channel was lit or passthrough.
  if (key.num_color_chans == 0u)
    emit(out, "    o.color0 = vec4f(0.0);\n");
  if (emit_color1 && key.num_color_chans <= 1u)
    emit(out, "    o.color1 = vec4f(0.0);\n");

  for (std::uint32_t i = 0; i < key.num_tex_gens; ++i) {
    const TexGenKey& tg = key.tex_gens[i];
    emit(out, "    {\n");
    emit(out, "        var coord = vec4f(0.0, 0.0, 1.0, 1.0);\n");
    const auto row = static_cast<TexSourceRow>(tg.sourcerow);
    if (row == TexSourceRow::Geom) {
      emit(out, "        coord = vec4f(in.rawpos, 1.0);\n");
    } else if (tg.sourcerow >= static_cast<std::uint8_t>(TexSourceRow::Tex0) &&
               tg.sourcerow <
                   static_cast<std::uint8_t>(TexSourceRow::Tex0) + 4u) {
      const std::uint32_t texnum =
          tg.sourcerow - static_cast<std::uint8_t>(TexSourceRow::Tex0);
      if ((key.uv_mask & (1u << texnum)) != 0u) {
        emitf(out,
              "        coord = vec4f(in.rawtex%u.x, in.rawtex%u.y, 1.0, "
              "1.0);\n",
              texnum, texnum);
      }
    }
    // Other source rows (normal/colors/binormals, tex4-7) are outside the
    // slice; the plan builder counted them and coord stays the default.
    if (tg.inputform == 0u) // AB11
      emit(out, "        coord.z = 1.0;\n");
    const auto tgtype = static_cast<TexGenType>(tg.texgentype);
    if (tgtype == TexGenType::Regular) {
      // Matrix rows: per-vertex TEXMTXIDX selects a matrix-memory row base from
      // the vertex attribute (like PNMTXIDX for position, the byte is the row
      // index into transformmatrices); otherwise use the static per-texgen slot
      // resolved from XF MatrixIndexA/B into texmatrices.
      const bool per_vertex =
          key.has_tex_mtx_idx != 0 && (key.tex_mtx_idx_mask & (1u << i)) != 0u;
      if (per_vertex) {
        emitf(out, "        let ti%u = (in.texmtxidx >> (8u * %uu)) & 0xFFu;\n",
              i, i);
        emitf(out, "        let m%u0 = vsc.transformmatrices[ti%u];\n", i, i);
        emitf(out, "        let m%u1 = vsc.transformmatrices[ti%u + 1u];\n", i, i);
        emitf(out, "        let m%u2 = vsc.transformmatrices[ti%u + 2u];\n", i, i);
        if (tg.projection != 0u)
          emitf(out, "        var uv = vec3f(dot(coord, m%u0), dot(coord, m%u1), "
                     "dot(coord, m%u2));\n",
                i, i, i);
        else
          emitf(out, "        var uv = vec3f(dot(coord, m%u0), dot(coord, m%u1), "
                     "1.0);\n",
                i, i);
      } else if (tg.projection != 0u) { // STQ, 3 rows
        emitf(out,
              "        var uv = vec3f(dot(coord, vsc.texmatrices[%u]), "
              "dot(coord, vsc.texmatrices[%u]), dot(coord, "
              "vsc.texmatrices[%u]));\n",
              3u * i, 3u * i + 1u, 3u * i + 2u);
      } else { // ST, 2 rows
        emitf(out,
              "        var uv = vec3f(dot(coord, vsc.texmatrices[%u]), "
              "dot(coord, vsc.texmatrices[%u]), 1.0);\n",
              3u * i, 3u * i + 1u);
      }
      // GC q==0 special case (Dolphin VertexShaderGen.cpp WriteTexCoordTransforms).
      emit(out, "        if (uv.z == 0.0) { uv = vec3f(clamp(uv.xy * 0.5, "
                "vec2f(-1.0), vec2f(1.0)), uv.z); }\n");
      emitf(out, "        o.uv%u = uv;\n", i);
    } else if (tgtype == TexGenType::Color0) {
      // Dolphin VertexShaderGen: texgen from a lit color channel (toon/ramp),
      // no matrix. rg of the channel become s,t; q = 1.
      emitf(out, "        o.uv%u = vec3f(o.color0.x, o.color0.y, 1.0);\n", i);
    } else if (tgtype == TexGenType::Color1) {
      emitf(out, "        o.uv%u = vec3f(o.color1.x, o.color1.y, 1.0);\n", i);
    } else { // EmbossMap
      // Dolphin VertexShaderGen: add the light dir (view space) projected onto
      // the view-space tangent/binormal to the emboss-source texgen's coords.
      // Tangent/binormal go to view space via the normal matrix (rows 3-5).
      emitf(out, "        let tn%u = vec3f("
                 "dot(vsc.posnormalmatrix[3].xyz, %s), "
                 "dot(vsc.posnormalmatrix[4].xyz, %s), "
                 "dot(vsc.posnormalmatrix[5].xyz, %s));\n",
            i, tangent_in, tangent_in, tangent_in);
      emitf(out, "        let bn%u = vec3f("
                 "dot(vsc.posnormalmatrix[3].xyz, %s), "
                 "dot(vsc.posnormalmatrix[4].xyz, %s), "
                 "dot(vsc.posnormalmatrix[5].xyz, %s));\n",
            i, binormal_in, binormal_in, binormal_in);
      emitf(out, "        let ld%u = normalize(vsc.lights[%uu].pos.xyz - "
                 "viewpos.xyz);\n",
            i, static_cast<unsigned>(tg.embosslightshift));
      emitf(out, "        o.uv%u = o.uv%u + vec3f(dot(ld%u, tn%u), "
                 "dot(ld%u, bn%u), 0.0);\n",
            i, static_cast<unsigned>(tg.embosssourceshift), i, i, i, i);
    }
    emit(out, "    }\n");
  }
  emit(out, "    return o;\n}\n\n");

  // Fragment. TEV path = ported integer combiner; else the S12/S13 passthrough
  // (kept byte-identical for keys whose combiner regs were never captured).
  if (tev) {
    emit_tev_fragment(out, key);
    return out;
  }
  emit(out, "@fragment\nfn fs_main(in: VertexOut) -> @location(0) vec4f {\n");
  emit(out, "    var prev = in.color0;\n");
  if (key.textured != 0) {
    if (key.num_tex_gens > 0 && key.tex_gens[0].projection != 0u) {
      emit(out, "    let uv = in.uv0.xy / max(in.uv0.z, 1e-6);\n");
    } else {
      emit(out, "    let uv = in.uv0.xy;\n");
    }
    emit(out, "    prev = textureSample(tex0, samp0, uv);\n");
  }
  emit(out, "    return prev;\n}\n");
  return out;
}

// --- Register state ----------------------------------------------------------

void GxCoreState::reset() { *this = GxCoreState{}; }

void GxCoreState::apply(const ar::RenderStatePacket& state) {
  using ar::RenderStateKind;
  switch (state.kind) {
  case RenderStateKind::BpReg:
    if (state.index < 256u) {
      bp_regs_[state.index] = state.value;
      bp_valid_[state.index] = true;
      // TEV color/konst registers (BP 0xE0-0xE7, TevReg RA/BG). The TevRegType
      // bit (23) selects konst vs tev-color; the two alias the same address, so
      // route each half to its own store to keep both (BPMemory.h TevReg).
      if (state.index >= 0xE0u && state.index <= 0xE7u) {
        const std::uint32_t idx = (state.index - 0xE0u) / 2u;
        const bool is_bg = ((state.index - 0xE0u) & 1u) != 0u;
        auto sx11 = [](std::uint32_t v) {
          return static_cast<std::int32_t>(v << 21) >> 21; // sign-extend 11 bits
        };
        std::int32_t(&dst)[4] =
            bits(state.value, 1, 23) != 0u ? konst_color_[idx] : tev_color_[idx];
        if (!is_bg) {
          dst[0] = sx11(bits(state.value, 11, 0));  // red
          dst[3] = sx11(bits(state.value, 11, 12)); // alpha
        } else {
          dst[2] = sx11(bits(state.value, 11, 0));  // blue
          dst[1] = sx11(bits(state.value, 11, 12)); // green
        }
      }
    }
    break;
  case RenderStateKind::CpVcd:
    if (state.index == 0u) {
      vcd_lo_ = state.value;
      vcd_lo_valid_ = true;
    } else {
      vcd_hi_ = state.value;
      vcd_hi_valid_ = true;
    }
    break;
  case RenderStateKind::CpVat:
    if (state.index < 8u && state.aux0 < 3u) {
      vat_[state.index][state.aux0] = state.value;
      vat_valid_[state.index] |= static_cast<std::uint8_t>(1u << state.aux0);
    }
    break;
  default:
    break; // arrays/cull/etc. are handled by the inner consumer
  }
}

// --- Vertex decode (Dolphin VertexLoader semantics) ---------------------------

namespace {

// One payload element in GC attribute order. attr uses CP array numbering
// (0 pos, 1 nrm, 2/3 colors, 4-11 tex); matrix-index bytes use kMatIdx.
struct WalkEntry {
  enum Kind : std::uint8_t {
    kPosMtxIdx,
    kTexMtxIdx,
    kPos,
    kNormal,
    kColor,
    kTex,
  };
  Kind kind = kPos;
  std::uint8_t attr = 0;      // CP array index for indexed fetch
  std::uint8_t vcd_type = 0;  // 1 direct, 2 idx8, 3 idx16
  std::uint8_t format = 0;    // ComponentFormat / ColorFormat
  std::uint8_t count = 0;     // components (pos 2/3, tex 1/2, normal 3/9)
  std::uint8_t frac = 0;
  std::uint8_t out_slot = 0;  // color: 0/1; tex: 0-7
  std::uint32_t element_size = 0; // bytes of one element in the source array
};

bool component_scalar_size(std::uint32_t format, std::uint32_t* out) {
  switch (format) {
  case 0u: // u8
  case 1u: // s8
    *out = 1u;
    return true;
  case 2u: // u16
  case 3u: // s16
    *out = 2u;
    return true;
  case 4u: // f32
    *out = 4u;
    return true;
  default:
    return false;
  }
}

bool color_element_size(std::uint32_t format, std::uint32_t* out) {
  switch (format) {
  case 0u: // RGB565
  case 3u: // RGBA4444
    *out = 2u;
    return true;
  case 1u: // RGB888
  case 4u: // RGBA6666
    *out = 3u;
    return true;
  case 2u: // RGB888x
  case 5u: // RGBA8888
    *out = 4u;
    return true;
  default:
    return false;
  }
}

struct WalkLayout {
  WalkEntry entries[24];
  std::uint32_t entry_count = 0;
  std::uint32_t vertex_size = 0;
  bool has_pos_mtx_idx = false;
  bool has_tex_mtx_idx = false;
  bool has_normal = false;
  bool has_nbt = false; // normal attr carries binormal+tangent (emboss inputs)
  bool has_color[2] = {false, false};
  std::uint8_t uv_mask = 0;          // tex0-7 presence
  std::uint8_t tex_mtx_idx_mask = 0; // per-vertex TEXMTXIDX per texgen (0..3)
};

// Derive the payload walk from raw VCD/VAT (CPMemory.h bit positions).
bool derive_walk(std::uint32_t vcd_lo, std::uint32_t vcd_hi,
                 const std::uint32_t vat[3], WalkLayout& out) {
  out = WalkLayout{};
  std::uint32_t offset = 0;
  auto add = [&](const WalkEntry& entry, std::uint32_t payload_size) {
    out.entries[out.entry_count++] = entry;
    offset += payload_size;
  };

  if (bits(vcd_lo, 1, 0) != 0u) { // PosMatIdx
    out.has_pos_mtx_idx = true;
    add({.kind = WalkEntry::kPosMtxIdx, .vcd_type = 1}, 1u);
  }
  for (std::uint32_t t = 0; t < 8; ++t) { // TexMatIdx0-7
    if (bits(vcd_lo, 1, 1 + t) != 0u) {
      out.has_tex_mtx_idx = true;
      if (t < kMaxTexGens)
        out.tex_mtx_idx_mask |= static_cast<std::uint8_t>(1u << t);
      add({.kind = WalkEntry::kTexMtxIdx,
           .vcd_type = 1,
           .out_slot = static_cast<std::uint8_t>(t)},
          1u);
    }
  }

  auto index_size = [](std::uint32_t type) -> std::uint32_t {
    return type == 2u ? 1u : 2u;
  };

  // Position (VCD bits 9-10; VAT g0 bits 0-8).
  {
    const std::uint32_t type = bits(vcd_lo, 2, 9);
    if (type == 0u)
      return false; // a draw without position is malformed
    const std::uint32_t elements = bits(vat[0], 1, 0) != 0u ? 3u : 2u;
    const std::uint32_t format = bits(vat[0], 3, 1);
    const std::uint32_t frac = bits(vat[0], 5, 4);
    std::uint32_t scalar = 0;
    if (!component_scalar_size(format, &scalar))
      return false;
    WalkEntry entry{.kind = WalkEntry::kPos,
                    .attr = 0,
                    .vcd_type = static_cast<std::uint8_t>(type),
                    .format = static_cast<std::uint8_t>(format),
                    .count = static_cast<std::uint8_t>(elements),
                    .frac = static_cast<std::uint8_t>(frac),
                    .element_size = elements * scalar};
    add(entry, type == 1u ? elements * scalar : index_size(type));
  }

  // Normal (VCD bits 11-12; VAT g0 bits 9-12). Decoded for stride only.
  {
    const std::uint32_t type = bits(vcd_lo, 2, 11);
    if (type != 0u) {
      out.has_normal = true;
      const bool nbt = bits(vat[0], 1, 9) != 0u;
      out.has_nbt = nbt;
      const std::uint32_t format = bits(vat[0], 3, 10);
      const bool index3 = bits(vat[0], 1, 31) != 0u;
      std::uint32_t scalar = 0;
      if (!component_scalar_size(format, &scalar))
        return false;
      const std::uint32_t elements = nbt ? 9u : 3u;
      // Normals use a FIXED fractional scale (not the VAT frac field): s8 => 6,
      // s16 => 14, f32 => 0 (GX/Dolphin VertexLoader normal decode).
      const std::uint32_t nfrac = format == 1u ? 6u : format == 3u ? 14u : 0u;
      WalkEntry entry{.kind = WalkEntry::kNormal,
                      .attr = 1,
                      .vcd_type = static_cast<std::uint8_t>(type),
                      .format = static_cast<std::uint8_t>(format),
                      .count = static_cast<std::uint8_t>(elements),
                      .frac = static_cast<std::uint8_t>(nfrac),
                      .element_size = nbt && index3 ? 3u * scalar
                                                    : elements * scalar};
      if (type == 1u) {
        add(entry, elements * scalar);
      } else if (nbt && index3) {
        // Three separate indices, one per 3-component normal.
        for (std::uint32_t n = 0; n < 3; ++n) {
          WalkEntry part = entry;
          part.count = 3;
          add(part, index_size(type));
        }
      } else {
        add(entry, index_size(type));
      }
    }
  }

  // Colors (VCD bits 13-14 / 15-16; VAT g0 bits 13-16 / 17-20).
  for (std::uint32_t c = 0; c < 2; ++c) {
    const std::uint32_t type = bits(vcd_lo, 2, 13 + 2 * c);
    if (type == 0u)
      continue;
    out.has_color[c] = true;
    const std::uint32_t format = bits(vat[0], 3, 14 + 4 * c);
    std::uint32_t element = 0;
    if (!color_element_size(format, &element))
      return false;
    WalkEntry entry{.kind = WalkEntry::kColor,
                    .attr = static_cast<std::uint8_t>(2 + c),
                    .vcd_type = static_cast<std::uint8_t>(type),
                    .format = static_cast<std::uint8_t>(format),
                    .out_slot = static_cast<std::uint8_t>(c),
                    .element_size = element};
    add(entry, type == 1u ? element : index_size(type));
  }

  // Tex coords 0-7 (VCD hi 2 bits each; VAT g0/g1/g2 per CPMemory.h).
  for (std::uint32_t t = 0; t < 8; ++t) {
    const std::uint32_t type = bits(vcd_hi, 2, 2 * t);
    if (type == 0u)
      continue;
    std::uint32_t elements_bit = 0, format = 0, frac = 0;
    switch (t) {
    case 0:
      elements_bit = bits(vat[0], 1, 21);
      format = bits(vat[0], 3, 22);
      frac = bits(vat[0], 5, 25);
      break;
    case 1:
      elements_bit = bits(vat[1], 1, 0);
      format = bits(vat[1], 3, 1);
      frac = bits(vat[1], 5, 4);
      break;
    case 2:
      elements_bit = bits(vat[1], 1, 9);
      format = bits(vat[1], 3, 10);
      frac = bits(vat[1], 5, 13);
      break;
    case 3:
      elements_bit = bits(vat[1], 1, 18);
      format = bits(vat[1], 3, 19);
      frac = bits(vat[1], 5, 22);
      break;
    case 4:
      elements_bit = bits(vat[1], 1, 27);
      format = bits(vat[1], 3, 28);
      frac = bits(vat[2], 5, 0);
      break;
    case 5:
      elements_bit = bits(vat[2], 1, 5);
      format = bits(vat[2], 3, 6);
      frac = bits(vat[2], 5, 9);
      break;
    case 6:
      elements_bit = bits(vat[2], 1, 14);
      format = bits(vat[2], 3, 15);
      frac = bits(vat[2], 5, 18);
      break;
    case 7:
      elements_bit = bits(vat[2], 1, 23);
      format = bits(vat[2], 3, 24);
      frac = bits(vat[2], 5, 27);
      break;
    }
    const std::uint32_t elements = elements_bit != 0u ? 2u : 1u;
    std::uint32_t scalar = 0;
    if (!component_scalar_size(format, &scalar))
      return false;
    out.uv_mask |= static_cast<std::uint8_t>(1u << t);
    WalkEntry entry{.kind = WalkEntry::kTex,
                    .attr = static_cast<std::uint8_t>(4 + t),
                    .vcd_type = static_cast<std::uint8_t>(type),
                    .format = static_cast<std::uint8_t>(format),
                    .count = static_cast<std::uint8_t>(elements),
                    .frac = static_cast<std::uint8_t>(frac),
                    .out_slot = static_cast<std::uint8_t>(t),
                    .element_size = elements * scalar};
    add(entry, type == 1u ? elements * scalar : index_size(type));
  }

  out.vertex_size = offset;
  return out.entry_count > 0;
}

std::uint32_t read_be(const std::uint8_t* p, std::uint32_t size) {
  std::uint32_t v = 0;
  for (std::uint32_t i = 0; i < size; ++i)
    v = (v << 8) | p[i];
  return v;
}

float decode_component(const std::uint8_t* p, std::uint32_t format,
                       std::uint32_t frac) {
  switch (format) {
  case 0u: // u8
    return static_cast<float>(p[0]) / static_cast<float>(1u << frac);
  case 1u: // s8
    return static_cast<float>(static_cast<std::int8_t>(p[0])) /
           static_cast<float>(1u << frac);
  case 2u: // u16
    return static_cast<float>(read_be(p, 2)) / static_cast<float>(1u << frac);
  case 3u: { // s16
    const auto raw = static_cast<std::int16_t>(read_be(p, 2));
    return static_cast<float>(raw) / static_cast<float>(1u << frac);
  }
  case 4u: { // f32 big-endian
    const std::uint32_t v = read_be(p, 4);
    float f;
    std::memcpy(&f, &v, sizeof f);
    return f;
  }
  default:
    return 0.f;
  }
}

std::uint8_t expand5(std::uint32_t v) {
  return static_cast<std::uint8_t>((v << 3) | (v >> 2));
}
std::uint8_t expand6(std::uint32_t v) {
  return static_cast<std::uint8_t>((v << 2) | (v >> 4));
}
std::uint8_t expand4(std::uint32_t v) {
  return static_cast<std::uint8_t>(v * 17u);
}

void decode_color(const std::uint8_t* p, std::uint32_t format, float out[4]) {
  std::uint8_t r = 255, g = 255, b = 255, a = 255;
  switch (format) {
  case 0u: { // RGB565
    const std::uint32_t v = read_be(p, 2);
    r = expand5(bits(v, 5, 11));
    g = expand6(bits(v, 6, 5));
    b = expand5(bits(v, 5, 0));
    break;
  }
  case 1u: // RGB888
    r = p[0];
    g = p[1];
    b = p[2];
    break;
  case 2u: // RGB888x
    r = p[0];
    g = p[1];
    b = p[2];
    break;
  case 3u: { // RGBA4444
    const std::uint32_t v = read_be(p, 2);
    r = expand4(bits(v, 4, 12));
    g = expand4(bits(v, 4, 8));
    b = expand4(bits(v, 4, 4));
    a = expand4(bits(v, 4, 0));
    break;
  }
  case 4u: { // RGBA6666
    const std::uint32_t v = read_be(p, 3);
    r = expand6(bits(v, 6, 18));
    g = expand6(bits(v, 6, 12));
    b = expand6(bits(v, 6, 6));
    a = expand6(bits(v, 6, 0));
    break;
  }
  case 5u: // RGBA8888
    r = p[0];
    g = p[1];
    b = p[2];
    a = p[3];
    break;
  }
  out[0] = static_cast<float>(r) / 255.f;
  out[1] = static_cast<float>(g) / 255.f;
  out[2] = static_cast<float>(b) / 255.f;
  out[3] = static_cast<float>(a) / 255.f;
}

const ar::ConsumedArrayInput* find_array(const ar::ConsumedDraw& draw,
                                         std::uint32_t attr) {
  for (std::uint32_t i = 0; i < draw.array_input_count; ++i) {
    if (draw.arrays[i].attr == attr)
      return &draw.arrays[i];
  }
  return nullptr;
}

// Matrix-memory row -> 4 floats, from the draw's captured XF matrix state.
// Rows 0-29 live in position_matrices (3 rows per matrix), rows 30-62 in
// tex_matrices. Returns false when any word of the row was never written.
bool load_matrix_row(const ar::ConsumedDraw& draw, std::uint32_t row,
                     float out[4]) {
  if (row < 30u) {
    const std::uint32_t matrix = row / 3u;
    const std::uint32_t base = (row % 3u) * 4u;
    const std::uint16_t mask = draw.position_matrix_valid_mask;
    for (std::uint32_t w = 0; w < 4; ++w)
      out[w] = draw.position_matrices[matrix][base + w];
    return (mask & (1u << matrix)) != 0u;
  }
  if (row < 63u) {
    const std::uint32_t rel = row - 30u;
    const std::uint32_t matrix = rel / 3u;
    const std::uint32_t base = (rel % 3u) * 4u;
    bool valid = true;
    for (std::uint32_t w = 0; w < 4; ++w) {
      out[w] = draw.tex_matrices[matrix][base + w];
      if ((draw.tex_matrix_word_mask[matrix] & (1u << (base + w))) == 0u)
        valid = false;
    }
    return valid;
  }
  out[0] = out[1] = out[2] = out[3] = 0.f;
  return false;
}

void identity_rows(float rows[3][4]) {
  std::memset(rows, 0, sizeof(float) * 12);
  rows[0][0] = 1.f;
  rows[1][1] = 1.f;
  rows[2][2] = 1.f;
}

} // namespace

DrawPlan GxCoreState::build_draw_plan(const ar::ConsumedDraw& draw,
                                      GapCounters& counters,
                                      CachedVertexAttrs* cached) const {
  DrawPlan plan;
  auto skip = [&](const char* reason) {
    plan.ok = false;
    plan.skip_reason = reason;
    ++counters.draws_skipped;
    return plan;
  };

  if (draw.cull_all) {
    ++counters.cull_all_draws;
    return skip("cull-all state");
  }
  if (!vcd_lo_valid_ || !vcd_hi_valid_ || draw.vtx_fmt >= 8u ||
      (vat_valid_[draw.vtx_fmt] & 0x7u) != 0x7u) {
    ++counters.missing_vcd;
    return skip("VCD/VAT not yet seen");
  }
  if ((draw.transform_flags &
       ar::kDrawTransformProjectionValid) == 0u) {
    ++counters.vertex_decode_failures;
    return skip("projection never captured");
  }
  if (draw.vertex_payload.empty()) {
    ++counters.vertex_decode_failures;
    return skip("draw carried no payload");
  }

  // BP-derived pipeline state.
  const std::uint32_t gen_mode = bp_valid_[0x00] ? bp_regs_[0x00] : 0u;
  const std::uint32_t cull = bits(gen_mode, 2, 14);
  if (cull == static_cast<std::uint32_t>(CullMode::All)) {
    ++counters.cull_all_draws;
    return skip("genMode culls all");
  }
  if (bits(gen_mode, 3, 16) != 0u)
    ++counters.indirect_ignored;
  // Alpha test is consumed by the TEV path (below); only count it as ignored
  // when no combiner was captured and we fall back to the passthrough fragment.
  if (!bp_valid_[0xC0] && bp_valid_[0xF3]) {
    const std::uint32_t ac = bp_regs_[0xF3];
    if (bits(ac, 3, 16) != 7u || bits(ac, 3, 19) != 7u)
      ++counters.alpha_compare_ignored;
  }

  // Payload walk from raw VCD/VAT.
  WalkLayout walk;
  if (!derive_walk(vcd_lo_, vcd_hi_, vat_[draw.vtx_fmt], walk)) {
    ++counters.vertex_decode_failures;
    return skip("VCD/VAT walk underivable");
  }
  if (walk.vertex_size != draw.vertex_size) {
    // Layout disagreement with the frontend is a correctness bug, not data.
    ++counters.vertex_decode_failures;
    return skip("walk stride != frontend stride");
  }
  // Shader key.
  ShaderKey& key = plan.pipeline.shader;
  const std::uint32_t xf_numtexgens_reg = 0x103Fu - 0x1018u;
  std::uint32_t num_tex_gens = 0;
  if (draw.xf_reg_mask & (1ull << xf_numtexgens_reg))
    num_tex_gens = draw.xf_regs[xf_numtexgens_reg] & 0xFu;
  else
    num_tex_gens = bits(gen_mode, 4, 0);
  if (num_tex_gens > kMaxTexGens) {
    ++counters.unsupported_texgen;
    num_tex_gens = kMaxTexGens;
  }
  key.num_tex_gens = static_cast<std::uint8_t>(num_tex_gens);
  key.has_pos_mtx_idx = walk.has_pos_mtx_idx ? 1u : 0u;
  key.has_tex_mtx_idx = walk.has_tex_mtx_idx ? 1u : 0u;
  key.tex_mtx_idx_mask = walk.tex_mtx_idx_mask;
  key.has_color0 = walk.has_color[0] ? 1u : 0u;
  key.has_color1 = walk.has_color[1] ? 1u : 0u;
  key.uv_mask = static_cast<std::uint8_t>(walk.uv_mask & 0xFu);
  // Vertex-format N/B/T presence (GC packs all three in one NBT attribute, so
  // binormal/tangent presence follows has_nbt). A lit/emboss draw that omits
  // one substitutes the cached fallback from the uniform instead of a
  // per-vertex input (Dolphin I_CACHED_NORMAL, populated below from the last
  // decoded vertex of a draw that DID carry the attribute).
  key.has_vertex_normal = walk.has_normal ? 1u : 0u;
  key.has_vertex_binormal = walk.has_nbt ? 1u : 0u;
  key.has_vertex_tangent = walk.has_nbt ? 1u : 0u;

  // Lighting key (S15). LitChannel bitfields (XFMemory.h): matsource@0,
  // enablelighting@1, lightMask0_3@2, ambsource@6, diffusefunc@7, attnfunc@9,
  // lightMask4_7@11. chan_regs slot = xf_addr - 0x1009: [0]=numColorChans,
  // [1..4]=amb0/amb1/mat0/mat1, [5..8]=color0/color1/alpha0/alpha1 ctrl.
  auto lit_chan = [&](std::uint32_t slot, LightChanKey& ch) {
    if ((draw.chan_reg_mask & (1u << slot)) == 0u)
      return;
    const std::uint32_t v = draw.chan_regs[slot];
    ch.matsource = static_cast<std::uint8_t>(bits(v, 1, 0));
    ch.enablelighting = static_cast<std::uint8_t>(bits(v, 1, 1));
    ch.ambsource = static_cast<std::uint8_t>(bits(v, 1, 6));
    ch.diffusefunc = static_cast<std::uint8_t>(bits(v, 2, 7));
    ch.attnfunc = static_cast<std::uint8_t>(bits(v, 2, 9));
    const std::uint32_t mask = bits(v, 4, 2) | (bits(v, 4, 11) << 4);
    ch.light_mask = ch.enablelighting ? static_cast<std::uint8_t>(mask) : 0u;
  };
  lit_chan(5u, key.litchan[0]);
  lit_chan(6u, key.litchan[1]);
  lit_chan(7u, key.litchan[2]);
  lit_chan(8u, key.litchan[3]);
  // Record which channel-control regs were actually loaded (slots 5..8 map to
  // litchan 0..3). A captured channel selecting MatSource::Register drives the
  // material register; an unconfigured channel stays vertex-color passthrough.
  for (std::uint32_t j = 0; j < 4u; ++j)
    if (draw.chan_reg_mask & (1u << (5u + j)))
      key.chan_captured_mask |= static_cast<std::uint8_t>(1u << j);
  if (draw.chan_reg_mask & 0x1u) {
    std::uint32_t n = bits(draw.chan_regs[0], 2, 0);
    if (n > 2u)
      n = 2u;
    key.num_color_chans = static_cast<std::uint8_t>(n);
  } else {
    // numColorChans reg (XF 0x1009) never captured. GX games always program it;
    // a 0 here is a capture artifact, not "no channels". Assume both channels
    // present so the WGSL numColorChans gate never spuriously blacks a draw whose
    // count we simply did not observe.
    key.num_color_chans = 2u;
  }
  key.lit_valid = (key.litchan[0].enablelighting || key.litchan[1].enablelighting ||
                   key.litchan[2].enablelighting || key.litchan[3].enablelighting)
                      ? 1u
                      : 0u;
  // Only count normals/lighting as ignored when we fall back to passthrough.
  if (walk.has_normal && key.lit_valid == 0u)
    ++counters.normals_ignored;
  if (key.lit_valid == 0u && (draw.chan_reg_mask & (1u << 5)) &&
      (draw.chan_regs[5] & 0x2u) != 0u)
    ++counters.lighting_ignored;

  for (std::uint32_t i = 0; i < num_tex_gens; ++i) {
    const std::uint32_t reg = (0x1040u - 0x1018u) + i;
    TexGenKey& tg = key.tex_gens[i];
    tg.enabled = 1;
    if (draw.xf_reg_mask & (1ull << reg)) {
      const std::uint32_t info = draw.xf_regs[reg];
      // XFMemory.h TexMtxInfo bitfields.
      tg.projection = static_cast<std::uint8_t>(bits(info, 1, 1));
      tg.inputform = static_cast<std::uint8_t>(bits(info, 1, 2));
      tg.texgentype = static_cast<std::uint8_t>(bits(info, 3, 4));
      tg.sourcerow = static_cast<std::uint8_t>(bits(info, 5, 7));
      tg.embosssourceshift = static_cast<std::uint8_t>(bits(info, 3, 12));
      tg.embosslightshift = static_cast<std::uint8_t>(bits(info, 3, 15));
    } else {
      tg.sourcerow =
          static_cast<std::uint8_t>(TexSourceRow::Tex0) +
          static_cast<std::uint8_t>(i);
    }
    // Regular / Color0 / Color1 / Emboss are all generated (item 5). Emboss
    // needs the NBT binormal/tangent vertex inputs; without them in the layout
    // the offset can't be computed, so count that residual case as a gap.
    if (static_cast<TexGenType>(tg.texgentype) == TexGenType::EmbossMap &&
        !walk.has_nbt)
      ++counters.unsupported_texgen;
  }
  const std::uint32_t tex_format = draw.texture.format;
  if (draw.texture.valid &&
      (tex_format == 0x8u || tex_format == 0x9u || tex_format == 0xAu))
    ++counters.tlut_texture;
  key.textured = (draw.texture.valid && draw.texture.resolved &&
                  num_tex_gens > 0u && draw.texture.width > 0u &&
                  draw.texture.height > 0u)
                     ? 1u
                     : 0u;

  // TEV combiner (S14). Combiner reg 0xC0 present => port the integer TEV;
  // otherwise leave tev_valid 0 and the passthrough fragment renders (used by
  // synthetic slices that never write combiner state).
  if (bp_valid_[0xC0]) {
    key.tev_valid = 1;
    std::uint32_t stages = bits(gen_mode, 4, 10) + 1u; // numtevstages + 1
    if (stages > kMaxTevStages) {
      counters.tev_stages_over += stages - kMaxTevStages;
      stages = kMaxTevStages;
    }
    key.num_tev_stages = static_cast<std::uint8_t>(stages);
    auto swap_table = [&](std::uint32_t id, std::uint8_t out[4]) {
      const std::uint32_t rg = bp_regs_[0xF6u + 2u * id];
      const std::uint32_t ba = bp_regs_[0xF6u + 2u * id + 1u];
      out[0] = static_cast<std::uint8_t>(bits(rg, 2, 0)); // swap_rb -> red
      out[1] = static_cast<std::uint8_t>(bits(rg, 2, 2)); // swap_ga -> green
      out[2] = static_cast<std::uint8_t>(bits(ba, 2, 0)); // blue
      out[3] = static_cast<std::uint8_t>(bits(ba, 2, 2)); // alpha
    };
    for (std::uint32_t n = 0; n < stages; ++n) {
      TevStageKey& ts = key.tev_stages[n];
      const std::uint32_t cc = bp_regs_[0xC0u + 2u * n]; // ColorCombiner
      const std::uint32_t ac = bp_regs_[0xC1u + 2u * n]; // AlphaCombiner
      ts.cc_d = static_cast<std::uint8_t>(bits(cc, 4, 0));
      ts.cc_c = static_cast<std::uint8_t>(bits(cc, 4, 4));
      ts.cc_b = static_cast<std::uint8_t>(bits(cc, 4, 8));
      ts.cc_a = static_cast<std::uint8_t>(bits(cc, 4, 12));
      ts.cc_bias = static_cast<std::uint8_t>(bits(cc, 2, 16));
      ts.cc_op = static_cast<std::uint8_t>(bits(cc, 1, 18));
      ts.cc_clamp = static_cast<std::uint8_t>(bits(cc, 1, 19));
      ts.cc_scale = static_cast<std::uint8_t>(bits(cc, 2, 20));
      ts.cc_dest = static_cast<std::uint8_t>(bits(cc, 2, 22));
      ts.ac_a = static_cast<std::uint8_t>(bits(ac, 3, 13));
      ts.ac_b = static_cast<std::uint8_t>(bits(ac, 3, 10));
      ts.ac_c = static_cast<std::uint8_t>(bits(ac, 3, 7));
      ts.ac_d = static_cast<std::uint8_t>(bits(ac, 3, 4));
      ts.ac_bias = static_cast<std::uint8_t>(bits(ac, 2, 16));
      ts.ac_op = static_cast<std::uint8_t>(bits(ac, 1, 18));
      ts.ac_clamp = static_cast<std::uint8_t>(bits(ac, 1, 19));
      ts.ac_scale = static_cast<std::uint8_t>(bits(ac, 2, 20));
      ts.ac_dest = static_cast<std::uint8_t>(bits(ac, 2, 22));
      // TwoTevStageOrders (BP 0x28 + n/2), even/odd halves.
      const std::uint32_t ord = bp_regs_[0x28u + n / 2u];
      const bool odd = (n & 1u) != 0u;
      ts.tevorders_texmap =
          static_cast<std::uint8_t>(odd ? bits(ord, 3, 12) : bits(ord, 3, 0));
      ts.tevorders_texcoord =
          static_cast<std::uint8_t>(odd ? bits(ord, 3, 15) : bits(ord, 3, 3));
      ts.tevorders_enable =
          static_cast<std::uint8_t>(odd ? bits(ord, 1, 18) : bits(ord, 1, 6));
      ts.tevorders_colorchan =
          static_cast<std::uint8_t>(odd ? bits(ord, 3, 19) : bits(ord, 3, 7));
      if (ts.tevorders_enable != 0u && ts.tevorders_texmap != 0u)
        ++counters.tev_multi_texmap;
      // Konst selectors (AllTevKSels ksel[n/2]).
      const std::uint32_t ksel = bp_regs_[0xF6u + n / 2u];
      ts.ksel_kc =
          static_cast<std::uint8_t>(odd ? bits(ksel, 5, 14) : bits(ksel, 5, 4));
      ts.ksel_ka =
          static_cast<std::uint8_t>(odd ? bits(ksel, 5, 19) : bits(ksel, 5, 9));
      swap_table(bits(ac, 2, 0), ts.ras_swap); // rswap
      swap_table(bits(ac, 2, 2), ts.tex_swap); // tswap
    }
    if (bp_valid_[0xF3]) { // AlphaTest (BP 0xF3)
      const std::uint32_t at = bp_regs_[0xF3];
      key.alpha_comp0 = static_cast<std::uint8_t>(bits(at, 3, 16));
      key.alpha_comp1 = static_cast<std::uint8_t>(bits(at, 3, 19));
      key.alpha_logic = static_cast<std::uint8_t>(bits(at, 2, 22));
    }
  }

  PipelineKey& pipe = plan.pipeline;
  pipe.cull_mode = static_cast<std::uint8_t>(cull);
  const std::uint32_t zmode = bp_valid_[0x40] ? bp_regs_[0x40] : 0x17u;
  pipe.depth_test = static_cast<std::uint8_t>(bits(zmode, 1, 0));
  pipe.depth_func = static_cast<std::uint8_t>(bits(zmode, 3, 1));
  pipe.depth_update = static_cast<std::uint8_t>(bits(zmode, 1, 4));
  const std::uint32_t cmode0 = bp_valid_[0x41] ? bp_regs_[0x41] : 0u;
  pipe.blend_enable = static_cast<std::uint8_t>(bits(cmode0, 1, 0));
  pipe.blend_subtract = static_cast<std::uint8_t>(bits(cmode0, 1, 11));
  pipe.dst_factor = static_cast<std::uint8_t>(bits(cmode0, 3, 5));
  pipe.src_factor = static_cast<std::uint8_t>(bits(cmode0, 3, 8));
  pipe.color_update = static_cast<std::uint8_t>(bits(cmode0, 1, 3));
  pipe.alpha_update = static_cast<std::uint8_t>(bits(cmode0, 1, 4));
  if (bits(cmode0, 1, 1) != 0u)
    ++counters.logic_op_ignored;

  // Topology.
  const auto primitive =
      static_cast<ar::GxPrimitive>(draw.primitive & 0xF8u);
  const std::uint32_t index_count = ar::build_topology_indices(
      primitive, 0u, static_cast<std::uint16_t>(draw.vertex_count),
      &plan.indices);
  if (index_count == 0u) {
    ++counters.vertex_decode_failures;
    return skip("line/point primitive (outside slice)");
  }

  // Vertex decode to the fixed layout.
  plan.vertex_count = draw.vertex_count;
  plan.vertices.assign(
      static_cast<std::size_t>(draw.vertex_count) * kVertexFloats, 0.f);
  const std::uint8_t* payload = draw.vertex_payload.data();
  const std::size_t payload_size = draw.vertex_payload.size();
  for (std::uint32_t v = 0; v < draw.vertex_count; ++v) {
    float* out_vertex = plan.vertices.data() +
                        static_cast<std::size_t>(v) * kVertexFloats;
    // Defaults: color0/1 white, uv 0, posmtx = current matrix's first row.
    out_vertex[4] = out_vertex[5] = out_vertex[6] = out_vertex[7] = 1.f;
    out_vertex[8] = out_vertex[9] = out_vertex[10] = out_vertex[11] = 1.f;
    // NBT part ordinal for index3 (three separate normal/binormal/tangent
    // entries); a single 9-component entry decodes all three at once.
    std::uint32_t normal_part = 0;
    std::uint32_t posmtx_row = draw.current_pn_matrix * 3u;
    std::uint32_t texmtxidx_packed = 0; // one byte per texgen (item-5 TEXMTXIDX)
    std::size_t offset =
        static_cast<std::size_t>(v) * walk.vertex_size;
    for (std::uint32_t e = 0; e < walk.entry_count; ++e) {
      const WalkEntry& entry = walk.entries[e];
      if (offset >= payload_size) {
        ++counters.vertex_decode_failures;
        return skip("payload overrun");
      }
      const std::uint8_t* p = payload + offset;
      const std::uint8_t* element = nullptr;
      std::uint32_t advance = 0;
      if (entry.kind == WalkEntry::kPosMtxIdx) {
        posmtx_row = p[0];
        offset += 1;
        continue;
      }
      if (entry.kind == WalkEntry::kTexMtxIdx) {
        // GX per-vertex TEXMTXIDX: the byte is a matrix-memory row index (GX
        // pre-multiplies by 3, same convention as PNMTXIDX). Pack one byte per
        // texgen so the VS can select transformmatrices[row] per vertex.
        if (entry.out_slot < 4u)
          texmtxidx_packed |= static_cast<std::uint32_t>(p[0])
                              << (8u * entry.out_slot);
        offset += 1;
        continue;
      }
      if (entry.vcd_type == 1u) {
        element = p;
        advance = entry.element_size;
      } else {
        const std::uint32_t idx_size = entry.vcd_type == 2u ? 1u : 2u;
        const std::uint32_t index = read_be(p, idx_size);
        advance = idx_size;
        const ar::ConsumedArrayInput* array = find_array(draw, entry.attr);
        if (array == nullptr || !array->resolved ||
            array->host_data == nullptr) {
          ++counters.vertex_decode_failures;
          return skip("indexed attr has no resolved array");
        }
        const std::uint64_t element_offset =
            static_cast<std::uint64_t>(index) * array->stride;
        if (element_offset + entry.element_size > array->host_available) {
          ++counters.vertex_decode_failures;
          return skip("indexed element outside resolved array");
        }
        element =
            static_cast<const std::uint8_t*>(array->host_data) +
            element_offset;
      }
      offset += advance;

      switch (entry.kind) {
      case WalkEntry::kPos: {
        std::uint32_t scalar = 0;
        component_scalar_size(entry.format, &scalar);
        for (std::uint32_t c = 0; c < entry.count && c < 3u; ++c)
          out_vertex[c] =
              decode_component(element + c * scalar, entry.format, entry.frac);
        if (entry.count == 2u)
          out_vertex[2] = 0.f;
        break;
      }
      case WalkEntry::kColor: {
        float rgba[4];
        decode_color(element, entry.format, rgba);
        float* dst = out_vertex + (entry.out_slot == 0u ? 4u : 8u);
        for (int c = 0; c < 4; ++c)
          dst[c] = rgba[c];
        break;
      }
      case WalkEntry::kTex: {
        if (entry.out_slot < kMaxTexGens) {
          std::uint32_t scalar = 0;
          component_scalar_size(entry.format, &scalar);
          float* dst = out_vertex + 12u + 2u * entry.out_slot;
          dst[0] = decode_component(element, entry.format, entry.frac);
          dst[1] = entry.count == 2u
                       ? decode_component(element + scalar, entry.format,
                                          entry.frac)
                       : 0.f;
        }
        break;
      }
      case WalkEntry::kNormal: {
        // Normal (+ NBT binormal/tangent for emboss texgens). A single 9-part
        // entry (direct or non-index3 indexed) holds N,B,T contiguously; index3
        // arrives as three count==3 entries routed by normal_part.
        std::uint32_t scalar = 0;
        component_scalar_size(entry.format, &scalar);
        auto decode3 = [&](const std::uint8_t* src, std::uint32_t dst_off) {
          float* dst = out_vertex + dst_off / 4u;
          for (std::uint32_t c = 0; c < 3u; ++c)
            dst[c] = decode_component(src + c * scalar, entry.format, entry.frac);
        };
        if (entry.count >= 9u) {
          decode3(element, kVertexNormalOffset);
          decode3(element + 3u * scalar, kVertexBinormalOffset);
          decode3(element + 6u * scalar, kVertexTangentOffset);
          normal_part = 3;
        } else if (normal_part == 0u) {
          decode3(element, kVertexNormalOffset);
          normal_part = 1;
        } else if (normal_part == 1u) {
          decode3(element, kVertexBinormalOffset);
          normal_part = 2;
        } else if (normal_part == 2u) {
          decode3(element, kVertexTangentOffset);
          normal_part = 3;
        }
        break;
      }
      default:
        break;
      }
    }
    std::memcpy(out_vertex + 3, &posmtx_row, sizeof posmtx_row);
    std::memcpy(out_vertex + kVertexTexMtxIdxOffset / 4u, &texmtxidx_packed,
                sizeof texmtxidx_packed);
  }

  // Uniforms.
  VertexShaderConstants& c = plan.constants;
  for (std::uint32_t row = 0; row < 63u; ++row)
    (void)load_matrix_row(draw, row, c.transformmatrices[row]);
  const std::uint32_t pn_row = draw.current_pn_matrix * 3u;
  for (std::uint32_t k = 0; k < 3u; ++k)
    (void)load_matrix_row(draw, pn_row + k, c.posnormalmatrix[k]);
  // Normal matrix (XF 0x400): matrix M = current_pn_matrix, 3 rows of
  // 3 (Dolphin VertexShaderManager normalMatrices[3*(PosNormalMtxIdx&31)] == our
  // per-matrix [M] slot). Fall back to the position rows when it was never
  // captured (keeps the A1 behavior for uncaptured draws).
  {
    const std::uint32_t m = draw.current_pn_matrix;
    const bool nm_valid =
        m < DOL_GX_RECOMP_NORMAL_MATRIX_COUNT &&
        draw.normal_matrix_word_mask[m] ==
            ((1u << DOL_GX_RECOMP_NORMAL_MATRIX_WORDS) - 1u);
    for (std::uint32_t k = 0; k < 3u; ++k) {
      if (nm_valid) {
        c.posnormalmatrix[3u + k][0] = draw.normal_matrices[m][3u * k + 0u];
        c.posnormalmatrix[3u + k][1] = draw.normal_matrices[m][3u * k + 1u];
        c.posnormalmatrix[3u + k][2] = draw.normal_matrices[m][3u * k + 2u];
        c.posnormalmatrix[3u + k][3] = 0.f;
      } else {
        (void)load_matrix_row(draw, pn_row + k, c.posnormalmatrix[3u + k]);
      }
    }
  }

  // Lighting uniforms (S15): 8 XF lights + 4 material/ambient registers. Light
  // words (gx_recomp.h): [3]=RGBA8 color, [4..6]=cosatt, [7..9]=distatt,
  // [10..12]=pos, [13..15]=dir (f32 bit patterns). Lights are read only when a
  // light is enabled (lit_valid); the material registers are read by any channel
  // taking the full path (register material/ambient or lighting). Both are left
  // zero otherwise — the WGSL for such a draw never references them.
  auto unpack_rgba8 = [](std::uint32_t col, std::int32_t out[4]) {
    out[0] = static_cast<std::int32_t>((col >> 24) & 0xFFu);
    out[1] = static_cast<std::int32_t>((col >> 16) & 0xFFu);
    out[2] = static_cast<std::int32_t>((col >> 8) & 0xFFu);
    out[3] = static_cast<std::int32_t>(col & 0xFFu);
  };
  if (key.lit_valid) {
    auto f32_of = [](std::uint32_t raw) {
      float f;
      std::memcpy(&f, &raw, sizeof f);
      return f;
    };
    for (std::uint32_t i = 0; i < 8u; ++i) {
      const std::uint32_t* w = draw.light_words[i];
      unpack_rgba8(w[3], c.lights[i].color);
      for (std::uint32_t k = 0; k < 3u; ++k) {
        c.lights[i].cosatt[k] = f32_of(w[4 + k]);
        c.lights[i].distatt[k] = f32_of(w[7 + k]);
        c.lights[i].pos[k] = f32_of(w[10 + k]);
        c.lights[i].dir[k] = f32_of(w[13 + k]);
      }
    }
  }
  if (channel_lit_path(key, 0u) || channel_lit_path(key, 1u)) {
    // I_MATERIALS: [0]=amb0(0x100A), [1]=amb1(0x100B), [2]=mat0(0x100C),
    // [3]=mat1(0x100D) — chan_regs slots 1..4.
    const std::uint32_t mat_slot[4] = {1u, 2u, 3u, 4u};
    for (std::uint32_t m = 0; m < 4u; ++m) {
      const std::uint32_t col = (draw.chan_reg_mask & (1u << mat_slot[m]))
                                    ? draw.chan_regs[mat_slot[m]]
                                    : 0u;
      unpack_rgba8(col, c.materials[m]);
    }
  }
  // Projection (GC 6-param form; Dolphin VertexShaderManager layout).
  const float* pr = draw.projection;
  if (draw.projection_type == 1u) { // orthographic
    c.projection[0][0] = pr[0];
    c.projection[0][3] = pr[1];
    c.projection[1][1] = pr[2];
    c.projection[1][3] = pr[3];
    c.projection[2][2] = pr[4];
    c.projection[2][3] = pr[5];
    c.projection[3][3] = 1.f;
  } else { // perspective
    c.projection[0][0] = pr[0];
    c.projection[0][2] = pr[1];
    c.projection[1][1] = pr[2];
    c.projection[1][2] = pr[3];
    c.projection[2][2] = pr[4];
    c.projection[2][3] = pr[5];
    c.projection[3][2] = -1.f;
  }
  // Per-texgen matrix rows via XF MatrixIndexA/B (CPMemory.h TMatrixIndexA/B).
  const bool mat_idx_a_valid = (draw.xf_reg_mask & 0x1ull) != 0ull;
  const bool mat_idx_b_valid = (draw.xf_reg_mask & 0x2ull) != 0ull;
  const std::uint32_t mat_idx_a = draw.xf_regs[0];
  const std::uint32_t mat_idx_b = draw.xf_regs[1];
  for (std::uint32_t i = 0; i < key.num_tex_gens; ++i) {
    std::uint32_t row = 60u; // GX_IDENTITY row when never configured
    bool row_known = false;
    if (i < 4u && mat_idx_a_valid) {
      row = bits(mat_idx_a, 6, 6 + 6 * i);
      row_known = true;
    } else if (i >= 4u && mat_idx_b_valid) {
      row = bits(mat_idx_b, 6, 6 * (i - 4u));
      row_known = true;
    }
    float rows[3][4];
    bool resolved = row_known;
    for (std::uint32_t k = 0; k < 3u; ++k)
      resolved = load_matrix_row(draw, row + k, rows[k]) && resolved;
    if (!resolved) {
      // Never-written matrix memory: pass raw ST through (identity) and
      // count it loudly instead of collapsing every UV to zero.
      identity_rows(rows);
      ++counters.unresolved_tex_matrix;
    }
    for (std::uint32_t k = 0; k < 3u; ++k)
      std::memcpy(c.texmatrices[3u * i + k], rows[k], sizeof rows[k]);
  }

  // Viewport + texture.
  if ((draw.transform_flags & ar::kDrawTransformViewportValid) != 0u) {
    plan.viewport_valid = true;
    std::memcpy(plan.viewport, draw.viewport, sizeof plan.viewport);
  }
  if (key.textured != 0u) {
    plan.has_texture = true;
    plan.tex_address = draw.texture.address;
    plan.tex_size = draw.texture.size;
    plan.tex_format = draw.texture.format;
    plan.tex_width = draw.texture.width;
    plan.tex_height = draw.texture.height;
    plan.tex_data = draw.texture.host_data;
    plan.tex_available = draw.texture.host_available;
    // Carry the resolved TLUT palette for CI-format textures (A3 decode
    // consumer). Frontend leaves has_tlut false for non-CI/unresolved.
    plan.has_tlut = draw.texture.has_tlut;
    plan.tlut_address = draw.texture.tlut_address;
    plan.tlut_format = draw.texture.tlut_format;
    plan.tlut_entries = draw.texture.tlut_entries;
    plan.tlut_data = draw.texture.tlut_host_data;
    plan.tlut_available = draw.texture.tlut_host_available;
    // Multi-texmap (63/Mfin): when a TEV combines >1 texmap (THP YUV Y/U/V) the
    // single flat texture above is not enough. Populate the per-texmap set from
    // the per-slot bound textures. texmap_mask stays 0 for <=1 distinct texmap
    // so single-texmap draws keep the flat fast path byte-for-byte.
    const std::uint32_t used = used_texmap_mask(key);
    if (texmap_popcount(used) > 1u) {
      plan.texmap_mask = used;
      for (std::uint32_t t = 0; t < 8u; ++t) {
        if ((used & (1u << t)) == 0u)
          continue;
        const ar::ConsumedTexture& src = draw.textures[t];
        PlanTexture& dst = plan.textures[t];
        dst.valid = src.valid && src.resolved && src.width > 0u &&
                    src.height > 0u;
        dst.address = src.address;
        dst.size = src.size;
        dst.format = src.format;
        dst.width = src.width;
        dst.height = src.height;
        dst.data = src.host_data;
        dst.available = src.host_available;
        dst.has_tlut = src.has_tlut;
        dst.tlut_address = src.tlut_address;
        dst.tlut_format = src.tlut_format;
        dst.tlut_entries = src.tlut_entries;
        dst.tlut_data = src.tlut_host_data;
        dst.tlut_available = src.tlut_host_available;
      }
    }
  }

  // Pixel-shader uniforms (S14): TEV color/konst registers + alpha refs.
  for (std::uint32_t r = 0; r < 4u; ++r)
    for (std::uint32_t ch = 0; ch < 4u; ++ch) {
      plan.pixel_constants.colors[r][ch] = tev_color_[r][ch];
      plan.pixel_constants.kcolors[r][ch] = konst_color_[r][ch];
    }
  if (bp_valid_[0xF3]) {
    plan.pixel_constants.alpha_ref[0] =
        static_cast<std::int32_t>(bits(bp_regs_[0xF3], 8, 0));
    plan.pixel_constants.alpha_ref[1] =
        static_cast<std::int32_t>(bits(bp_regs_[0xF3], 8, 8));
  }

  // Fog (S16, Dolphin PixelShaderManager fog constants). The fog uniform lives
  // in PixelShaderConstants, bound only on the TEV path, so fog emits on TEV
  // draws; a fog-enabled non-TEV draw is counted and renders fog-free.
  if (bp_valid_[0xF1]) {
    const std::uint32_t p3 = bp_regs_[0xF1];
    const std::uint32_t fsel = bits(p3, 3, 21);
    if (fsel != 0u) {
      if (key.tev_valid) {
        key.fog_fsel = static_cast<std::uint8_t>(fsel);
        key.fog_proj = static_cast<std::uint8_t>(bits(p3, 1, 20));
        // A (FogParam0 0xEE) and C (FogParam3 0xF1); the inf/inf NaN case
        // (Dolphin FogParams::GetA/GetC) maps to A=0, C=+-inf.
        const std::uint32_t p0 = bp_valid_[0xEE] ? bp_regs_[0xEE] : 0u;
        const bool nan_case =
            bits(p0, 8, 11) == 255u && bits(p3, 8, 11) == 255u;
        const float inf = std::numeric_limits<float>::infinity();
        plan.pixel_constants.fogf[0] = nan_case ? 0.f : fog_param_float(p0);
        plan.pixel_constants.fogf[1] =
            nan_case ? ((bits(p0, 1, 19) == 0u && bits(p3, 1, 19) == 0u) ? -inf
                                                                         : inf)
                     : fog_param_float(p3);
        plan.pixel_constants.fogi[1] = static_cast<std::int32_t>(
            bp_valid_[0xEF] ? (bp_regs_[0xEF] & 0xFFFFFFu) : 1u);
        plan.pixel_constants.fogi[3] = static_cast<std::int32_t>(
            bp_valid_[0xF0] ? (bp_regs_[0xF0] & 0xFFFFFFu) : 1u);
        const std::uint32_t col = bp_valid_[0xF2] ? bp_regs_[0xF2] : 0u;
        plan.pixel_constants.fogcolor[0] = static_cast<std::int32_t>(bits(col, 8, 16));
        plan.pixel_constants.fogcolor[1] = static_cast<std::int32_t>(bits(col, 8, 8));
        plan.pixel_constants.fogcolor[2] = static_cast<std::int32_t>(bits(col, 8, 0));
        // Range adjust (fogRange base 0xE8 + K[0..4] 0xE9..0xED). Center/width
        // need the viewport (Dolphin PixelShaderManager); fall back to Dolphin's
        // disabled default (center 0, width 1) when off or viewport-less.
        const std::uint32_t base = bp_valid_[0xE8] ? bp_regs_[0xE8] : 0u;
        if (bits(base, 1, 10) != 0u && plan.viewport_valid) {
          key.fog_range = 1;
          const float two_wd = 4.f * plan.viewport[0]; // viewport[0] = wd/2
          const int center = static_cast<int>(bits(base, 10, 0)) - 342;
          float ssc = two_wd != 0.f ? static_cast<float>(center) / two_wd : 0.f;
          plan.pixel_constants.fogf[2] = ssc * 2.f - 1.f;
          plan.pixel_constants.fogf[3] = two_wd;
          for (std::uint32_t i = 0, vi = 0; i < 5u; ++i) {
            const std::uint32_t kw =
                bp_valid_[0xE9u + i] ? bp_regs_[0xE9u + i] : 0u;
            // FogRangeKElement HI=bits(0,12) LO=bits(12,12); GetValue*scale(4).
            plan.pixel_constants.fogrange[vi / 4][vi % 4] =
                static_cast<float>(bits(kw, 12, 12)) / 256.f * 4.f; // LO
            ++vi;
            plan.pixel_constants.fogrange[vi / 4][vi % 4] =
                static_cast<float>(bits(kw, 12, 0)) / 256.f * 4.f; // HI
            ++vi;
          }
        } else {
          plan.pixel_constants.fogf[2] = 0.f;
          plan.pixel_constants.fogf[3] = 1.f;
        }
      } else {
        ++counters.fog_ignored;
      }
    }
  }

  // Cached N/B/T fallback (Dolphin VertexLoaderManager normal_cache /
  // ConstantManager cached_normal). Expose the incoming cross-draw cache in the
  // uniform so a draw whose format omits an attribute reads the last-decoded
  // vertex's value instead of a zero input (normalize(0) = NaN). Then, if THIS
  // draw carried the attribute, advance the cache to its LAST vertex's raw
  // object-space value (Dolphin writes the cache on the loader's m_remaining==0
  // vertex). N/B/T are appended in the fixed vertex layout at these float slots.
  if (cached != nullptr) {
    for (std::uint32_t k = 0; k < 3u; ++k) {
      c.cached_normal[k] = cached->normal[k];
      c.cached_tangent[k] = cached->tangent[k];
      c.cached_binormal[k] = cached->binormal[k];
    }
    if (plan.vertex_count > 0u) {
      const float* last =
          plan.vertices.data() +
          static_cast<std::size_t>(plan.vertex_count - 1u) * kVertexFloats;
      if (walk.has_normal)
        for (std::uint32_t k = 0; k < 3u; ++k)
          cached->normal[k] = last[kVertexNormalOffset / 4u + k];
      if (walk.has_nbt) {
        for (std::uint32_t k = 0; k < 3u; ++k) {
          cached->binormal[k] = last[kVertexBinormalOffset / 4u + k];
          cached->tangent[k] = last[kVertexTangentOffset / 4u + k];
        }
      }
    }
  }

  plan.ok = true;
  ++counters.draws_planned;
  return plan;
}

// --- Sink ---------------------------------------------------------------------

GxCoreSink::GxCoreSink() {
  consumer_.set_streaming(true);
  consumer_.set_draw_observer(&GxCoreSink::on_consumed_draw, this);
}

void GxCoreSink::on_consumed_draw(const ar::ConsumedDraw& draw,
                                  unsigned long long, void* user) {
  auto* self = static_cast<GxCoreSink*>(user);
  if (self->plan_observer_ == nullptr)
    return;
  const DrawPlan plan = self->pending_state_.build_draw_plan(
      draw, self->counters_, &self->cached_attrs_);
  self->plan_observer_(plan, self->plan_observer_user_);
}

bool GxCoreSink::submit_packet(const ar::RenderPacket& packet) {
  if (packet.kind == ar::RenderPacketKind::State) {
    live_state_.apply(packet.state);
    // A BP 0x52 with no copy observer (headless Mode A) is a stubbed copy;
    // when an observer is registered the copy is performed (counted below on
    // its resolved CopyDestination packet, which carries the real params).
    if (packet.state.kind == ar::RenderStateKind::BpReg &&
        packet.state.index == 0x52u && copy_observer_ == nullptr)
      ++counters_.efb_copy_ignored;
  }
  // EFB copy: the resolved CopyDestination carries the copy params. Flush the
  // pending draw FIRST (its geometry must be in the pass the copy resolves),
  // then perform the copy at its true stream position.
  if (packet.kind == ar::RenderPacketKind::Resource &&
      packet.resource.kind == ar::RenderResourceKind::CopyDestination &&
      copy_observer_ != nullptr) {
    consumer_.flush_assembly();
    const ar::RenderResourcePacket& r = packet.resource;
    // BP 0x4F clear R/A, 0x50 clear B/G, 0x51 clear Z (GXSetCopyClear encoding).
    const std::uint32_t c0 = live_state_.bp(0x4Fu);
    const std::uint32_t c1 = live_state_.bp(0x50u);
    const std::uint32_t cz = live_state_.bp(0x51u);
    // Clear write masks (Dolphin BPStructs ClearScreen): cmode0 bits 3/4,
    // zmode bit 4. Default enabled when the trace never wrote the reg
    // (GXInit leaves color/alpha/z update on).
    const bool cmode0_seen = live_state_.bp_valid(0x41u);
    const bool zmode_seen = live_state_.bp_valid(0x40u);
    const std::uint32_t cmode0 = live_state_.bp(0x41u);
    const std::uint32_t zmode = live_state_.bp(0x40u);
    const EfbCopyCommand cmd{
        .dest_address = r.address,
        .byte_size = r.size,
        .format = r.format,
        .src_x = r.copy_src_x,
        .src_y = r.copy_src_y,
        .width = r.width,
        .height = r.height,
        .clear = r.copy_clear != 0u,
        .clear_r = (c0 >> 0u) & 0xFFu,
        .clear_g = (c1 >> 8u) & 0xFFu,
        .clear_b = (c1 >> 0u) & 0xFFu,
        .clear_a = (c0 >> 8u) & 0xFFu,
        .clear_z = cz & 0xFFFFFFu,
        .color_update = !cmode0_seen || ((cmode0 >> 3u) & 1u) != 0u,
        .alpha_update = !cmode0_seen || ((cmode0 >> 4u) & 1u) != 0u,
        .depth_update = !zmode_seen || ((zmode >> 4u) & 1u) != 0u,
    };
    copy_observer_(cmd, copy_observer_user_);
    if (r.format == 0xFu) {
      ++counters_.efb_display_copies;
    } else {
      ++counters_.efb_copies;
      // The frontend folds PE_CONTROL Z24 into the format's GXTexFmt Z bit.
      if ((r.format & 0x10u) != 0u)
        ++counters_.efb_copy_depth;
    }
  }
  // Forward first: a Draw packet makes the PREVIOUS draw span-complete and
  // fires the observer, which must see the state that was current at that
  // previous draw (pending_state_), not this packet's.
  const bool ok = consumer_.submit_packet(packet);
  if (packet.kind == ar::RenderPacketKind::Draw)
    pending_state_ = live_state_;
  return ok;
}

void GxCoreSink::flush_frame() { consumer_.flush_assembly(); }

} // namespace dolruntime::gxcore
