# eqgame_dll

## DX11 Rendering Bridge

The DLL includes a DirectX 11 bridge module that creates a parallel D3D11
device alongside EverQuest's native DirectX 8 renderer. It hooks the DX8
Present call and captures each rendered frame into a D3D11 texture, then
presents it through a modern DXGI swap chain.

### How It Works

1. **DX8 Present Hook** — Intercepts every frame via vtable patching of the
   IDirect3DDevice8 interface.
2. **GDI Frame Capture** — After DX8 presents, captures the rendered frame
   from the game window using `BitBlt` into a 32bpp BGRA bitmap.
3. **D3D11 Staging Texture** — Uploads the captured pixels into a
   CPU-writable D3D11 dynamic texture.
4. **DXGI Swap Chain** — Copies the staging texture to the swap chain back
   buffer and presents via DXGI.

### What This Does NOT Do

**This DLL does not enable DLSS 4 or any upscaling technology.** DLSS requires:

- **NVIDIA NGX SDK runtime** (`nvngx_dlss.dll`) — a proprietary NVIDIA binary
  that cannot be redistributed in this DLL.
- **Per-frame motion vectors** — DLSS needs pixel-accurate motion data every
  frame. The DX8 EverQuest client does not expose this.
- **Depth buffer access** — DLSS requires the scene depth buffer. DX8 does
  not provide a mechanism to share this with a DX11 device.
- **Sub-pixel jitter** — DLSS requires the camera to be jittered each frame
  by a sub-pixel offset. This DLL does not modify EQ's rendering pipeline.

The DX11 bridge is a foundation layer. Actual DLSS integration would require
significant additional work (motion vector estimation, depth reconstruction)
and the external NVIDIA NGX runtime.

### Configuration

Add to your `eqclient.ini`:

```ini
[DXUpgrade]
; Enable the DX11 rendering bridge (TRUE/FALSE)
Enabled=TRUE
```

### Requirements

- **Windows 10+** — Required for DirectX 11 and DXGI 1.1+.
- **Any GPU with D3D11 support** — NVIDIA, AMD, or Intel.

### Building

The project requires:
- Visual Studio 2022 (v143 toolset)
- Windows SDK with DirectX 11 headers (`d3d11.h`, `dxgi.h`)
- The D3D11 and DXGI libraries are linked automatically via the project file
