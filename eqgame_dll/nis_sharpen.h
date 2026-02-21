#pragma once
#include <windows.h>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;

// Initialize the NIS sharpening pipeline.
// device: D3D11 device (must support feature level 11_0+)
// width/height: frame dimensions
// Returns true if initialization succeeded (shader compiled, resources created)
bool InitNISSharpen(ID3D11Device* device, UINT width, UINT height);

// Apply NIS sharpening pass.
// context: D3D11 immediate context
// inputTexture: source texture (B8G8R8A8_UNORM, must have SRV binding)
// outputTexture: destination texture (B8G8R8A8_UNORM, must have UAV binding)
// sharpness: 0.0 to 1.0 (0=no sharpening, 0.5=default, 1.0=maximum)
//
// NOTE: The output texture must be created with D3D11_BIND_UNORDERED_ACCESS flag.
// The caller (dx_upgrade.cpp) is responsible for creating this texture.
bool ApplyNISSharpen(ID3D11DeviceContext* context, ID3D11Texture2D* inputTexture, ID3D11Texture2D* outputTexture, float sharpness);

// Release all NIS sharpening resources.
void ShutdownNISSharpen();

// Returns true if NIS sharpening is initialized and ready.
bool IsNISSharpenReady();
