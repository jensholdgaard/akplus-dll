#pragma once

#include <windows.h>

// DX11 Rendering Bridge with NVIDIA Image Scaling (NIS) Sharpening
//
// This module creates a parallel DirectX 11 device alongside EverQuest's
// native DirectX 8 renderer. It hooks the DX8 Present call, captures each
// rendered frame into a D3D11 texture via GDI, optionally applies GPU-based
// adaptive sharpening (CAS algorithm, inspired by NVIDIA Image Scaling SDK),
// and presents the result through a modern DXGI swap chain.
//
// What this provides:
//   - A D3D11 device and DXGI swap chain running on the same GPU
//   - Per-frame capture of the DX8 backbuffer into a D3D11 texture
//   - GPU-accelerated adaptive sharpening via D3D11 compute shader
//   - Works on any GPU (NVIDIA, AMD, Intel) with D3D11 support
//
// Why not DLSS 4 (NVIDIA NGX SDK)?
//   - The NGX SDK only provides x86_64 libraries; this project is Win32 (x86)
//   - DLSS requires per-frame motion vectors, depth buffers, and sub-pixel
//     jitter — none available from the DX8 client
//   - NIS (spatial sharpening) is a practical alternative that works here
//
// Configuration is done via [DXUpgrade] section in eqclient.ini

// Configuration loaded from eqclient.ini [DXUpgrade] section
struct DXUpgradeConfig {
	bool  enabled;           // Master enable/disable for DX11 bridge
	bool  sharpenEnabled;    // Enable GPU sharpening pass
	float sharpness;         // Sharpness intensity 0.0-1.0 (default 0.5)
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
