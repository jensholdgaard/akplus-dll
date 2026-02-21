#pragma once

#include <windows.h>

// DX11 Rendering Bridge
//
// This module creates a parallel DirectX 11 device alongside EverQuest's
// native DirectX 8 renderer. It hooks the DX8 Present call and captures
// each rendered frame into a D3D11 texture via GDI, then presents it
// through a modern DXGI swap chain.
//
// What this provides:
//   - A D3D11 device and DXGI swap chain running on the same GPU
//   - Per-frame capture of the DX8 backbuffer into a D3D11 texture
//   - A foundation that future upscaling integrations could build on
//
// What this does NOT provide:
//   - DLSS 4 or any upscaling. DLSS requires the NVIDIA NGX SDK runtime
//     (nvngx_dlss.dll), per-frame motion vectors, depth buffers, and
//     sub-pixel jitter — none of which are available from the DX8 client.
//   - Any visual improvement over the native DX8 renderer.
//   - GPU-accelerated frame capture (uses GDI BitBlt, which is CPU-bound).
//
// Configuration is done via [DXUpgrade] section in eqclient.ini

// Configuration loaded from eqclient.ini [DXUpgrade] section
struct DXUpgradeConfig {
	bool enabled;            // Master enable/disable for DX11 bridge
};

// Initialize the DX11 bridge.
// Must be called after the DX8 device is available (d3dDevicePtr is set).
// d3dDevicePtr: address in EQGfx_Dx8.dll containing the IDirect3DDevice8*
// hwnd: game window handle
bool InitDXUpgrade(DWORD d3dDevicePtr, HWND hwnd);

// Shutdown the DX11 bridge and restore original DX8 Present.
void ShutdownDXUpgrade();

// Load settings from eqclient.ini [DXUpgrade] section.
DXUpgradeConfig LoadDXUpgradeConfig();

// Check if an NVIDIA GPU is present.
bool CheckNvidiaGPU();

// Returns true if the DX11 bridge is currently active.
bool IsDXUpgradeActive();

// Returns the current configuration.
const DXUpgradeConfig& GetDXUpgradeConfig();

// Get the name of the GPU adapter being used for D3D11 rendering.
// Returns empty string if not initialized.
const char* GetDXUpgradeGPUName();
