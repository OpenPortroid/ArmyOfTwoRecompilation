// post_effects.h - Shaders of the renderer's own post effects (renderer.cpp,
// "Post effects"): ambient occlusion (SSAO, HBAO, GTAO), motion blur, depth of
// field, FXAA and a morphological anti-aliasing in the manner of SMAA 1x.
//
// They work on the game's HDR scene texture (the resolve of the scene before
// its own post processing) and on the scene depth. Positions are rebuilt in
// camera-relative world space from the depth and the inverse of the view
// projection the scene was drawn with (vertex shader constants c0-c3): a pixel
// at view depth w lies at w * ray(ndc), ray = x * R0 + y * R1 + R3 (rows of
// the inverse, see PostConstants in renderer.cpp).
//
// Every pass is a fullscreen triangle over its render target. One source,
// compiled once per entry point (`ps_<name>`); DEPTH_MS selects a
// multisampled depth source for ps_linearize.
#pragma once

namespace ao2::gpu
{
  const char kPostShader[] = R"(
cbuffer PostConstants : register(b0) {
  float4 inv_vp[4];     // (ndc x, ndc y, ndc z, 1) -> camera-relative world (homogeneous), rows
  float4 vp[4];         // camera-relative world -> clip
  float4 prev_vp[4];    // camera-relative world (this frame's camera) -> the previous frame's clip
  float4 target_size;   // render target: w, h, 1/w, 1/h
  float4 view_size;     // the picture (full resolution): w, h, 1/w, 1/h
  float4 depth_params;  // x, y: window z = ndc z * x + y; z: focal length in picture pixels (vertical)
  float4 p0;
  float4 p1;
  uint4 i0;
};

Texture2D<float4> t0 : register(t0);
Texture2D<float4> t1 : register(t1);
Texture2D<float4> t2 : register(t2);
SamplerState linear_clamp : register(s0);
SamplerState point_clamp : register(s1);

static const float kPi = 3.14159265;
static const float kHalfPi = 1.57079633;
static const float kSky = 1.0e7;

struct VO { float4 pos : SV_Position; float2 uv : TEXCOORD0; };

VO vs(uint id : SV_VertexID) {
  VO o;
  o.uv = float2((id << 1) & 2, id & 2);
  o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
  return o;
}

float2 NdcOf(float2 uv) { return float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0); }

// Camera-relative position of view depth 1 along the pixel's ray.
float3 RayOf(float2 uv) {
  float2 n = NdcOf(uv);
  return (n.x * inv_vp[0] + n.y * inv_vp[1] + inv_vp[3]).xyz;
}

// Interleaved gradient noise (per pixel, 0..1).
float Noise(int2 p) { return frac(52.9829189 * frac(dot(float2(p), float2(0.06711056, 0.00583715)))); }

// HDR scene color -> a perceptual luma for edge detection (the scene is
// stored at a quarter of its brightness; p1.x is the scale back).
float Luma(float3 c) {
  float l = max(dot(c, float3(0.299, 0.587, 0.114)) * p1.x, 0.0);
  return sqrt(l / (1.0 + l));
}

// ---------------------------------------------------------------- Depth

#ifdef DEPTH_MS
Texture2DMS<float> depth_ms : register(t0);
#endif

// Depth buffer -> view depth (distance along the view axis, world units).
float ps_linearize(VO i) : SV_Target {
  int2 p = int2(i.pos.xy);
#ifdef DEPTH_MS
  float d = depth_ms.Load(p, 0);
#else
  float d = t0.Load(int3(p, 0)).r;
#endif
  float z = (d - depth_params.y) / depth_params.x;
  float2 n = NdcOf(i.uv);
  float hw = n.x * inv_vp[0].w + n.y * inv_vp[1].w + z * inv_vp[2].w + inv_vp[3].w;
  return hw > 1.0 / kSky ? 1.0 / hw : kSky;
}

// ---------------------------------------------------------------- Ambient occlusion
// Half resolution: t0 = view depth at half resolution (ps_depth_half: the top
// left pixel of each 2x2, the one the occlusion is computed for). Output: x =
// visibility, y = the pixel's view depth (for the depth-aware blur and
// upsampling). p0: x = radius (world units), y = largest radius in (full
// resolution) pixels, z = strength (power), w = distance where it has faded
// out; i0: y = directions / slices, z = steps per direction.
// The taps read the half resolution depth: scattered reads of the full one
// miss the texture cache.

float ps_depth_half(VO i) : SV_Target {
  return t0.Load(int3(int2(i.pos.xy) * 2, 0)).r;
}

float DepthAt(int2 hp) {
  hp = clamp(hp, int2(0, 0), int2(target_size.xy) - 1);
  return t0.Load(int3(hp, 0)).r;
}

// hp: a half resolution pixel, at the centre of its full resolution top left one.
float3 PosAt(int2 hp) {
  hp = clamp(hp, int2(0, 0), int2(target_size.xy) - 1);
  return RayOf((float2(hp * 2) + 0.5) * view_size.zw) * min(t0.Load(int3(hp, 0)).r, kSky);
}

// Normal from the neighbours on the nearer side of each axis (no smearing
// across silhouettes), facing the camera.
float3 NormalAt(int2 hp, float3 P) {
  float3 l = PosAt(hp + int2(-1, 0)), r = PosAt(hp + int2(1, 0));
  float3 u = PosAt(hp + int2(0, -1)), d = PosAt(hp + int2(0, 1));
  float3 dx = dot(r - P, r - P) < dot(P - l, P - l) ? r - P : P - l;
  float3 dy = dot(d - P, d - P) < dot(P - u, P - u) ? d - P : P - u;
  float3 n = normalize(cross(dx, dy));
  return dot(n, P) > 0.0 ? -n : n;
}

// Screen-space ambient occlusion (normal-oriented hemisphere): points in the
// hemisphere over the surface are projected onto the screen; one that lies
// behind what the depth buffer holds there is occluded (counted only when that
// surface is within the radius).
float AoSsao(int2 hp, float3 P, float3 N, float radius_px, float noise) {
  int taps = int(i0.y);
  float r = p0.x;
  float w = dot(float4(P, 1.0), float4(vp[0].w, vp[1].w, vp[2].w, vp[3].w));
  float3 axis = abs(N.z) < 0.9 ? float3(0, 0, 1) : float3(1, 0, 0);
  float3 T = normalize(cross(axis, N)), B = cross(N, T);
  float angle = noise * 2.0 * kPi;
  float3 T2 = T * cos(angle) + B * sin(angle), B2 = B * cos(angle) - T * sin(angle);
  float occlusion = 0.0;
  [loop] for (int k = 0; k < taps; ++k) {
    float u = (float(k) + 0.5) / float(taps);
    float cos_t = 1.0 - u, sin_t = sqrt(1.0 - cos_t * cos_t);
    float phi = float(k) * 2.39996323;
    float len = frac(float(k) * 0.618034 + noise);
    len = lerp(0.1, 1.0, len * len);
    float3 S = P + (T2 * (cos(phi) * sin_t) + B2 * (sin(phi) * sin_t) + N * cos_t) * (r * len);
    float4 clip = S.x * vp[0] + S.y * vp[1] + S.z * vp[2] + vp[3];
    if (clip.w > 1.0) {
      float2 uv = float2(clip.x / clip.w * 0.5 + 0.5, 0.5 - clip.y / clip.w * 0.5);
      float scene = t0.SampleLevel(point_clamp, uv, 0).r;
      float range = smoothstep(0.0, 1.0, r / max(abs(w - scene), 1.0e-3));
      occlusion += (scene < clip.w - 0.03 * r ? 1.0 : 0.0) * range;
    }
  }
  return 1.0 - occlusion / float(taps);
}

// Horizon-based (Bavoil et al.): along each direction the horizon rises over
// the tangent plane; each rise occludes, weighted by distance.
float AoHbao(int2 hp, float3 P, float3 N, float radius_px, float noise) {
  int dirs = int(i0.y), steps = int(i0.z);
  float r2 = p0.x * p0.x;
  float step_px = radius_px * 0.5 / float(steps); // in half resolution pixels
  float ao = 0.0;
  [loop] for (int d = 0; d < dirs; ++d) {
    float angle = (float(d) + noise) * 2.0 * kPi / float(dirs);
    float2 dir = float2(cos(angle), sin(angle));
    float jitter = frac(noise * 7.13 + float(d) * 0.618);
    float top = 0.1; // sine of the tangent bias
    float occlusion = 0.0;
    [loop] for (int s = 0; s < steps; ++s) {
      float2 off = dir * max((float(s) + jitter) * step_px, 1.0 + float(s));
      float3 v = PosAt(hp + int2(round(off))) - P;
      float vv = dot(v, v);
      float h = dot(v, N) * rsqrt(vv + 1.0e-6);
      if (h > top) {
        occlusion += (h - top) * saturate(1.0 - vv / r2);
        top = h;
      }
    }
    ao += occlusion;
  }
  return saturate(1.0 - ao / float(dirs));
}

float IntegrateArc(float h, float n) { return 0.25 * (-cos(2.0 * h - n) + cos(n) + 2.0 * h * sin(n)); }

// Ground-truth ambient occlusion (Jimenez et al.): per slice through the view
// vector the two horizons are found and the cosine-weighted visible arc
// between them is integrated around the normal projected into the slice.
float AoGtao(int2 hp, float3 P, float3 N, float radius_px, float noise) {
  int slices = int(i0.y), steps = int(i0.z);
  float r2 = p0.x * p0.x;
  float3 V = normalize(-P);
  float3 right = normalize(inv_vp[0].xyz), up = normalize(inv_vp[1].xyz);
  float visibility = 0.0;
  [loop] for (int s = 0; s < slices; ++s) {
    float phi = (float(s) + noise) * kPi / float(slices);
    float2 d = float2(cos(phi), sin(phi));                  // on the screen (y down)
    float3 dir3 = d.x * right - d.y * up;
    float3 ortho = dir3 - dot(dir3, V) * V;
    float3 axis = normalize(cross(ortho, V));
    float3 projected = N - axis * dot(N, axis);
    float len = length(projected);
    float cos_n = saturate(dot(projected, V) / max(len, 1.0e-4));
    float n = (dot(ortho, projected) >= 0.0 ? 1.0 : -1.0) * acos(cos_n);
    float c0 = -1.0, c1 = -1.0;
    float jitter = frac(noise * 5.31 + float(s) * 0.618);
    [loop] for (int k = 0; k < steps; ++k) {
      float t = (float(k) + jitter) / float(steps);
      float off = max(t * t * radius_px * 0.5, 1.0 + float(k)); // in half resolution pixels
      float3 v1 = PosAt(hp + int2(round(d * off))) - P;
      float3 v0 = PosAt(hp - int2(round(d * off))) - P;
      float l1 = dot(v1, v1), l0 = dot(v0, v0);
      c1 = max(c1, lerp(-1.0, dot(v1, V) * rsqrt(l1 + 1.0e-6), saturate(1.0 - l1 / r2)));
      c0 = max(c0, lerp(-1.0, dot(v0, V) * rsqrt(l0 + 1.0e-6), saturate(1.0 - l0 / r2)));
    }
    float h0 = n + max(-acos(c0) - n, -kHalfPi);
    float h1 = n + min(acos(c1) - n, kHalfPi);
    visibility += len * (IntegrateArc(h0, n) + IntegrateArc(h1, n));
  }
  return saturate(visibility / float(slices));
}

float2 ps_ao(VO i) : SV_Target {
  int2 hp = int2(i.pos.xy);
  float w = DepthAt(hp);
  if (w >= p0.w) {
    return float2(1.0, min(w, 60000.0));
  }
  float3 P = RayOf((float2(hp * 2) + 0.5) * view_size.zw) * w;
  float3 N = NormalAt(hp, P);
  float radius_px = min(p0.x * depth_params.z / w, p0.y);
  float ao = 1.0;
  if (radius_px >= 2.0) {
    float noise = Noise(hp);
    if (i0.x == 1u) {
      ao = AoSsao(hp, P, N, radius_px, noise);
    } else if (i0.x == 2u) {
      ao = AoHbao(hp, P, N, radius_px, noise);
    } else {
      ao = AoGtao(hp, P, N, radius_px, noise);
    }
  }
  ao = pow(saturate(ao), p0.z);
  ao = lerp(ao, 1.0, saturate((w - p0.w * 0.6) / (p0.w * 0.4)));
  return float2(ao, min(w, 60000.0));
}

// Separable depth-aware blur (half resolution), i0.x: 0 horizontal, 1 vertical.
float2 ps_ao_blur(VO i) : SV_Target {
  int2 p = int2(i.pos.xy);
  int2 dir = i0.x != 0u ? int2(0, 1) : int2(1, 0);
  int2 last = int2(target_size.xy) - 1;
  float2 c = t0.Load(int3(p, 0)).rg;
  float sum = c.x, weights = 1.0;
  [unroll] for (int k = -4; k <= 4; ++k) {
    if (k == 0) {
      continue;
    }
    float2 s = t0.Load(int3(clamp(p + dir * k, int2(0, 0), last), 0)).rg;
    float w = exp2(-float(k * k) / 8.0) * saturate(1.0 - abs(s.y - c.y) / max(c.y, 1.0e-3) * 20.0);
    sum += s.x * w;
    weights += w;
  }
  return float2(sum / weights, c.y);
}

// Full resolution, multiplied into the scene (blend: dest * src): the half
// resolution visibility, upsampled with the weights of the texels whose depth
// is near the pixel's. t0 = visibility (p1.xy = its size, p1.z = scale), t1 = view depth.
float4 ps_ao_apply(VO i) : SV_Target {
  int2 p = int2(i.pos.xy);
  float w = min(t1.Load(int3(p, 0)).r, 60000.0);
  float2 hp = (float2(p) + 0.5) * 0.5 - 0.5;
  int2 b = int2(floor(hp));
  float2 f = hp - float2(b);
  int2 last = int2(p1.xy) - 1;
  float sum = 0.0, weights = 0.0;
  [unroll] for (int k = 0; k < 4; ++k) {
    int2 o = int2(k & 1, k >> 1);
    float2 s = t0.Load(int3(clamp(b + o, int2(0, 0), last), 0)).rg;
    float bilinear = (o.x != 0 ? f.x : 1.0 - f.x) * (o.y != 0 ? f.y : 1.0 - f.y);
    float wt = bilinear * saturate(1.0 - abs(s.y - w) / max(w, 1.0e-3) * 10.0) + 1.0e-4;
    sum += s.x * wt;
    weights += wt;
  }
  float ao = sum / weights * p1.z;
  return float4(ao, ao, ao, 1.0);
}

// ---------------------------------------------------------------- Motion blur
// Camera motion: each pixel's position (from its depth) is projected with the
// previous frame's view projection; the color is averaged along the way.
// t0 = color, t1 = view depth. p0: x = length scale (shutter, frame rate),
// y = longest blur in pixels, z/w = view depth where blur starts / is full
// (the player's character near the camera moves with it); i0.x = samples.
float4 ps_motion_blur(VO i) : SV_Target {
  int2 p = int2(i.pos.xy);
  float4 c = t0.Load(int3(p, 0));
  float w = min(t1.Load(int3(p, 0)).r, 1.0e6);
  float3 P = RayOf(i.uv) * w;
  float4 prev = P.x * prev_vp[0] + P.y * prev_vp[1] + P.z * prev_vp[2] + prev_vp[3];
  if (prev.w <= 1.0e-3) {
    return c;
  }
  float2 prev_ndc = prev.xy / prev.w;
  float2 prev_uv = float2(prev_ndc.x * 0.5 + 0.5, 0.5 - prev_ndc.y * 0.5);
  float2 v = (i.uv - prev_uv) * view_size.xy * p0.x * saturate((w - p0.z) / max(p0.w - p0.z, 1.0));
  float len = length(v);
  if (len > p0.y) {
    v *= p0.y / len;
    len = p0.y;
  }
  if (len < 0.5) {
    return c;
  }
  int n = int(i0.x);
  float noise = Noise(p);
  float4 sum = c;
  float weights = 1.0;
  [loop] for (int k = 0; k < n; ++k) {
    float t = (float(k) + noise) / float(n) - 0.5;
    float2 uv = (float2(p) + 0.5 + v * t) * view_size.zw;
    float wt = t1.SampleLevel(point_clamp, uv, 0).r < p0.z ? 0.0 : 1.0;
    sum += t0.SampleLevel(linear_clamp, uv, 0) * wt;
    weights += wt;
  }
  return sum / weights;
}

// ---------------------------------------------------------------- Depth of field
// Focus: the harmonic mean of the view depth around the screen's centre (the
// crosshair), eased towards over time. t0 = view depth, t1 = the previous
// focus (1x1). p0: x = nearest focus, y = easing (0..1 per frame); i0.x = 1
// takes the new focus at once.
float ps_dof_focus(VO i) : SV_Target {
  float inv = 0.0;
  [unroll] for (int y = -2; y <= 2; ++y) {
    [unroll] for (int x = -2; x <= 2; ++x) {
      float2 uv = float2(0.5, 0.5) + float2(x, y) * float2(0.012, 0.02);
      inv += 1.0 / max(t0.SampleLevel(point_clamp, uv, 0).r, 1.0);
    }
  }
  float focus = clamp(25.0 / inv, p0.x, 1.0e5);
  float prev = t1.Load(int3(0, 0, 0)).r;
  if (i0.x != 0u || !(prev > 0.0)) {
    return focus;
  }
  return lerp(prev, focus, p0.y);
}

// Circle of confusion (pixels) of what lies beyond the focus (only the far
// field blurs: the player's character in front of the aim point stays sharp).
// p0: x = largest radius in pixels, y = focus margin, z = steepness.
float CocOf(float w, float focus) { return p0.x * saturate((w - focus * p0.y) / w * p0.z); }

// Gather over a disc (golden angle spiral): a sample counts when its own
// circle covers the pixel, so sharp foreground does not bleed into the blur.
// t0 = color, t1 = view depth, t2 = focus; i0.x = samples.
float4 ps_dof(VO i) : SV_Target {
  int2 p = int2(i.pos.xy);
  float focus = t2.Load(int3(0, 0, 0)).r;
  float4 c = t0.Load(int3(p, 0));
  float coc = CocOf(t1.Load(int3(p, 0)).r, focus);
  if (coc < 0.5) {
    return c;
  }
  int n = int(i0.x);
  float noise = Noise(p) * 2.0 * kPi;
  float4 sum = c;
  float weights = 1.0;
  [loop] for (int k = 0; k < n; ++k) {
    float r = sqrt((float(k) + 0.5) / float(n)) * coc;
    float a = float(k) * 2.39996323 + noise;
    float2 uv = (float2(p) + 0.5 + float2(cos(a), sin(a)) * r) * view_size.zw;
    float wt = saturate(CocOf(t1.SampleLevel(point_clamp, uv, 0).r, focus) - r + 1.0);
    sum += t0.SampleLevel(linear_clamp, uv, 0) * wt;
    weights += wt;
  }
  return sum / weights;
}

// ---------------------------------------------------------------- FXAA
// Fast approximate anti-aliasing (after Lottes' FXAA 3.11 quality): the
// local edge's direction, the search for its ends along it, and the shift of
// the sample position across it; plus the sub-pixel blur of isolated pixels.
// t0 = color. p0: x = contrast threshold, y = relative threshold, z = sub-pixel amount.

float LumaAt(float2 uv) { return Luma(t0.SampleLevel(linear_clamp, uv, 0).rgb); }

float4 ps_fxaa(VO i) : SV_Target {
  float2 px = view_size.zw;
  float2 uv = i.uv;
  float4 cm = t0.SampleLevel(point_clamp, uv, 0);
  float lm = Luma(cm.rgb);
  float ln = LumaAt(uv + float2(0, -px.y)), ls = LumaAt(uv + float2(0, px.y));
  float le = LumaAt(uv + float2(px.x, 0)), lw = LumaAt(uv + float2(-px.x, 0));
  float lmax = max(lm, max(max(ln, ls), max(le, lw)));
  float lmin = min(lm, min(min(ln, ls), min(le, lw)));
  float range = lmax - lmin;
  if (range < max(p0.x, lmax * p0.y)) {
    return cm;
  }
  float lnw = LumaAt(uv + float2(-px.x, -px.y)), lne = LumaAt(uv + float2(px.x, -px.y));
  float lsw = LumaAt(uv + float2(-px.x, px.y)), lse = LumaAt(uv + float2(px.x, px.y));
  float edge_h = abs(lnw + lsw - 2.0 * lw) + 2.0 * abs(ln + ls - 2.0 * lm) + abs(lne + lse - 2.0 * le);
  float edge_v = abs(lnw + lne - 2.0 * ln) + 2.0 * abs(lw + le - 2.0 * lm) + abs(lsw + lse - 2.0 * ls);
  bool horizontal = edge_h >= edge_v;

  float average = (2.0 * (ln + ls + le + lw) + lnw + lne + lsw + lse) / 12.0;
  float sub = saturate(abs(average - lm) / range);
  sub = smoothstep(0.0, 1.0, sub);
  sub = sub * sub * p0.z;

  float l1 = horizontal ? ln : lw, l2 = horizontal ? ls : le;
  float g1 = abs(l1 - lm), g2 = abs(l2 - lm);
  float step_len = horizontal ? px.y : px.x;
  float local, gradient;
  if (g1 >= g2) {
    step_len = -step_len;
    local = 0.5 * (l1 + lm);
    gradient = g1;
  } else {
    local = 0.5 * (l2 + lm);
    gradient = g2;
  }
  float2 edge_uv = uv + (horizontal ? float2(0, step_len * 0.5) : float2(step_len * 0.5, 0));
  float2 along = horizontal ? float2(px.x, 0) : float2(0, px.y);
  float threshold = gradient * 0.25;
  float2 uv1 = edge_uv - along, uv2 = edge_uv + along;
  float e1 = LumaAt(uv1) - local, e2 = LumaAt(uv2) - local;
  bool done1 = abs(e1) >= threshold, done2 = abs(e2) >= threshold;
  static const float kSteps[10] = {1.0, 1.0, 1.0, 1.5, 2.0, 2.0, 2.0, 2.0, 4.0, 8.0};
  [loop] for (int k = 0; k < 10 && !(done1 && done2); ++k) {
    if (!done1) {
      uv1 -= along * kSteps[k];
      e1 = LumaAt(uv1) - local;
      done1 = abs(e1) >= threshold;
    }
    if (!done2) {
      uv2 += along * kSteps[k];
      e2 = LumaAt(uv2) - local;
      done2 = abs(e2) >= threshold;
    }
  }
  float d1 = horizontal ? uv.x - uv1.x : uv.y - uv1.y;
  float d2 = horizontal ? uv2.x - uv.x : uv2.y - uv.y;
  bool nearer1 = d1 < d2;
  float offset = 0.5 - min(d1, d2) / (d1 + d2);
  bool center_lower = lm < local;
  bool right_side = ((nearer1 ? e1 : e2) < 0.0) != center_lower;
  offset = max(right_side ? offset : 0.0, sub);
  float2 final_uv = uv + (horizontal ? float2(0, offset * step_len) : float2(offset * step_len, 0));
  return t0.SampleLevel(linear_clamp, final_uv, 0);
}

// ---------------------------------------------------------------- SMAA-style morphological AA
// 1. Edges (luma, with local contrast adaptation): r = with the left
//    neighbour, g = with the one above.
// 2. Blending weights: each edge run is followed to its ends (and the edges
//    crossing it there); the silhouette is rebuilt as lines from the ends'
//    crossings to the run's middle (the L, Z and U shapes) and each pixel
//    along the run takes the area of it on its side. r: this pixel takes the
//    one above, g: the one above takes this one, b / a: the same for left.
// 3. Blending with the neighbours by those areas.

float LumaLoad(int2 p) {
  p = clamp(p, int2(0, 0), int2(view_size.xy) - 1);
  return Luma(t0.Load(int3(p, 0)).rgb);
}

float2 ps_smaa_edges(VO i) : SV_Target {
  int2 p = int2(i.pos.xy);
  float l = LumaLoad(p);
  float2 delta = abs(l - float2(LumaLoad(p + int2(-1, 0)), LumaLoad(p + int2(0, -1))));
  float2 edges = step(p0.x, delta);
  if (edges.x + edges.y == 0.0) {
    return float2(0.0, 0.0);
  }
  float2 next = abs(l - float2(LumaLoad(p + int2(1, 0)), LumaLoad(p + int2(0, 1))));
  float2 far = abs(float2(LumaLoad(p + int2(-1, 0)) - LumaLoad(p + int2(-2, 0)),
                          LumaLoad(p + int2(0, -1)) - LumaLoad(p + int2(0, -2))));
  float most = max(max(max(delta.x, delta.y), max(next.x, next.y)), max(far.x, far.y));
  return edges * step(most, 2.0 * delta);
}

float2 EdgeAt(int2 p) {
  bool inside = all(p >= int2(0, 0)) && all(p < int2(view_size.xy));
  return inside ? t0.Load(int3(clamp(p, int2(0, 0), int2(view_size.xy) - 1), 0)).rg : float2(0.0, 0.0);
}

static const int kSearch = 32;

// How many pixels the run of edges (component `c`) goes on past p in `dir`.
int RunLength(int2 p, int2 dir, int c) {
  int n = 0;
  [loop] for (; n < kSearch; ++n) {
    if (EdgeAt(p + dir * (n + 1))[c] < 0.5) {
      break;
    }
  }
  return n;
}

// Height of the silhouette at a run's end: a crossing edge on this pixel's
// side (+) or the neighbour's (-); none or both: the run is straight there.
float EndHeight(float inside, float outside) { return inside > 0.5 ? (outside > 0.5 ? 0.0 : 0.5) : (outside > 0.5 ? -0.5 : 0.0); }

// Area of the line (t0, h0) - (t1, h1) over [a, b].
float SegmentArea(float a, float b, float t0_, float h0, float t1_, float h1) {
  float lo = max(a, t0_), hi = min(b, t1_);
  float slope = (h1 - h0) / (t1_ - t0_);
  return hi > lo ? (h0 + slope * (lo - t0_) + h0 + slope * (hi - t0_)) * 0.5 * (hi - lo) : 0.0;
}

float2 RunArea(int before, int after, float h_start, float h_end) {
  float len = float(before + after + 1), a = float(before), b = a + 1.0;
  float s0 = 0.0, s1 = 0.0;
  if (h_start != 0.0 && h_end != 0.0) {
    s0 = SegmentArea(a, b, 0.0, h_start, len * 0.5, 0.0);
    s1 = SegmentArea(a, b, len * 0.5, 0.0, len, h_end);
  } else if (h_start != 0.0) {
    s0 = SegmentArea(a, b, 0.0, h_start, len, 0.0);
  } else if (h_end != 0.0) {
    s0 = SegmentArea(a, b, 0.0, 0.0, len, h_end);
  }
  return float2(max(s0, 0.0) + max(s1, 0.0), max(-s0, 0.0) + max(-s1, 0.0));
}

float4 ps_smaa_weights(VO i) : SV_Target {
  int2 p = int2(i.pos.xy);
  float2 e = EdgeAt(p);
  float4 w = float4(0.0, 0.0, 0.0, 0.0);
  if (e.g > 0.5) {
    // Along x, between this row and the one above.
    int before = RunLength(p, int2(-1, 0), 1), after = RunLength(p, int2(1, 0), 1);
    int2 first = p - int2(before, 0), past = p + int2(after + 1, 0);
    float h0 = before < kSearch ? EndHeight(EdgeAt(first).r, EdgeAt(first + int2(0, -1)).r) : 0.0;
    float h1 = after < kSearch ? EndHeight(EdgeAt(past).r, EdgeAt(past + int2(0, -1)).r) : 0.0;
    w.rg = RunArea(before, after, h0, h1);
  }
  if (e.r > 0.5) {
    // Along y, between this column and the one on the left.
    int before = RunLength(p, int2(0, -1), 0), after = RunLength(p, int2(0, 1), 0);
    int2 first = p - int2(0, before), past = p + int2(0, after + 1);
    float h0 = before < kSearch ? EndHeight(EdgeAt(first).g, EdgeAt(first + int2(-1, 0)).g) : 0.0;
    float h1 = after < kSearch ? EndHeight(EdgeAt(past).g, EdgeAt(past + int2(-1, 0)).g) : 0.0;
    w.ba = RunArea(before, after, h0, h1);
  }
  return w;
}

// t0 = color, t1 = weights.
float4 ColorLoad(int2 p) { return t0.Load(int3(clamp(p, int2(0, 0), int2(view_size.xy) - 1), 0)); }
float4 WeightLoad(int2 p) {
  bool inside = all(p < int2(view_size.xy));
  return inside ? t1.Load(int3(min(p, int2(view_size.xy) - 1), 0)) : float4(0.0, 0.0, 0.0, 0.0);
}

float4 ps_smaa_blend(VO i) : SV_Target {
  int2 p = int2(i.pos.xy);
  float4 w = WeightLoad(p);
  float top = w.r, left = w.b;
  float bottom = WeightLoad(p + int2(0, 1)).g, right = WeightLoad(p + int2(1, 0)).a;
  float total = top + left + bottom + right;
  float4 c = ColorLoad(p);
  if (total <= 0.0) {
    return c;
  }
  float4 mixed = ColorLoad(p + int2(0, -1)) * top + ColorLoad(p + int2(-1, 0)) * left +
                 ColorLoad(p + int2(0, 1)) * bottom + ColorLoad(p + int2(1, 0)) * right;
  float keep = max(1.0 - total, 0.0);
  return (c * keep + mixed) / (keep + total);
}
)";
} // namespace ao2::gpu
