# eqgame_dll

## DX11 Rendering Bridge with GPU Sharpening

The DLL includes a DirectX 11 bridge module that creates a parallel D3D11
device alongside EverQuest's native DirectX 8 renderer. It hooks the DX8
Present call, captures each rendered frame into a D3D11 texture, optionally
applies GPU-accelerated adaptive sharpening, and presents the result through
a modern DXGI swap chain.

### How It Works

1. **DX8 Present Hook** — Intercepts every frame via vtable patching of the
   IDirect3DDevice8 interface.
2. **GDI Frame Capture** — After DX8 presents, captures the rendered frame
   from the game window using `BitBlt` into a 32bpp BGRA bitmap.
3. **D3D11 Staging Texture** — Uploads the captured pixels into a
   CPU-writable D3D11 dynamic texture.
4. **GPU Sharpening (CAS)** — Runs a D3D11 compute shader that applies
   Contrast Adaptive Sharpening to the captured frame. The shader analyzes
   local contrast in a 3×3 neighborhood and applies adaptive sharpening
   that enhances detail without over-sharpening edges.
5. **DXGI Swap Chain** — Copies the sharpened output to the swap chain
   back buffer and presents via DXGI.

### Why Not DLSS 4 (NVIDIA NGX SDK)?

The NVIDIA NGX SDK headers are [publicly available](https://github.com/NVIDIA/DLSS)
but DLSS cannot be integrated into this DLL for two reasons:

1. **32-bit limitation** — The NGX SDK only provides x86_64 (64-bit) link
   libraries. This project builds for Win32 (x86/32-bit) to match the
   EverQuest client.
2. **Missing rendering data** — DLSS Super Resolution requires per-frame
   motion vectors, a depth buffer, and sub-pixel camera jitter. The DX8
   EverQuest client does not expose any of these.

**NVIDIA Image Scaling (NIS)** is used instead — it's NVIDIA's MIT-licensed
spatial upscaler/sharpener that works with only a color image input, requires
no motion vectors or depth, and runs on any GPU with D3D11 support.

### Configuration

Add to your `eqclient.ini`:

```ini
[DXUpgrade]
; Enable the DX11 rendering bridge (TRUE/FALSE)
Enabled=TRUE

; Enable GPU sharpening pass (TRUE/FALSE)
Sharpen=TRUE

; Sharpness intensity (0-100, default 50)
; 0 = no sharpening, 50 = balanced, 100 = maximum
Sharpness=50
```

### Requirements

- **Windows 10+** — Required for DirectX 11, DXGI 1.1+, and d3dcompiler_47.dll.
- **Any GPU with D3D11 support** — NVIDIA, AMD, or Intel.
- The GPU sharpening shader is compiled at runtime via `d3dcompiler_47.dll`
  (ships with Windows 10+). If unavailable, the bridge works without sharpening.

### Building

The project requires:
- Visual Studio 2022 (v143 toolset)
- Windows SDK with DirectX 11 headers (`d3d11.h`, `dxgi.h`, `d3dcompiler.h`)
- The D3D11 and DXGI libraries are linked automatically via the project file
- `d3dcompiler_47.dll` is loaded dynamically at runtime (not a link dependency)
