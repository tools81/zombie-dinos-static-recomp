/* Direct3D 11 presenter: scales the 768x240 CD-i frame to any window or
   monitor resolution (up to 4K and beyond) with selectable filters and
   aspect-ratio modes, including blurred side panels for 16:9 displays. */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <string.h>
#include "render.h"
#include "runtime.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dxguid.lib")
#pragma comment(lib, "d3dcompiler.lib")

static ID3D11Device *dev;
static ID3D11DeviceContext *ctx;
static IDXGISwapChain *swap;
static ID3D11RenderTargetView *rtv;
static ID3D11Texture2D *tex;
static ID3D11ShaderResourceView *srv;
static ID3D11VertexShader *vs;
static ID3D11PixelShader *ps, *ps_prep;
static ID3D11Texture2D *tens_tex;          /* structure tensor (Cartoon filters) */
static ID3D11ShaderResourceView *tens_srv;
static ID3D11RenderTargetView *tens_rtv;
static ID3D11Buffer *cbuf;
static ID3D11SamplerState *samp_lin, *samp_point;
static int tex_w, tex_h, tex_mips;
static UINT buf_w, buf_h;
static HWND rwnd;
static UINT sync_interval;           /* 0 = no vsync, n = present every n-th refresh */
void render_set_vsync(int n) { sync_interval = n < 0 ? 0 : (UINT)n; }

typedef struct {
    float rect[4];   /* destination rect in NDC: left, top, right, bottom */
    float src[4];    /* texture w, h, destination w, h (pixels) */
    float prm[4];    /* filter, mip level for backdrop, brightness, mode (0 image, 1 backdrop) */
    float uvx[4];    /* uv offset xy, uv scale zw */
} CB;

static const char *shader_src =
"Texture2D tex : register(t0);\n"
"SamplerState sl : register(s0);\n"
"SamplerState sp : register(s1);\n"
"cbuffer C : register(b0) { float4 rect; float4 src; float4 prm; float4 uvx; };\n"
"struct VO { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
"VO vsmain(uint id : SV_VertexID) {\n"
"  float2 c = float2(id & 1, id >> 1);\n"
"  VO o;\n"
"  o.pos = float4(lerp(rect.x, rect.z, c.x), lerp(rect.y, rect.w, c.y), 0, 1);\n"
"  o.uv = uvx.xy + c * uvx.zw;\n"
"  return o;\n"
"}\n"
/* sharp bilinear: crisp pixels without shimmer at non-integer scales */
"float3 sharp(float2 uv) {\n"
"  float2 ts = src.xy;\n"
"  float2 scale = src.zw / ts;\n"
"  float2 texel = uv * ts;\n"
"  float2 pre = max(floor(scale), 1.0);\n"
"  float2 fl = floor(texel);\n"
"  float2 cd = frac(texel) - 0.5;\n"
"  float2 rr = 0.5 - 0.5 / pre;\n"
"  float2 f = (cd - clamp(cd, -rr, rr)) * pre + 0.5;\n"
"  return tex.SampleLevel(sl, (fl + f) / ts, 0).rgb;\n"
"}\n"
/* Catmull-Rom bicubic with 9 bilinear taps */
"float3 bicubic(float2 uv) {\n"
"  float2 ts = src.xy;\n"
"  float2 sp2 = uv * ts;\n"
"  float2 t1 = floor(sp2 - 0.5) + 0.5;\n"
"  float2 f = sp2 - t1;\n"
"  float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));\n"
"  float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);\n"
"  float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));\n"
"  float2 w3 = f * f * (-0.5 + 0.5 * f);\n"
"  float2 w12 = w1 + w2;\n"
"  float2 o12 = w2 / w12;\n"
"  float2 t0 = (t1 - 1) / ts, t3 = (t1 + 2) / ts, t12 = (t1 + o12) / ts;\n"
"  float3 r = 0;\n"
"  r += tex.SampleLevel(sl, float2(t0.x, t0.y), 0).rgb * w0.x * w0.y;\n"
"  r += tex.SampleLevel(sl, float2(t12.x, t0.y), 0).rgb * w12.x * w0.y;\n"
"  r += tex.SampleLevel(sl, float2(t3.x, t0.y), 0).rgb * w3.x * w0.y;\n"
"  r += tex.SampleLevel(sl, float2(t0.x, t12.y), 0).rgb * w0.x * w12.y;\n"
"  r += tex.SampleLevel(sl, float2(t12.x, t12.y), 0).rgb * w12.x * w12.y;\n"
"  r += tex.SampleLevel(sl, float2(t3.x, t12.y), 0).rgb * w3.x * w12.y;\n"
"  r += tex.SampleLevel(sl, float2(t0.x, t3.y), 0).rgb * w0.x * w3.y;\n"
"  r += tex.SampleLevel(sl, float2(t12.x, t3.y), 0).rgb * w12.x * w3.y;\n"
"  r += tex.SampleLevel(sl, float2(t3.x, t3.y), 0).rgb * w3.x * w3.y;\n"
"  return saturate(r);\n"
"}\n"
/* simple CRT: gaussian scanlines + aperture grille */
"float3 crt(float2 uv, float2 pos) {\n"
"  float ty = uv.y * src.y;\n"
"  float yc = floor(ty) + 0.5;\n"
"  float3 a = sharp(float2(uv.x, yc / src.y));\n"
"  float d = ty - yc;\n"
"  float lum = dot(a, float3(0.3, 0.6, 0.1));\n"
"  float width = lerp(6.0, 2.5, lum);\n"
"  float beam = exp(-d * d * width * 4.0);\n"
/* scanlines need several output pixels per source line to look right */
"  float strength = saturate((src.w / src.y - 2.5) / 4.0);\n"
"  float3 c = a * lerp(1.0, beam * 1.35, strength);\n"
"  int m = (int)pos.x % 3;\n"
"  float3 mask = m == 0 ? float3(1.0, 0.85, 0.85) : (m == 1 ? float3(0.85, 1.0, 0.85) : float3(0.85, 0.85, 1.0));\n"
"  mask = lerp(1.0, mask, strength);\n"
"  return saturate(c * mask * lerp(1.0, 1.1, strength));\n"
"}\n"
"/* ---- Cartoon HD / HD+ (edge-directed upscaling for flat-shaded art) ----\n"
"   The CD-i frame is 768 x 240 shown at 4:3, so a texel is about 2.4 times\n"
"   taller than wide; geometry below works in \"normal pixel\" units (2 texels\n"
"   wide, 1 line tall, scaled to be square). */\n"
"Texture2D tens : register(t1);\n"
"static const float2 PX = float2(0.5, 1.2);           /* texel size in normal-pixel units */\n"
"float luma(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }\n"
"float3 texel(int2 t) {\n"
"  t = clamp(t, int2(0, 0), int2(src.xy) - 1);\n"
"  return tex.Load(int3(t, 0)).rgb;\n"
"}\n"
"/* prepass (source resolution): smoothed structure tensor Jxx, Jxy, Jyy */\n"
"float4 psprep(VO i) : SV_Target {\n"
"  int2 t = int2(i.pos.xy);\n"
"  float3 J = 0;\n"
"  [unroll] for (int y = -1; y <= 1; y++)\n"
"  [unroll] for (int x = -1; x <= 1; x++) {\n"
"    int2 c = t + int2(x * 2, y);\n"
"    float gx = (luma(texel(c + int2(2, 0))) - luma(texel(c - int2(2, 0)))) / (4.0 * PX.x);\n"
"    float gy = (luma(texel(c + int2(0, 1))) - luma(texel(c - int2(0, 1)))) / (2.0 * PX.y);\n"
"    float w = (x == 0 ? 2.0 : 1.0) * (y == 0 ? 2.0 : 1.0);\n"
"    J += w * float3(gx * gx, gx * gy, gy * gy);\n"
"  }\n"
"  return float4(J / 16.0, 1);\n"
"}\n"
"float3 cartoon(float2 uv, bool plus) {\n"
"  float2 ts = src.xy;\n"
"  float2 p = uv * ts - 0.5;\n"
"  int2 base = int2(floor(p));\n"
"  /* edge orientation from the structure tensor */\n"
"  float3 J = tens.SampleLevel(sl, uv, 0).xyz;\n"
"  float a = J.x, b = J.y, c = J.z;\n"
"  float root = sqrt((a - c) * (a - c) + 4.0 * b * b);\n"
"  float l1 = 0.5 * (a + c + root), l2 = 0.5 * (a + c - root);\n"
"  float2 g = abs(b) > 1e-6 ? normalize(float2(l1 - c, b)) : (a >= c ? float2(1, 0) : float2(0, 1));\n"
"  float2 tg = float2(-g.y, g.x);\n"
"  float coh = (l1 - l2) / (l1 + l2 + 1e-5);\n"
"  float strength = saturate(sqrt(l1) * 8.0);\n"
"  float sa = lerp(plus ? 0.75 : 0.6, plus ? 1.9 : 1.5, coh * strength);   /* along the edge */\n"
"  float sc = lerp(plus ? 0.75 : 0.6, 0.55, strength);                     /* across it */\n"
"  float3 col[24]; float wt[24]; float lu[24];\n"
"  float3 avg = 0; float wsum = 0; float lmin = 1, lmax = 0;\n"
"  [unroll] for (int j = 0; j < 4; j++)\n"
"  [unroll] for (int q = 0; q < 6; q++) {\n"
"    int n = j * 6 + q;\n"
"    int2 t = base + int2(q - 2, j - 1);\n"
"    float2 d = (float2(t) - p) * PX;\n"
"    float al = dot(d, tg) / sa, ac = dot(d, g) / sc;\n"
"    float w = exp(-(al * al + ac * ac));\n"
"    col[n] = texel(t); wt[n] = w; lu[n] = luma(col[n]);\n"
"    avg += col[n] * w; wsum += w;\n"
"    if (w > 0.08) { lmin = min(lmin, lu[n]); lmax = max(lmax, lu[n]); }\n"
"  }\n"
"  avg /= wsum;\n"
"  float contrast = lmax - lmin;\n"
"  if (contrast < 0.04) return avg;                      /* flat area: smooth */\n"
"  /* split the neighbourhood into its light and dark side */\n"
"  float mid = 0.5 * (lmin + lmax);\n"
"  float3 hi = 0, lo = 0; float wh = 1e-5, wl = 1e-5;\n"
"  [unroll] for (int m = 0; m < 24; m++) {\n"
"    if (lu[m] > mid) { hi += col[m] * wt[m]; wh += wt[m]; }\n"
"    else { lo += col[m] * wt[m]; wl += wt[m]; }\n"
"  }\n"
"  hi /= wh; lo /= wl;\n"
"  if (wh < 0.01) hi = avg;\n"
"  if (wl < 0.01) lo = avg;\n"
"  float tt = saturate((luma(avg) - luma(lo)) / max(luma(hi) - luma(lo), 1e-4));\n"
"  /* re-sharpen the transition to about one output pixel */\n"
"  float outpx = src.w / src.y / PX.y;                  /* output pixels per normal pixel */\n"
"  float kw = clamp((plus ? 0.45 : 0.6) / max(outpx, 1.0), plus ? 0.04 : 0.06, 0.5);\n"
"  tt = smoothstep(0.5 - kw, 0.5 + kw, tt);\n"
"  if (plus && luma(lo) < 0.3 && contrast > 0.25) lo *= 0.82;                /* ink lines */\n"
"  float3 r = lerp(lo, hi, tt);\n"
"  r = lerp(avg, r, saturate((contrast - 0.04) * 12.0));\n"
"  if (plus) { float L = luma(r); r = saturate(L + (r - L) * 1.12); }        /* vivid colour */\n"
"  return r;\n"
"}\n"
"float4 psmain(VO i) : SV_Target {\n"
"  if (prm.w > 0.5) {\n"
"    float2 st = exp2(prm.y) / src.xy;\n"
"    float3 b = 0; float wsum = 0;\n"
"    for (int y = -2; y <= 2; y++) for (int x = -2; x <= 2; x++) {\n"
"      float w = exp(-(x * x + y * y) / 4.0);\n"
"      b += tex.SampleLevel(sl, i.uv + float2(x, y) * st, prm.y).rgb * w; wsum += w;\n"
"    }\n"
"    return float4(b / wsum * prm.z, 1);\n"
"  }\n"
"  int f = (int)prm.x;\n"
"  float3 c;\n"
"  if (f == 0) c = sharp(i.uv);\n"
"  else if (f == 1) c = bicubic(i.uv);\n"
"  else if (f == 2) c = crt(i.uv, i.pos.xy);\n"
"  else if (f == 3) c = tex.SampleLevel(sp, i.uv, 0).rgb;\n"
"  else c = cartoon(i.uv, f == 5);\n"
"  return float4(c, 1);\n"
"}\n";

static const char *aspect_names[] = {"4:3 + blurred sides", "4:3", "stretch to fill", "zoom to fill width"};
static const char *filter_names[] = {"sharp", "smooth", "CRT", "nearest", "Cartoon HD", "Cartoon HD+"};
RenderSettings render_settings = {0, 0};

const char *render_aspect_name(int a) { return aspect_names[a % RENDER_ASPECTS]; }
const char *render_filter_name(int f) { return filter_names[f % RENDER_FILTERS]; }

static void release_rtv(void) {
    if (rtv) { ID3D11RenderTargetView_Release(rtv); rtv = NULL; }
}

static int make_rtv(void) {
    ID3D11Texture2D *bb = NULL;
    if (FAILED(IDXGISwapChain_GetBuffer(swap, 0, &IID_ID3D11Texture2D, (void **)&bb))) return -1;
    HRESULT hr = ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)bb, NULL, &rtv);
    ID3D11Texture2D_Release(bb);
    return FAILED(hr) ? -1 : 0;
}

int render_init(HWND hwnd) {
    rwnd = hwnd;
    RECT r;
    GetClientRect(hwnd, &r);
    buf_w = r.right > 0 ? (UINT)r.right : 1;
    buf_h = r.bottom > 0 ? (UINT)r.bottom : 1;
    DXGI_SWAP_CHAIN_DESC sd = {0};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = buf_w;
    sd.BufferDesc.Height = buf_h;
    sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    D3D_FEATURE_LEVEL fl[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    HRESULT hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, fl, 3, D3D11_SDK_VERSION,
                                               &sd, &swap, &dev, NULL, &ctx);
    if (FAILED(hr)) {
        sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        sd.BufferCount = 1;
        hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, fl, 3, D3D11_SDK_VERSION,
                                           &sd, &swap, &dev, NULL, &ctx);
    }
    if (FAILED(hr)) { LOG("Direct3D 11 unavailable (%08lx); using GDI", (unsigned long)hr); return -1; }
    /* we handle Alt+Enter ourselves (borderless fullscreen) */
    {
        IDXGIDevice *xd = NULL; IDXGIAdapter *ad = NULL; IDXGIFactory *fa = NULL;
        if (SUCCEEDED(ID3D11Device_QueryInterface(dev, &IID_IDXGIDevice, (void **)&xd)) &&
            SUCCEEDED(IDXGIDevice_GetAdapter(xd, &ad)) &&
            SUCCEEDED(IDXGIAdapter_GetParent(ad, &IID_IDXGIFactory, (void **)&fa)))
            IDXGIFactory_MakeWindowAssociation(fa, hwnd, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
        if (fa) IDXGIFactory_Release(fa);
        if (ad) IDXGIAdapter_Release(ad);
        if (xd) IDXGIDevice_Release(xd);
    }
    if (make_rtv()) return -1;

    ID3DBlob *vb = NULL, *pb = NULL, *err = NULL;
    size_t sl = strlen(shader_src);
    if (FAILED(D3DCompile(shader_src, sl, "present", NULL, NULL, "vsmain", "vs_4_0", 0, 0, &vb, &err)) ||
        FAILED(D3DCompile(shader_src, sl, "present", NULL, NULL, "psmain", "ps_4_0", 0, 0, &pb, &err))) {
        LOG("shader compile failed: %s", err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "?");
        return -1;
    }
    ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(vb), ID3D10Blob_GetBufferSize(vb), NULL, &vs);
    ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(pb), ID3D10Blob_GetBufferSize(pb), NULL, &ps);
    {
        ID3DBlob *qb = NULL;
        if (FAILED(D3DCompile(shader_src, sl, "present", NULL, NULL, "psprep", "ps_4_0", 0, 0, &qb, &err))) {
            LOG("shader compile failed: %s", err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "?");
            return -1;
        }
        ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(qb), ID3D10Blob_GetBufferSize(qb), NULL, &ps_prep);
        ID3D10Blob_Release(qb);
    }
    ID3D10Blob_Release(vb);
    ID3D10Blob_Release(pb);

    D3D11_BUFFER_DESC bd = {0};
    bd.ByteWidth = sizeof(CB);
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ID3D11Device_CreateBuffer(dev, &bd, NULL, &cbuf);

    D3D11_SAMPLER_DESC s = {0};
    s.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    s.AddressU = s.AddressV = s.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    s.MaxLOD = D3D11_FLOAT32_MAX;
    ID3D11Device_CreateSamplerState(dev, &s, &samp_lin);
    s.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    ID3D11Device_CreateSamplerState(dev, &s, &samp_point);
    LOG("renderer: Direct3D 11, %ux%u", buf_w, buf_h);
    return 0;
}

static int ensure_texture(int w, int h) {
    if (tex && w == tex_w && h == tex_h) return 0;
    if (srv) { ID3D11ShaderResourceView_Release(srv); srv = NULL; }
    if (tex) { ID3D11Texture2D_Release(tex); tex = NULL; }
    if (tens_srv) { ID3D11ShaderResourceView_Release(tens_srv); tens_srv = NULL; }
    if (tens_rtv) { ID3D11RenderTargetView_Release(tens_rtv); tens_rtv = NULL; }
    if (tens_tex) { ID3D11Texture2D_Release(tens_tex); tens_tex = NULL; }
    D3D11_TEXTURE2D_DESC td = {0};
    td.Width = (UINT)w;
    td.Height = (UINT)h;
    td.MipLevels = 0;                 /* full chain: low mips give the blurred backdrop */
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
    if (FAILED(ID3D11Device_CreateTexture2D(dev, &td, NULL, &tex))) return -1;
    if (FAILED(ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)tex, NULL, &srv))) return -1;
    ID3D11Texture2D_GetDesc(tex, &td);
    tex_w = w; tex_h = h; tex_mips = (int)td.MipLevels;
    {
        D3D11_TEXTURE2D_DESC tdd = {0};
        tdd.Width = (UINT)w; tdd.Height = (UINT)h; tdd.MipLevels = 1; tdd.ArraySize = 1;
        tdd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        tdd.SampleDesc.Count = 1;
        tdd.Usage = D3D11_USAGE_DEFAULT;
        tdd.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        if (FAILED(ID3D11Device_CreateTexture2D(dev, &tdd, NULL, &tens_tex))) return -1;
        if (FAILED(ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)tens_tex, NULL, &tens_srv))) return -1;
        if (FAILED(ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)tens_tex, NULL, &tens_rtv))) return -1;
    }
    return 0;
}

static void draw(float l, float t, float r, float b, float u0, float v0, float us, float vs_, int backdrop,
                 float mip, float bright, float dw, float dh) {
    CB c;
    c.rect[0] = l; c.rect[1] = t; c.rect[2] = r; c.rect[3] = b;
    c.src[0] = (float)tex_w; c.src[1] = (float)tex_h; c.src[2] = dw; c.src[3] = dh;
    c.prm[0] = (float)render_settings.filter; c.prm[1] = mip; c.prm[2] = bright; c.prm[3] = (float)backdrop;
    c.uvx[0] = u0; c.uvx[1] = v0; c.uvx[2] = us; c.uvx[3] = vs_;
    ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)cbuf, 0, NULL, &c, 0, 0);
    ID3D11DeviceContext_Draw(ctx, 4, 0);
}

static int render_frame(const uint32_t *fb, int w, int h, ID3D11RenderTargetView *target, UINT cw, UINT ch) {
    if (ensure_texture(w, h)) return -1;
    ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)tex, 0, NULL, fb, (UINT)w * 4, 0);
    ID3D11DeviceContext_GenerateMips(ctx, srv);

    int cartoon = (render_settings.filter % RENDER_FILTERS) >= 4;
    if (cartoon) {
        /* analysis pass at source resolution */
        ID3D11ShaderResourceView *none[2] = {NULL, NULL};
        ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 2, none);
        ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &tens_rtv, NULL);
        D3D11_VIEWPORT tvp = {0, 0, (float)tex_w, (float)tex_h, 0, 1};
        ID3D11DeviceContext_RSSetViewports(ctx, 1, &tvp);
        ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        ID3D11DeviceContext_IASetInputLayout(ctx, NULL);
        ID3D11DeviceContext_VSSetShader(ctx, vs, NULL, 0);
        ID3D11DeviceContext_PSSetShader(ctx, ps_prep, NULL, 0);
        ID3D11DeviceContext_VSSetConstantBuffers(ctx, 0, 1, &cbuf);
        ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 1, &cbuf);
        ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &srv);
        draw(-1, 1, 1, -1, 0, 0, 1, 1, 0, 0, 1, (float)tex_w, (float)tex_h);
        ID3D11DeviceContext_OMSetRenderTargets(ctx, 0, NULL, NULL);
    }
    float black[4] = {0, 0, 0, 1};
    ID3D11DeviceContext_ClearRenderTargetView(ctx, target, black);
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &target, NULL);
    D3D11_VIEWPORT vp = {0, 0, (float)cw, (float)ch, 0, 1};
    ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ID3D11DeviceContext_IASetInputLayout(ctx, NULL);
    ID3D11DeviceContext_VSSetShader(ctx, vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(ctx, ps, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(ctx, 0, 1, &cbuf);
    ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 1, &cbuf);
    ID3D11SamplerState *ss[2] = {samp_lin, samp_point};
    ID3D11DeviceContext_PSSetSamplers(ctx, 0, 2, ss);
    ID3D11ShaderResourceView *srvs[2] = {srv, cartoon ? tens_srv : NULL};
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 2, srvs);

    /* picture size in pixels for the chosen aspect mode */
    double win_aspect = (double)cw / ch, aspect = 4.0 / 3.0;
    double pw, ph;
    int mode = render_settings.aspect % RENDER_ASPECTS;
    if (mode == 2) { pw = cw; ph = ch; }
    else if (mode == 3) { pw = cw; ph = cw / aspect; if (ph < ch) { ph = ch; pw = ch * aspect; } }
    else if (win_aspect > aspect) { ph = ch; pw = ch * aspect; }
    else { pw = cw; ph = cw / aspect; }
    float l = (float)(-pw / cw), r = (float)(pw / cw), t = (float)(ph / ch), b = (float)(-ph / ch);

    if (mode == 0 && pw < cw - 1) {
        /* blurred, dimmed copy of the picture zoomed to cover the whole window */
        double cover_w = cw, cover_h = cw / aspect;
        if (cover_h < ch) { cover_h = ch; cover_w = ch * aspect; }
        float us = (float)(cw / cover_w), vs2 = (float)(ch / cover_h);
        float mip = (float)(tex_mips > 3 ? 3 : tex_mips - 1);
        draw(-1, 1, 1, -1, (1 - us) / 2, (1 - vs2) / 2, us, vs2, 1, mip, 0.5f, (float)cw, (float)ch);
    }
    draw(l, t, r, b, 0, 0, 1, 1, 0, 0, 1, (float)pw, (float)ph);
    return 0;
}

int render_present(const uint32_t *fb, int w, int h) {
    if (!dev) return -1;
    RECT cr;
    GetClientRect(rwnd, &cr);
    UINT cw = cr.right > 0 ? (UINT)cr.right : 1, ch = cr.bottom > 0 ? (UINT)cr.bottom : 1;
    if (IsIconic(rwnd)) return 0;
    if (cw != buf_w || ch != buf_h) {
        release_rtv();
        ID3D11DeviceContext_OMSetRenderTargets(ctx, 0, NULL, NULL);
        if (FAILED(IDXGISwapChain_ResizeBuffers(swap, 0, cw, ch, DXGI_FORMAT_UNKNOWN, 0))) return -1;
        buf_w = cw; buf_h = ch;
        if (make_rtv()) return -1;
    }
    if (render_frame(fb, w, h, rtv, cw, ch)) return -1;
    IDXGISwapChain_Present(swap, sync_interval, 0);
    return 0;
}

/* debug: render a frame offscreen at ow x oh with the current settings and save it as a BMP */
int render_capture(const uint32_t *fb, int w, int h, int ow, int oh, const char *path) {
    if (!dev) return -1;
    D3D11_TEXTURE2D_DESC td = {0};
    td.Width = (UINT)ow; td.Height = (UINT)oh; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET;
    ID3D11Texture2D *rt = NULL, *st = NULL;
    ID3D11RenderTargetView *v = NULL;
    if (FAILED(ID3D11Device_CreateTexture2D(dev, &td, NULL, &rt))) return -1;
    ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)rt, NULL, &v);
    td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Device_CreateTexture2D(dev, &td, NULL, &st);
    render_frame(fb, w, h, v, (UINT)ow, (UINT)oh);
    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)st, (ID3D11Resource *)rt);
    D3D11_MAPPED_SUBRESOURCE m;
    int ok = -1;
    if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &m))) {
        FILE *f = fopen(path, "wb");
        if (f) {
            BITMAPFILEHEADER bf = {0};
            BITMAPINFOHEADER bi = {0};
            bi.biSize = sizeof bi; bi.biWidth = ow; bi.biHeight = -oh; bi.biPlanes = 1; bi.biBitCount = 32;
            bf.bfType = 0x4D42; bf.bfOffBits = sizeof bf + sizeof bi; bf.bfSize = bf.bfOffBits + ow * oh * 4;
            fwrite(&bf, sizeof bf, 1, f); fwrite(&bi, sizeof bi, 1, f);
            for (int y = 0; y < oh; y++) fwrite((uint8_t *)m.pData + (size_t)y * m.RowPitch, 4, (size_t)ow, f);
            fclose(f);
            ok = 0;
        }
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)st, 0);
    }
    ID3D11RenderTargetView_Release(v);
    ID3D11Texture2D_Release(st);
    ID3D11Texture2D_Release(rt);
    return ok;
}
