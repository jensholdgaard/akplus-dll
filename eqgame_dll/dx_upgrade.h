#pragma once

#include <windows.h>

// DX Upgrade and DLSS Support
//
// This module provides infrastructure for upgrading the EverQuest client's
// DirectX 8 rendering pipeline to support newer DirectX features, enabling
// NVIDIA DLSS 4 and other modern upscaling technologies.
//
// How it works:
//   1. Hooks the DX8 device's Present call via vtable patching
//   2. Creates a D3D11 device and DXGI swap chain for the same GPU
//   3. After each DX8 frame, copies the backbuffer to a D3D11 texture
//   4. Applies upscaling (DLSS/FSR) on the D3D11 texture
//   5. Presents the upscaled frame via the DXGI swap chain
//
// Configuration is done via [DXUpgrade] section in eqclient.ini

// Upscaling quality modes (maps to DLSS quality presets)
enum UpscaleMode {
	UPSCALE_OFF              = 0,
	UPSCALE_QUALITY          = 1,  // DLSS Quality
	UPSCALE_BALANCED         = 2,  // DLSS Balanced
	UPSCALE_PERFORMANCE      = 3,  // DLSS Performance
	UPSCALE_ULTRA_PERF       = 4   // DLSS Ultra Performance
};

// Configuration loaded from eqclient.ini [DXUpgrade] section
struct DXUpgradeConfig {
	bool enabled;            // Master enable/disable for DX upgrade
	int  upscaleMode;        // UpscaleMode enum value
	int  renderScale;        // Internal render scale percentage (25-100)
	bool enableHDR;          // HDR output (requires monitor support)
};

// Initialize the DX upgrade system.
// Must be called after the DX8 device is available (d3dDevicePtr is set).
// d3dDevicePtr: address in EQGfx_Dx8.dll containing the IDirect3DDevice8*
// hwnd: game window handle
bool InitDXUpgrade(DWORD d3dDevicePtr, HWND hwnd);

// Shutdown the DX upgrade system and restore original DX8 Present.
void ShutdownDXUpgrade();

// Load DX upgrade settings from eqclient.ini [DXUpgrade] section.
DXUpgradeConfig LoadDXUpgradeConfig();

// Check if an NVIDIA GPU is present (required for DLSS).
bool CheckNvidiaGPU();

// Returns true if the DX upgrade system is currently active.
bool IsDXUpgradeActive();

// Returns the current DX upgrade configuration.
const DXUpgradeConfig& GetDXUpgradeConfig();
