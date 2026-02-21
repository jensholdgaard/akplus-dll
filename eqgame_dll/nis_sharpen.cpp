// NIS sharpening module - Contrast Adaptive Sharpening via D3D11 compute shader.
// The project defines CINTERFACE globally for DX8 C-style COM access.
// We need C++ style COM interfaces for D3D11, so undefine it before any includes.
#ifdef CINTERFACE
#undef CINTERFACE
#endif

#include <Windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include "NIS/NIS_Config.h"
#include "nis_sharpen.h"

// ---------------------------------------------------------------------------
// D3DCompile loaded dynamically to avoid linking d3dcompiler.lib
// ---------------------------------------------------------------------------
typedef HRESULT(WINAPI* pD3DCompile)(
    LPCVOID pSrcData,
    SIZE_T SrcDataSize,
    LPCSTR pSourceName,
    const D3D_SHADER_MACRO* pDefines,
    ID3DInclude* pInclude,
    LPCSTR pEntrypoint,
    LPCSTR pTarget,
    UINT Flags1,
    UINT Flags2,
    ID3DBlob** ppCode,
    ID3DBlob** ppErrorMsgs);

// ---------------------------------------------------------------------------
// Embedded CAS compute shader (HLSL)
// ---------------------------------------------------------------------------
static const char* s_casShaderSrc =
    "Texture2D<float4> InputTexture : register(t0);\n"
    "RWTexture2D<float4> OutputTexture : register(u0);\n"
    "\n"
    "cbuffer SharpenParams : register(b0)\n"
    "{\n"
    "    float Sharpness;\n"
    "    float InputWidth;\n"
    "    float InputHeight;\n"
    "    float Padding;\n"
    "};\n"
    "\n"
    "[numthreads(8, 8, 1)]\n"
    "void CSMain(uint3 DTid : SV_DispatchThreadID)\n"
    "{\n"
    "    int2 pos = int2(DTid.xy);\n"
    "    int2 dim = int2((int)InputWidth, (int)InputHeight);\n"
    "    if (pos.x >= dim.x || pos.y >= dim.y) return;\n"
    "\n"
    "    // Sample 3x3 neighborhood\n"
    "    float3 a = InputTexture[int2(max(pos.x-1,0), max(pos.y-1,0))].rgb;\n"
    "    float3 b = InputTexture[int2(pos.x,         max(pos.y-1,0))].rgb;\n"
    "    float3 c = InputTexture[int2(min(pos.x+1,dim.x-1), max(pos.y-1,0))].rgb;\n"
    "    float3 d = InputTexture[int2(max(pos.x-1,0), pos.y)].rgb;\n"
    "    float3 e = InputTexture[pos].rgb;\n"
    "    float3 f = InputTexture[int2(min(pos.x+1,dim.x-1), pos.y)].rgb;\n"
    "    float3 g = InputTexture[int2(max(pos.x-1,0), min(pos.y+1,dim.y-1))].rgb;\n"
    "    float3 h = InputTexture[int2(pos.x,         min(pos.y+1,dim.y-1))].rgb;\n"
    "    float3 i = InputTexture[int2(min(pos.x+1,dim.x-1), min(pos.y+1,dim.y-1))].rgb;\n"
    "\n"
    "    // Per-channel local min/max for contrast detection\n"
    "    float3 mnRGB = min(min(min(d, e), min(f, b)), h);\n"
    "    float3 mxRGB = max(max(max(d, e), max(f, b)), h);\n"
    "    mnRGB = min(mnRGB, min(min(a, c), min(g, i)));\n"
    "    mxRGB = max(mxRGB, max(max(a, c), max(g, i)));\n"
    "\n"
    "    // Adaptive weight: peak = min(mn, 1-mx) / mx, clamped\n"
    "    float3 rcpM = 1.0 / (mxRGB + 0.00001);\n"
    "    float3 amp = saturate(min(mnRGB, 1.0 - mxRGB) * rcpM);\n"
    "    amp = sqrt(amp);\n"
    "\n"
    "    // Scale weight by sharpness (0..1 maps to 0..peak)\n"
    "    float peak = -3.0 * Sharpness + 8.0;\n"
    "    float3 w = amp / peak;\n"
    "\n"
    "    // Weighted cross filter: (b+d+f+h)*w + e  /  (4w + 1)\n"
    "    float3 crossSum = b + d + f + h;\n"
    "    float3 result = (crossSum * w + e) / (4.0 * w + 1.0);\n"
    "\n"
    "    OutputTexture[pos] = float4(saturate(result), InputTexture[pos].a);\n"
    "}\n";

// ---------------------------------------------------------------------------
// Module-level state
// ---------------------------------------------------------------------------
static HMODULE              s_hD3DCompiler   = NULL;
static pD3DCompile          s_pfnD3DCompile  = NULL;
static ID3D11ComputeShader* s_computeShader  = NULL;
static ID3D11Buffer*        s_constantBuffer = NULL;
static UINT                 s_width          = 0;
static UINT                 s_height         = 0;
static bool                 s_initialized    = false;

struct SharpenCB
{
    float Sharpness;
    float InputWidth;
    float InputHeight;
    float Padding;
};

// ---------------------------------------------------------------------------
// InitNISSharpen
// ---------------------------------------------------------------------------
bool InitNISSharpen(ID3D11Device* device, UINT width, UINT height)
{
    if (s_initialized)
        ShutdownNISSharpen();

    if (!device || width == 0 || height == 0)
        return false;

    // Load d3dcompiler dynamically
    s_hD3DCompiler = LoadLibraryA("d3dcompiler_47.dll");
    if (!s_hD3DCompiler)
        return false;

    s_pfnD3DCompile = (pD3DCompile)GetProcAddress(s_hD3DCompiler, "D3DCompile");
    if (!s_pfnD3DCompile)
    {
        FreeLibrary(s_hD3DCompiler);
        s_hD3DCompiler = NULL;
        return false;
    }

    // Compile the compute shader
    ID3DBlob* shaderBlob = NULL;
    ID3DBlob* errorBlob  = NULL;
    HRESULT hr = s_pfnD3DCompile(
        s_casShaderSrc,
        strlen(s_casShaderSrc),
        "CAS_Sharpen",
        NULL,
        NULL,
        "CSMain",
        "cs_5_0",
        D3DCOMPILE_OPTIMIZATION_LEVEL3,
        0,
        &shaderBlob,
        &errorBlob);

    if (FAILED(hr))
    {
        if (errorBlob) errorBlob->Release();
        if (shaderBlob) shaderBlob->Release();
        FreeLibrary(s_hD3DCompiler);
        s_hD3DCompiler = NULL;
        s_pfnD3DCompile = NULL;
        return false;
    }
    if (errorBlob) errorBlob->Release();

    hr = device->CreateComputeShader(
        shaderBlob->GetBufferPointer(),
        shaderBlob->GetBufferSize(),
        NULL,
        &s_computeShader);
    shaderBlob->Release();

    if (FAILED(hr))
    {
        FreeLibrary(s_hD3DCompiler);
        s_hD3DCompiler = NULL;
        s_pfnD3DCompile = NULL;
        return false;
    }

    // Create constant buffer (16 bytes, dynamic)
    D3D11_BUFFER_DESC cbDesc = {};
    cbDesc.ByteWidth      = sizeof(SharpenCB);
    cbDesc.Usage           = D3D11_USAGE_DYNAMIC;
    cbDesc.BindFlags       = D3D11_BIND_CONSTANT_BUFFER;
    cbDesc.CPUAccessFlags  = D3D11_CPU_ACCESS_WRITE;

    hr = device->CreateBuffer(&cbDesc, NULL, &s_constantBuffer);
    if (FAILED(hr))
    {
        s_computeShader->Release();
        s_computeShader = NULL;
        FreeLibrary(s_hD3DCompiler);
        s_hD3DCompiler = NULL;
        s_pfnD3DCompile = NULL;
        return false;
    }

    s_width  = width;
    s_height = height;
    s_initialized = true;
    return true;
}

// ---------------------------------------------------------------------------
// ApplyNISSharpen
// ---------------------------------------------------------------------------
bool ApplyNISSharpen(ID3D11DeviceContext* context,
                     ID3D11Texture2D* inputTexture,
                     ID3D11Texture2D* outputTexture,
                     float sharpness)
{
    if (!s_initialized || !context || !inputTexture || !outputTexture)
        return false;

    HRESULT hr;

    // Create SRV from input texture
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format                    = DXGI_FORMAT_B8G8R8A8_UNORM;
    srvDesc.ViewDimension             = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels       = 1;
    srvDesc.Texture2D.MostDetailedMip = 0;

    ID3D11ShaderResourceView* srv = NULL;
    ID3D11Device* device = NULL;
    context->GetDevice(&device);
    if (!device)
        return false;

    hr = device->CreateShaderResourceView(inputTexture, &srvDesc, &srv);
    if (FAILED(hr))
    {
        device->Release();
        return false;
    }

    // Create UAV from output texture
    D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
    uavDesc.Format             = DXGI_FORMAT_B8G8R8A8_UNORM;
    uavDesc.ViewDimension      = D3D11_UAV_DIMENSION_TEXTURE2D;
    uavDesc.Texture2D.MipSlice = 0;

    ID3D11UnorderedAccessView* uav = NULL;
    hr = device->CreateUnorderedAccessView(outputTexture, &uavDesc, &uav);
    if (FAILED(hr))
    {
        srv->Release();
        device->Release();
        return false;
    }

    // Update constant buffer
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    hr = context->Map(s_constantBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr))
    {
        uav->Release();
        srv->Release();
        device->Release();
        return false;
    }

    SharpenCB cb;
    cb.Sharpness   = (sharpness < 0.0f) ? 0.0f : ((sharpness > 1.0f) ? 1.0f : sharpness);
    cb.InputWidth  = (float)s_width;
    cb.InputHeight = (float)s_height;
    cb.Padding     = 0.0f;
    memcpy(mapped.pData, &cb, sizeof(cb));
    context->Unmap(s_constantBuffer, 0);

    // Dispatch compute shader
    context->CSSetShader(s_computeShader, NULL, 0);
    context->CSSetShaderResources(0, 1, &srv);
    context->CSSetUnorderedAccessViews(0, 1, &uav, NULL);
    context->CSSetConstantBuffers(0, 1, &s_constantBuffer);
    context->Dispatch((s_width + 7) / 8, (s_height + 7) / 8, 1);

    // Clear bindings
    ID3D11ShaderResourceView*  nullSRV = NULL;
    ID3D11UnorderedAccessView* nullUAV = NULL;
    context->CSSetShaderResources(0, 1, &nullSRV);
    context->CSSetUnorderedAccessViews(0, 1, &nullUAV, NULL);
    context->CSSetShader(NULL, NULL, 0);

    // Release temporary views
    uav->Release();
    srv->Release();
    device->Release();

    return true;
}

// ---------------------------------------------------------------------------
// ShutdownNISSharpen
// ---------------------------------------------------------------------------
void ShutdownNISSharpen()
{
    if (s_constantBuffer)
    {
        s_constantBuffer->Release();
        s_constantBuffer = NULL;
    }
    if (s_computeShader)
    {
        s_computeShader->Release();
        s_computeShader = NULL;
    }
    if (s_hD3DCompiler)
    {
        FreeLibrary(s_hD3DCompiler);
        s_hD3DCompiler = NULL;
    }
    s_pfnD3DCompile = NULL;
    s_width  = 0;
    s_height = 0;
    s_initialized = false;
}

// ---------------------------------------------------------------------------
// IsNISSharpenReady
// ---------------------------------------------------------------------------
bool IsNISSharpenReady()
{
    return s_initialized;
}
