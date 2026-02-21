// The project defines CINTERFACE globally for DX8 C-style COM access.
// We need C++ style COM interfaces for D3D11/DXGI, so undefine it
// before any includes (Windows.h pulls in COM headers that check it).
#ifdef CINTERFACE
#undef CINTERFACE
#endif

#include <Windows.h>
#include <stdio.h>
#include <string>
#include <dxgi.h>
#include <d3d11.h>
#include "dx_upgrade.h"

// ---------------------------------------------------------------------------
// DX8 Present vtable index (IDirect3DDevice8 vtable layout)
// ---------------------------------------------------------------------------
// [0]  QueryInterface       [1]  AddRef            [2]  Release
// [3]  TestCooperativeLevel [4]  GetAvailableTextureMem
// [5]  ResourceManagerDiscardBytes
// [6]  GetDirect3D          [7]  GetDeviceCaps     [8]  GetDisplayMode
// [9]  GetCreationParameters
// [10] SetCursorProperties  [11] SetCursorPosition [12] ShowCursor
// [13] CreateAdditionalSwapChain
// [14] Reset
// [15] Present  <-- hooked
// [16] GetBackBuffer
// ...
// [30] GetFrontBuffer  <-- used to copy rendered frame
// ---------------------------------------------------------------------------

#define D3D8_VTABLE_INDEX_PRESENT        15
#define D3D8_VTABLE_INDEX_RESET          14
#define D3D8_VTABLE_INDEX_GETBACKBUFFER  16
#define D3D8_VTABLE_INDEX_GETFRONTBUFFER 30
#define D3D8_VTABLE_INDEX_GETDISPLAYMODE 8

// DX8 Present function signature (COM __stdcall method)
typedef HRESULT(__stdcall* D3D8Present_t)(
	void* pDevice,
	const RECT* pSourceRect,
	const RECT* pDestRect,
	HWND hDestWindowOverride,
	const void* pDirtyRegion
);

// DX8 Reset function signature
typedef HRESULT(__stdcall* D3D8Reset_t)(
	void* pDevice,
	void* pPresentationParameters
);

// DX8 GetFrontBuffer signature
// HRESULT GetFrontBuffer(IDirect3DSurface8* pDestSurface)
typedef HRESULT(__stdcall* D3D8GetFrontBuffer_t)(
	void* pDevice,
	void* pDestSurface
);

// DX8 GetDisplayMode signature
// HRESULT GetDisplayMode(D3DDISPLAYMODE* pMode)
typedef HRESULT(__stdcall* D3D8GetDisplayMode_t)(
	void* pDevice,
	void* pMode
);

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------
static bool g_dxUpgradeInitialized = false;
static bool g_dxUpgradeActive = false;
static DXUpgradeConfig g_dxConfig = { false, UPSCALE_OFF, 100, false };

// DX8 device pointer location (in EQGfx_Dx8.dll)
static DWORD g_d3d8DevicePtr = 0;

// Original DX8 vtable function pointers
static D3D8Present_t g_originalPresent = nullptr;
static D3D8Reset_t g_originalReset = nullptr;

// Game window
static HWND g_gameHwnd = 0;

// DXGI / D3D11 objects for upscaling pipeline
static IDXGIFactory1* g_dxgiFactory = nullptr;
static IDXGIAdapter1* g_dxgiAdapter = nullptr;
static ID3D11Device* g_d3d11Device = nullptr;
static ID3D11DeviceContext* g_d3d11Context = nullptr;
static IDXGISwapChain* g_dxgiSwapChain = nullptr;

// D3D11 staging texture for receiving DX8 frame data
static ID3D11Texture2D* g_stagingTexture = nullptr;
// D3D11 render target for upscaled output
static ID3D11Texture2D* g_outputTexture = nullptr;
static ID3D11RenderTargetView* g_outputRTV = nullptr;

// Frame dimensions
static UINT g_frameWidth = 0;
static UINT g_frameHeight = 0;

// NVIDIA GPU detection flag
static bool g_isNvidiaGPU = false;

// GPU adapter name (for logging/status)
static char g_gpuName[128] = { 0 };

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
static bool CreateD3D11Device();
static bool CreateUpscaleResources(UINT width, UINT height);
static void ReleaseUpscaleResources();
static void ReleaseD3D11Device();
static bool HookDX8Present();
static void UnhookDX8Present();
static bool CaptureFrameToStaging();
static bool CopyStagingToSwapChain();

// ---------------------------------------------------------------------------
// DXGI Adapter Enumeration and NVIDIA GPU detection
// ---------------------------------------------------------------------------
bool CheckNvidiaGPU()
{
	IDXGIFactory1* factory = nullptr;
	HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory);
	if (FAILED(hr) || !factory)
		return false;

	IDXGIAdapter1* adapter = nullptr;
	bool foundNvidia = false;

	for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++)
	{
		DXGI_ADAPTER_DESC1 desc;
		if (SUCCEEDED(adapter->GetDesc1(&desc)))
		{
			// NVIDIA vendor ID is 0x10DE
			if (desc.VendorId == 0x10DE)
			{
				foundNvidia = true;
				adapter->Release();
				break;
			}
		}
		adapter->Release();
	}

	factory->Release();
	return foundNvidia;
}

// ---------------------------------------------------------------------------
// D3D11 Device Creation
// ---------------------------------------------------------------------------
static bool CreateD3D11Device()
{
	HRESULT hr;

	// Create DXGI factory
	hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&g_dxgiFactory);
	if (FAILED(hr) || !g_dxgiFactory)
		return false;

	// Find the best adapter (prefer NVIDIA for DLSS)
	IDXGIAdapter1* selectedAdapter = nullptr;
	IDXGIAdapter1* adapter = nullptr;

	for (UINT i = 0; g_dxgiFactory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++)
	{
		DXGI_ADAPTER_DESC1 desc;
		if (SUCCEEDED(adapter->GetDesc1(&desc)))
		{
			if (desc.VendorId == 0x10DE) // NVIDIA
			{
				g_isNvidiaGPU = true;
				if (selectedAdapter)
					selectedAdapter->Release();
				selectedAdapter = adapter;
				// Store GPU name
				WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, g_gpuName, sizeof(g_gpuName), NULL, NULL);
				break;
			}
			if (!selectedAdapter)
			{
				selectedAdapter = adapter;
				// Store GPU name (may be overwritten if NVIDIA is found later)
				WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, g_gpuName, sizeof(g_gpuName), NULL, NULL);
			}
			else
			{
				adapter->Release();
			}
		}
		else
		{
			adapter->Release();
		}
	}

	if (!selectedAdapter)
	{
		// Fallback: use first adapter
		if (g_dxgiFactory->EnumAdapters1(0, &selectedAdapter) != S_OK)
			return false;
	}

	g_dxgiAdapter = selectedAdapter;

	// Create D3D11 device on the selected adapter
	D3D_FEATURE_LEVEL featureLevels[] = {
		D3D_FEATURE_LEVEL_11_1,
		D3D_FEATURE_LEVEL_11_0,
		D3D_FEATURE_LEVEL_10_1,
		D3D_FEATURE_LEVEL_10_0
	};
	D3D_FEATURE_LEVEL achievedLevel;
	UINT createFlags = 0;
#ifdef _DEBUG
	createFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

	hr = D3D11CreateDevice(
		g_dxgiAdapter,
		D3D_DRIVER_TYPE_UNKNOWN, // Must be UNKNOWN when adapter is specified
		NULL,
		createFlags,
		featureLevels,
		ARRAYSIZE(featureLevels),
		D3D11_SDK_VERSION,
		&g_d3d11Device,
		&achievedLevel,
		&g_d3d11Context
	);

	if (FAILED(hr) || !g_d3d11Device)
		return false;

	return true;
}

// ---------------------------------------------------------------------------
// DXGI Swap Chain Creation
// ---------------------------------------------------------------------------
static bool CreateSwapChain(HWND hwnd, UINT width, UINT height)
{
	if (!g_dxgiFactory || !g_d3d11Device || !hwnd)
		return false;

	DXGI_SWAP_CHAIN_DESC scd;
	ZeroMemory(&scd, sizeof(scd));
	scd.BufferCount = 2;
	scd.BufferDesc.Width = width;
	scd.BufferDesc.Height = height;
	scd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; // Matches GDI capture format
	scd.BufferDesc.RefreshRate.Numerator = 0;
	scd.BufferDesc.RefreshRate.Denominator = 1;
	scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	scd.OutputWindow = hwnd;
	scd.SampleDesc.Count = 1;
	scd.SampleDesc.Quality = 0;
	scd.Windowed = TRUE;
	scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

	HRESULT hr = g_dxgiFactory->CreateSwapChain(g_d3d11Device, &scd, &g_dxgiSwapChain);
	return SUCCEEDED(hr) && g_dxgiSwapChain != nullptr;
}

// ---------------------------------------------------------------------------
// Upscale Resources (staging texture for DX8->DX11 copy, output texture)
// ---------------------------------------------------------------------------
static bool CreateUpscaleResources(UINT width, UINT height)
{
	if (!g_d3d11Device)
		return false;

	// Staging texture: CPU-writable, used to upload DX8 frame data to GPU
	D3D11_TEXTURE2D_DESC stagingDesc;
	ZeroMemory(&stagingDesc, sizeof(stagingDesc));
	stagingDesc.Width = width;
	stagingDesc.Height = height;
	stagingDesc.MipLevels = 1;
	stagingDesc.ArraySize = 1;
	stagingDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; // DX8 typically uses BGRA
	stagingDesc.SampleDesc.Count = 1;
	stagingDesc.Usage = D3D11_USAGE_DYNAMIC;
	stagingDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

	HRESULT hr = g_d3d11Device->CreateTexture2D(&stagingDesc, NULL, &g_stagingTexture);
	if (FAILED(hr))
		return false;

	// Output texture: GPU render target for upscaled output
	D3D11_TEXTURE2D_DESC outputDesc;
	ZeroMemory(&outputDesc, sizeof(outputDesc));
	outputDesc.Width = width;
	outputDesc.Height = height;
	outputDesc.MipLevels = 1;
	outputDesc.ArraySize = 1;
	outputDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; // Matches staging/swap chain format
	outputDesc.SampleDesc.Count = 1;
	outputDesc.Usage = D3D11_USAGE_DEFAULT;
	outputDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

	hr = g_d3d11Device->CreateTexture2D(&outputDesc, NULL, &g_outputTexture);
	if (FAILED(hr))
		return false;

	hr = g_d3d11Device->CreateRenderTargetView(g_outputTexture, NULL, &g_outputRTV);
	if (FAILED(hr))
		return false;

	g_frameWidth = width;
	g_frameHeight = height;

	return true;
}

static void ReleaseUpscaleResources()
{
	if (g_outputRTV) { g_outputRTV->Release(); g_outputRTV = nullptr; }
	if (g_outputTexture) { g_outputTexture->Release(); g_outputTexture = nullptr; }
	if (g_stagingTexture) { g_stagingTexture->Release(); g_stagingTexture = nullptr; }
}

// ---------------------------------------------------------------------------
// Release D3D11 Device and DXGI objects
// ---------------------------------------------------------------------------
static void ReleaseD3D11Device()
{
	ReleaseUpscaleResources();
	if (g_dxgiSwapChain) { g_dxgiSwapChain->Release(); g_dxgiSwapChain = nullptr; }
	if (g_d3d11Context) { g_d3d11Context->Release(); g_d3d11Context = nullptr; }
	if (g_d3d11Device) { g_d3d11Device->Release(); g_d3d11Device = nullptr; }
	if (g_dxgiAdapter) { g_dxgiAdapter->Release(); g_dxgiAdapter = nullptr; }
	if (g_dxgiFactory) { g_dxgiFactory->Release(); g_dxgiFactory = nullptr; }
}

// ---------------------------------------------------------------------------
// DX8 → DX11 Frame Capture
//
// Captures the DX8-rendered frame from the game window using GDI (BitBlt),
// then uploads the pixel data into a D3D11 staging texture. This is the most
// reliable method since DX8 surfaces can't be shared directly with DX11.
// ---------------------------------------------------------------------------
static bool CaptureFrameToStaging()
{
	if (!g_d3d11Context || !g_stagingTexture || !g_gameHwnd)
		return false;

	if (g_frameWidth == 0 || g_frameHeight == 0)
		return false;

	// Get a DC for the game window's client area
	HDC hdcWindow = GetDC(g_gameHwnd);
	if (!hdcWindow)
		return false;

	// Create a compatible memory DC and bitmap for BitBlt
	HDC hdcMem = CreateCompatibleDC(hdcWindow);
	if (!hdcMem)
	{
		ReleaseDC(g_gameHwnd, hdcWindow);
		return false;
	}

	// Set up BITMAPINFO for a 32bpp BGRA bitmap matching the frame size
	BITMAPINFO bmi;
	ZeroMemory(&bmi, sizeof(bmi));
	bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	bmi.bmiHeader.biWidth = (LONG)g_frameWidth;
	bmi.bmiHeader.biHeight = -(LONG)g_frameHeight; // top-down
	bmi.bmiHeader.biPlanes = 1;
	bmi.bmiHeader.biBitCount = 32;
	bmi.bmiHeader.biCompression = BI_RGB;

	void* pBits = nullptr;
	HBITMAP hBitmap = CreateDIBSection(hdcMem, &bmi, DIB_RGB_COLORS, &pBits, NULL, 0);
	if (!hBitmap || !pBits)
	{
		DeleteDC(hdcMem);
		ReleaseDC(g_gameHwnd, hdcWindow);
		return false;
	}

	HGDIOBJ hOld = SelectObject(hdcMem, hBitmap);

	// Copy the game window's rendered content into our bitmap
	BOOL bltResult = BitBlt(hdcMem, 0, 0, (int)g_frameWidth, (int)g_frameHeight,
		hdcWindow, 0, 0, SRCCOPY);

	SelectObject(hdcMem, hOld);

	bool success = false;

	if (bltResult)
	{
		// Map the D3D11 staging texture and copy the captured pixels into it
		D3D11_MAPPED_SUBRESOURCE mapped;
		HRESULT hr = g_d3d11Context->Map(g_stagingTexture, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
		if (SUCCEEDED(hr))
		{
			// Copy row by row (source and dest pitches may differ)
			UINT srcPitch = g_frameWidth * 4; // 32bpp = 4 bytes per pixel
			BYTE* pSrc = (BYTE*)pBits;
			BYTE* pDst = (BYTE*)mapped.pData;

			for (UINT y = 0; y < g_frameHeight; y++)
			{
				memcpy(pDst, pSrc, srcPitch);
				pSrc += srcPitch;
				pDst += mapped.RowPitch;
			}

			g_d3d11Context->Unmap(g_stagingTexture, 0);
			success = true;
		}
	}

	// Clean up GDI objects
	DeleteObject(hBitmap);
	DeleteDC(hdcMem);
	ReleaseDC(g_gameHwnd, hdcWindow);

	return success;
}

// ---------------------------------------------------------------------------
// Copy staging texture to the DXGI swap chain back buffer
// ---------------------------------------------------------------------------
static bool CopyStagingToSwapChain()
{
	if (!g_d3d11Context || !g_stagingTexture || !g_dxgiSwapChain)
		return false;

	// Get the swap chain's back buffer
	ID3D11Texture2D* pBackBuffer = nullptr;
	HRESULT hr = g_dxgiSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&pBackBuffer);
	if (FAILED(hr) || !pBackBuffer)
		return false;

	// Copy the staging texture to the back buffer
	// The staging texture is BGRA, back buffer is RGBA — CopyResource handles
	// format conversion if the textures are compatible in dimension/type
	g_d3d11Context->CopyResource(pBackBuffer, g_stagingTexture);

	pBackBuffer->Release();
	return true;
}

// ---------------------------------------------------------------------------
// Hooked DX8 Present
//
// This intercepts every frame presented by the DX8 device. After the original
// DX8 Present completes, we capture the rendered frame via GDI, upload it to
// a D3D11 staging texture, copy to the swap chain back buffer, and present
// the frame through the DX11/DXGI pipeline.
//
// When DLSS or another upscaler is integrated, the upscaling step would be
// inserted between the staging texture upload and the swap chain copy:
//   1. CaptureFrameToStaging()     — DX8 frame → staging texture
//   2. DLSS evaluate               — staging → output texture (upscaled)
//   3. Copy output → swap chain    — present upscaled frame
// ---------------------------------------------------------------------------
static HRESULT __stdcall HookedPresent(
	void* pDevice,
	const RECT* pSourceRect,
	const RECT* pDestRect,
	HWND hDestWindowOverride,
	const void* pDirtyRegion)
{
	// Call the original DX8 Present first
	HRESULT hr = g_originalPresent(pDevice, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion);

	// If DX upgrade is not active, just return
	if (!g_dxUpgradeActive)
		return hr;

	if (!g_d3d11Device || !g_d3d11Context || !g_dxgiSwapChain)
		return hr;

	// Step 1: Capture the DX8-rendered frame into the D3D11 staging texture
	if (!CaptureFrameToStaging())
		return hr;

	// Step 2: (Future) Apply DLSS/FSR upscaling here
	// If upscaleMode != UPSCALE_OFF && g_isNvidiaGPU:
	//   NVSDK_NGX_D3D11_EvaluateFeature(g_d3d11Context, ...)
	//   Copy g_outputTexture → swap chain back buffer
	// Else: pass-through (copy staging directly to swap chain)

	// Step 3: Copy the staging texture (or upscaled output) to the swap chain
	CopyStagingToSwapChain();

	// Step 4: Present the frame through the DXGI swap chain
	g_dxgiSwapChain->Present(g_dxConfig.upscaleMode != UPSCALE_OFF ? 0 : 1, 0);

	return hr;
}

// ---------------------------------------------------------------------------
// Hooked DX8 Reset
//
// When the DX8 device is reset (resolution change, alt-tab, etc.),
// we need to recreate our D3D11 resources to match the new dimensions.
// ---------------------------------------------------------------------------
static HRESULT __stdcall HookedReset(
	void* pDevice,
	void* pPresentationParameters)
{
	// Release our upscale resources before the device reset
	ReleaseUpscaleResources();
	if (g_dxgiSwapChain)
	{
		g_dxgiSwapChain->Release();
		g_dxgiSwapChain = nullptr;
	}

	// Call original Reset
	HRESULT hr = g_originalReset(pDevice, pPresentationParameters);

	// Recreate resources if reset succeeded and upgrade is still enabled
	if (SUCCEEDED(hr) && g_dxConfig.enabled)
	{
		// Re-read window dimensions
		RECT clientRect;
		if (GetClientRect(g_gameHwnd, &clientRect))
		{
			UINT newWidth = clientRect.right - clientRect.left;
			UINT newHeight = clientRect.bottom - clientRect.top;
			if (newWidth > 0 && newHeight > 0)
			{
				CreateSwapChain(g_gameHwnd, newWidth, newHeight);
				CreateUpscaleResources(newWidth, newHeight);
			}
		}
	}

	return hr;
}

// ---------------------------------------------------------------------------
// VTable hooking for DX8 device
// ---------------------------------------------------------------------------
static bool HookDX8Present()
{
	if (g_d3d8DevicePtr == 0)
		return false;

	// Get the DX8 device pointer: d3dDevicePtr points to a location
	// in EQGfx_Dx8.dll that holds the IDirect3DDevice8*
	void* pDevice = *(void**)g_d3d8DevicePtr;
	if (!pDevice)
		return false;

	// Get the vtable pointer (first DWORD of the COM object)
	DWORD* vtable = *(DWORD**)pDevice;
	if (!vtable)
		return false;

	// Save original function pointers
	g_originalPresent = (D3D8Present_t)vtable[D3D8_VTABLE_INDEX_PRESENT];
	g_originalReset = (D3D8Reset_t)vtable[D3D8_VTABLE_INDEX_RESET];

	// Patch the vtable to point to our hooks
	DWORD oldProtect;

	VirtualProtect(&vtable[D3D8_VTABLE_INDEX_PRESENT], sizeof(DWORD), PAGE_EXECUTE_READWRITE, &oldProtect);
	vtable[D3D8_VTABLE_INDEX_PRESENT] = (DWORD)&HookedPresent;
	VirtualProtect(&vtable[D3D8_VTABLE_INDEX_PRESENT], sizeof(DWORD), oldProtect, &oldProtect);

	VirtualProtect(&vtable[D3D8_VTABLE_INDEX_RESET], sizeof(DWORD), PAGE_EXECUTE_READWRITE, &oldProtect);
	vtable[D3D8_VTABLE_INDEX_RESET] = (DWORD)&HookedReset;
	VirtualProtect(&vtable[D3D8_VTABLE_INDEX_RESET], sizeof(DWORD), oldProtect, &oldProtect);

	return true;
}

static void UnhookDX8Present()
{
	if (g_d3d8DevicePtr == 0 || !g_originalPresent)
		return;

	void* pDevice = *(void**)g_d3d8DevicePtr;
	if (!pDevice)
		return;

	DWORD* vtable = *(DWORD**)pDevice;
	if (!vtable)
		return;

	// Restore original vtable entries
	DWORD oldProtect;

	if (g_originalPresent)
	{
		VirtualProtect(&vtable[D3D8_VTABLE_INDEX_PRESENT], sizeof(DWORD), PAGE_EXECUTE_READWRITE, &oldProtect);
		vtable[D3D8_VTABLE_INDEX_PRESENT] = (DWORD)g_originalPresent;
		VirtualProtect(&vtable[D3D8_VTABLE_INDEX_PRESENT], sizeof(DWORD), oldProtect, &oldProtect);
		g_originalPresent = nullptr;
	}

	if (g_originalReset)
	{
		VirtualProtect(&vtable[D3D8_VTABLE_INDEX_RESET], sizeof(DWORD), PAGE_EXECUTE_READWRITE, &oldProtect);
		vtable[D3D8_VTABLE_INDEX_RESET] = (DWORD)g_originalReset;
		VirtualProtect(&vtable[D3D8_VTABLE_INDEX_RESET], sizeof(DWORD), oldProtect, &oldProtect);
		g_originalReset = nullptr;
	}
}

// ---------------------------------------------------------------------------
// Configuration (eqclient.ini)
// ---------------------------------------------------------------------------
DXUpgradeConfig LoadDXUpgradeConfig()
{
	DXUpgradeConfig config;
	char szResult[255];
	char szDefault[255];

	// [DXUpgrade] Enabled=FALSE
	sprintf(szDefault, "%s", "FALSE");
	GetPrivateProfileStringA("DXUpgrade", "Enabled", szDefault, szResult, 255, "./eqclient.ini");
	config.enabled = (!strcmp(szResult, "TRUE") || !strcmp(szResult, "true") || !strcmp(szResult, "1"));

	// [DXUpgrade] UpscaleMode=0  (0=off, 1=quality, 2=balanced, 3=performance, 4=ultra_performance)
	sprintf(szDefault, "%d", 0);
	GetPrivateProfileStringA("DXUpgrade", "UpscaleMode", szDefault, szResult, 255, "./eqclient.ini");
	config.upscaleMode = atoi(szResult);
	if (config.upscaleMode < UPSCALE_OFF || config.upscaleMode > UPSCALE_ULTRA_PERF)
		config.upscaleMode = UPSCALE_OFF;

	// [DXUpgrade] RenderScale=100 (25-100, internal render resolution percentage)
	sprintf(szDefault, "%d", 100);
	GetPrivateProfileStringA("DXUpgrade", "RenderScale", szDefault, szResult, 255, "./eqclient.ini");
	config.renderScale = atoi(szResult);
	if (config.renderScale < 25) config.renderScale = 25;
	if (config.renderScale > 100) config.renderScale = 100;

	// [DXUpgrade] EnableHDR=FALSE
	sprintf(szDefault, "%s", "FALSE");
	GetPrivateProfileStringA("DXUpgrade", "EnableHDR", szDefault, szResult, 255, "./eqclient.ini");
	config.enableHDR = (!strcmp(szResult, "TRUE") || !strcmp(szResult, "true") || !strcmp(szResult, "1"));

	return config;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
bool InitDXUpgrade(DWORD d3dDevicePtr, HWND hwnd)
{
	if (g_dxUpgradeInitialized)
		return true;

	g_dxConfig = LoadDXUpgradeConfig();
	if (!g_dxConfig.enabled)
		return false;

	g_d3d8DevicePtr = d3dDevicePtr;
	g_gameHwnd = hwnd;

	// Check for NVIDIA GPU (required for DLSS)
	g_isNvidiaGPU = CheckNvidiaGPU();

	// If DLSS upscaling is requested but no NVIDIA GPU, disable upscaling
	if (g_dxConfig.upscaleMode != UPSCALE_OFF && !g_isNvidiaGPU)
	{
		g_dxConfig.upscaleMode = UPSCALE_OFF;
	}

	// Create the D3D11 device for the upscaling pipeline
	if (!CreateD3D11Device())
	{
		return false;
	}

	// Get window dimensions for resource creation
	RECT clientRect;
	if (!GetClientRect(hwnd, &clientRect))
	{
		ReleaseD3D11Device();
		return false;
	}

	UINT width = clientRect.right - clientRect.left;
	UINT height = clientRect.bottom - clientRect.top;
	if (width == 0 || height == 0)
	{
		ReleaseD3D11Device();
		return false;
	}

	// Create swap chain for DXGI presentation
	if (!CreateSwapChain(hwnd, width, height))
	{
		ReleaseD3D11Device();
		return false;
	}

	// Create upscale resources (staging texture, output render target)
	if (!CreateUpscaleResources(width, height))
	{
		ReleaseD3D11Device();
		return false;
	}

	// Hook the DX8 device's Present and Reset methods
	if (!HookDX8Present())
	{
		ReleaseD3D11Device();
		return false;
	}

	g_dxUpgradeInitialized = true;
	g_dxUpgradeActive = true;

	return true;
}

void ShutdownDXUpgrade()
{
	if (!g_dxUpgradeInitialized)
		return;

	g_dxUpgradeActive = false;

	// Restore original DX8 vtable
	UnhookDX8Present();

	// Release all D3D11/DXGI resources
	ReleaseD3D11Device();

	g_dxUpgradeInitialized = false;
}

bool IsDXUpgradeActive()
{
	return g_dxUpgradeActive;
}

const DXUpgradeConfig& GetDXUpgradeConfig()
{
	return g_dxConfig;
}
