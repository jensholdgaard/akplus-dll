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
#include "nis_sharpen.h"

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
static DXUpgradeConfig g_dxConfig = { false, false, 0.5f, false };

// DX8 device pointer location (in EQGfx_Dx8.dll)
static DWORD g_d3d8DevicePtr = 0;

// Original DX8 vtable function pointers
static D3D8Present_t g_originalPresent = nullptr;
static D3D8Reset_t g_originalReset = nullptr;

// Game window
static HWND g_gameHwnd = 0;

// DXGI / D3D11 objects
static IDXGIFactory1* g_dxgiFactory = nullptr;
static IDXGIAdapter1* g_dxgiAdapter = nullptr;
static ID3D11Device* g_d3d11Device = nullptr;
static ID3D11DeviceContext* g_d3d11Context = nullptr;
static IDXGISwapChain* g_dxgiSwapChain = nullptr;

// D3D11 staging texture for receiving DX8 frame data via GDI
static ID3D11Texture2D* g_stagingTexture = nullptr;

// D3D11 output texture for sharpening pass (needs UAV binding)
static ID3D11Texture2D* g_outputTexture = nullptr;

// Frame dimensions
static UINT g_frameWidth = 0;
static UINT g_frameHeight = 0;

// NVIDIA GPU detection flag
static bool g_isNvidiaGPU = false;

// GPU adapter name (for logging/status)
static char g_gpuName[128] = { 0 };

// Cached DX8 backbuffer dimensions (for letterbox aspect ratio correction)
static UINT g_dx8BackbufferWidth = 0;
static UINT g_dx8BackbufferHeight = 0;

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
static bool CreateD3D11Device();
static bool CreateStagingTexture(UINT width, UINT height);
static void ReleaseStagingTexture();
static void ReleaseD3D11Device();
static bool HookDX8Present();
static void UnhookDX8Present();
static bool CaptureFrameToStaging();
static bool CopyStagingToSwapChain();
static bool QueryDX8BackbufferSize(UINT* outWidth, UINT* outHeight);

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

	// Find the best adapter (prefer NVIDIA if present)
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
// Staging Texture (CPU-writable texture for DX8->DX11 frame copy via GDI)
// and Output Texture (GPU-writable for sharpening pass, needs UAV binding)
// ---------------------------------------------------------------------------
static bool CreateStagingTexture(UINT width, UINT height)
{
	if (!g_d3d11Device)
		return false;

	D3D11_TEXTURE2D_DESC stagingDesc;
	ZeroMemory(&stagingDesc, sizeof(stagingDesc));
	stagingDesc.Width = width;
	stagingDesc.Height = height;
	stagingDesc.MipLevels = 1;
	stagingDesc.ArraySize = 1;
	stagingDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; // Matches GDI BGRA output
	stagingDesc.SampleDesc.Count = 1;
	stagingDesc.Usage = D3D11_USAGE_DYNAMIC;
	stagingDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

	HRESULT hr = g_d3d11Device->CreateTexture2D(&stagingDesc, NULL, &g_stagingTexture);
	if (FAILED(hr))
		return false;

	// Output texture for sharpening pass (needs UAV for compute shader output)
	D3D11_TEXTURE2D_DESC outputDesc;
	ZeroMemory(&outputDesc, sizeof(outputDesc));
	outputDesc.Width = width;
	outputDesc.Height = height;
	outputDesc.MipLevels = 1;
	outputDesc.ArraySize = 1;
	outputDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	outputDesc.SampleDesc.Count = 1;
	outputDesc.Usage = D3D11_USAGE_DEFAULT;
	outputDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;

	hr = g_d3d11Device->CreateTexture2D(&outputDesc, NULL, &g_outputTexture);
	if (FAILED(hr))
	{
		// Sharpening output failed, but staging still works — continue without sharpening
		g_outputTexture = nullptr;
	}

	g_frameWidth = width;
	g_frameHeight = height;

	return true;
}

static void ReleaseStagingTexture()
{
	if (g_outputTexture) { g_outputTexture->Release(); g_outputTexture = nullptr; }
	if (g_stagingTexture) { g_stagingTexture->Release(); g_stagingTexture = nullptr; }
}

// ---------------------------------------------------------------------------
// Release D3D11 Device and DXGI objects
// ---------------------------------------------------------------------------
static void ReleaseD3D11Device()
{
	ReleaseStagingTexture();
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
//
// When letterbox mode is enabled and DX8 backbuffer dimensions are known,
// StretchBlt is used to preserve the original aspect ratio with pillarbox
// or letterbox bars (black).
// ---------------------------------------------------------------------------

// Query DX8 backbuffer dimensions from the device via vtable.
// IDirect3DSurface8::GetDesc returns D3DSURFACE_DESC with Width at offset 24
// and Height at offset 28.
static bool QueryDX8BackbufferSize(UINT* outWidth, UINT* outHeight)
{
	if (g_d3d8DevicePtr == 0)
		return false;

	void* pDevice = *(void**)g_d3d8DevicePtr;
	if (!pDevice)
		return false;

	DWORD* devVtable = *(DWORD**)pDevice;
	if (!devVtable)
		return false;

	// IDirect3DDevice8::GetBackBuffer(UINT BackBuffer, DWORD Type, IDirect3DSurface8**)
	// vtable[16], Type 0 = D3DBACKBUFFER_TYPE_MONO
	typedef HRESULT(__stdcall* GetBackBuffer_t)(void*, UINT, DWORD, void**);
	GetBackBuffer_t fnGetBackBuffer = (GetBackBuffer_t)devVtable[D3D8_VTABLE_INDEX_GETBACKBUFFER];

	void* pSurface = nullptr;
	HRESULT hr = fnGetBackBuffer(pDevice, 0, 0, &pSurface);
	if (FAILED(hr) || !pSurface)
		return false;

	DWORD* surfVtable = *(DWORD**)pSurface;

	// D3DSURFACE_DESC8 layout (32 bytes total):
	//   offset  0: Format (4 bytes)
	//   offset  4: Type (4 bytes)
	//   offset  8: Usage (4 bytes)
	//   offset 12: Pool (4 bytes)
	//   offset 16: Size (4 bytes)
	//   offset 20: MultiSampleType (4 bytes)
	//   offset 24: Width (4 bytes)
	//   offset 28: Height (4 bytes)
	BYTE desc[32];
	ZeroMemory(desc, sizeof(desc));

	// IDirect3DSurface8 vtable: [0] QI, [1] AddRef, [2] Release, ... [8] GetDesc
	static const int SURFACE_VTABLE_RELEASE = 2;
	static const int SURFACE_VTABLE_GETDESC = 8;

	typedef HRESULT(__stdcall* SurfGetDesc_t)(void*, void*);
	SurfGetDesc_t fnGetDesc = (SurfGetDesc_t)surfVtable[SURFACE_VTABLE_GETDESC];

	hr = fnGetDesc(pSurface, desc);

	// Release surface
	typedef ULONG(__stdcall* Release_t)(void*);
	Release_t fnRelease = (Release_t)surfVtable[SURFACE_VTABLE_RELEASE];
	fnRelease(pSurface);

	if (FAILED(hr))
		return false;

	*outWidth = *(UINT*)(desc + 24);
	*outHeight = *(UINT*)(desc + 28);
	return (*outWidth > 0 && *outHeight > 0);
}

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

	BOOL bltResult = FALSE;

	// Letterbox/pillarbox: preserve DX8 backbuffer aspect ratio on widescreen
	if (g_dxConfig.letterboxEnabled && g_dx8BackbufferWidth > 0 && g_dx8BackbufferHeight > 0)
	{
		// Clear bitmap to black (for pillarbox/letterbox bars)
		memset(pBits, 0, g_frameWidth * g_frameHeight * 4);

		// Compute destination rect that preserves the DX8 backbuffer aspect ratio
		float srcAspect = (float)g_dx8BackbufferWidth / (float)g_dx8BackbufferHeight;
		float dstAspect = (float)g_frameWidth / (float)g_frameHeight;

		int dstX = 0, dstY = 0;
		int dstW = (int)g_frameWidth, dstH = (int)g_frameHeight;

		if (srcAspect < dstAspect)
		{
			// Window is wider than content: pillarbox (bars on sides)
			dstW = (int)(g_frameHeight * srcAspect);
			dstX = ((int)g_frameWidth - dstW) / 2;
		}
		else if (srcAspect > dstAspect)
		{
			// Window is taller than content: letterbox (bars on top/bottom)
			dstH = (int)(g_frameWidth / srcAspect);
			dstY = ((int)g_frameHeight - dstH) / 2;
		}

		// StretchBlt from full window → centered content rect in DIB.
		// This un-stretches the DX8 content back to correct aspect ratio.
		SetStretchBltMode(hdcMem, HALFTONE);
		SetBrushOrgEx(hdcMem, 0, 0, NULL);
		bltResult = StretchBlt(
			hdcMem, dstX, dstY, dstW, dstH,
			hdcWindow, 0, 0, (int)g_frameWidth, (int)g_frameHeight,
			SRCCOPY);
	}
	else
	{
		// No letterbox: full 1:1 BitBlt
		bltResult = BitBlt(hdcMem, 0, 0, (int)g_frameWidth, (int)g_frameHeight,
			hdcWindow, 0, 0, SRCCOPY);
	}

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

	// Copy the staging texture to the back buffer (both are B8G8R8A8_UNORM)
	g_d3d11Context->CopyResource(pBackBuffer, g_stagingTexture);

	pBackBuffer->Release();
	return true;
}

// ---------------------------------------------------------------------------
// Hooked DX8 Present
//
// This intercepts every frame presented by the DX8 device. After the original
// DX8 Present completes, we capture the rendered frame via GDI, upload it to
// a D3D11 staging texture, optionally apply GPU sharpening, then copy the
// result to the swap chain back buffer and present via DXGI.
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

	// Query DX8 backbuffer size for letterbox (cached, re-queried on Reset)
	if (g_dxConfig.letterboxEnabled && g_dx8BackbufferWidth == 0)
	{
		QueryDX8BackbufferSize(&g_dx8BackbufferWidth, &g_dx8BackbufferHeight);
	}

	// Step 1: Capture the DX8-rendered frame into the D3D11 staging texture
	if (!CaptureFrameToStaging())
		return hr;

	// Step 2: Apply GPU sharpening if enabled and available
	if (g_dxConfig.sharpenEnabled && IsNISSharpenReady() && g_outputTexture)
	{
		if (ApplyNISSharpen(g_d3d11Context, g_stagingTexture, g_outputTexture, g_dxConfig.sharpness))
		{
			// Copy sharpened output to swap chain back buffer
			ID3D11Texture2D* pBackBuffer = nullptr;
			if (SUCCEEDED(g_dxgiSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&pBackBuffer)))
			{
				g_d3d11Context->CopyResource(pBackBuffer, g_outputTexture);
				pBackBuffer->Release();
			}
		}
		else
		{
			// Sharpening failed, fall back to direct copy
			CopyStagingToSwapChain();
		}
	}
	else
	{
		// No sharpening — direct copy
		CopyStagingToSwapChain();
	}

	// Step 3: Present the frame through the DXGI swap chain (no VSync)
	g_dxgiSwapChain->Present(0, 0);

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
	// Release staging/output textures and NIS before the device reset
	ShutdownNISSharpen();
	ReleaseStagingTexture();
	if (g_dxgiSwapChain)
	{
		g_dxgiSwapChain->Release();
		g_dxgiSwapChain = nullptr;
	}

	// Reset cached DX8 backbuffer dimensions (will be re-queried after reset)
	g_dx8BackbufferWidth = 0;
	g_dx8BackbufferHeight = 0;

	// Call original Reset
	HRESULT hr = g_originalReset(pDevice, pPresentationParameters);

	// Recreate resources if reset succeeded and bridge is still enabled
	if (SUCCEEDED(hr) && g_dxConfig.enabled)
	{
		// Read DX8 backbuffer dims from PresentationParameters if available
		// D3DPRESENT_PARAMETERS8: BackBufferWidth at offset 0, BackBufferHeight at offset 4
		if (pPresentationParameters)
		{
			UINT* pParams = (UINT*)pPresentationParameters;
			if (pParams[0] > 0 && pParams[1] > 0)
			{
				g_dx8BackbufferWidth = pParams[0];
				g_dx8BackbufferHeight = pParams[1];
			}
		}

		RECT clientRect;
		if (GetClientRect(g_gameHwnd, &clientRect))
		{
			UINT newWidth = clientRect.right - clientRect.left;
			UINT newHeight = clientRect.bottom - clientRect.top;
			if (newWidth > 0 && newHeight > 0)
			{
				CreateSwapChain(g_gameHwnd, newWidth, newHeight);
				CreateStagingTexture(newWidth, newHeight);
				if (g_dxConfig.sharpenEnabled)
					InitNISSharpen(g_d3d11Device, newWidth, newHeight);
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

	// [DXUpgrade] Sharpen=TRUE (enable GPU sharpening)
	sprintf(szDefault, "%s", "TRUE");
	GetPrivateProfileStringA("DXUpgrade", "Sharpen", szDefault, szResult, 255, "./eqclient.ini");
	config.sharpenEnabled = (!strcmp(szResult, "TRUE") || !strcmp(szResult, "true") || !strcmp(szResult, "1"));

	// [DXUpgrade] Sharpness=50 (0-100, maps to 0.0-1.0)
	sprintf(szDefault, "%d", 50);
	GetPrivateProfileStringA("DXUpgrade", "Sharpness", szDefault, szResult, 255, "./eqclient.ini");
	int sharpnessInt = atoi(szResult);
	if (sharpnessInt < 0) sharpnessInt = 0;
	if (sharpnessInt > 100) sharpnessInt = 100;
	config.sharpness = sharpnessInt / 100.0f;

	// [DXUpgrade] Letterbox=TRUE (preserve 4:3 aspect with pillarbox bars)
	sprintf(szDefault, "%s", "TRUE");
	GetPrivateProfileStringA("DXUpgrade", "Letterbox", szDefault, szResult, 255, "./eqclient.ini");
	config.letterboxEnabled = (!strcmp(szResult, "TRUE") || !strcmp(szResult, "true") || !strcmp(szResult, "1"));

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

	// Detect GPU vendor (informational only)
	g_isNvidiaGPU = CheckNvidiaGPU();

	// Create the D3D11 device
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

	// Create DXGI swap chain
	if (!CreateSwapChain(hwnd, width, height))
	{
		ReleaseD3D11Device();
		return false;
	}

	// Create staging texture for DX8 → DX11 frame copy
	if (!CreateStagingTexture(width, height))
	{
		ReleaseD3D11Device();
		return false;
	}

	// Initialize NIS GPU sharpening if enabled
	if (g_dxConfig.sharpenEnabled)
	{
		// Non-fatal: if sharpening init fails, we continue without it
		InitNISSharpen(g_d3d11Device, width, height);
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

	// Shutdown NIS sharpening
	ShutdownNISSharpen();

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

const char* GetDXUpgradeGPUName()
{
	return g_gpuName;
}
