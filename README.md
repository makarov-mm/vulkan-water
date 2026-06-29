# vulkan-water

A C++/Vulkan port of Evan Wallace's classic **WebGL Water** demo.

Heightfield water simulated on the GPU with raytraced reflection/refraction,
real-time caustics, soft sphere shadows, switchable box/cylinder pools, and
optional glTF object loading.

## Zero dependencies

The only external requirement is the **Vulkan SDK** (the loader + the
`glslangValidator` shader compiler). Everything else is hand-written:

| Concern            | Usual library | Here                                  |
|--------------------|---------------|---------------------------------------|
| Window + input     | GLFW          | native **Win32** (`CreateWindowExW`)  |
| Vulkan surface     | GLFW          | `vkCreateWin32SurfaceKHR` (`VK_KHR_win32_surface`) |
| Vector / matrix    | GLM           | `src/math3d.h` (own, column-major, Vulkan ZO depth) |
| glTF / GLB parsing | cgltf         | `src/gltf_min.h` (own JSON + accessor reader) |
| Vulkan bootstrap   | vk-bootstrap  | hand-rolled instance/device/swapchain |
| Memory allocation  | VMA           | hand-rolled allocator (`findMemoryType`) |

No `third_party/`, no package manager, no vendored sources.

> **Windows-only.** The windowing layer talks to the Win32 API directly, so the
> project targets Windows + MSVC. Porting to another platform means swapping the
> ~120-line window/surface layer (and the corresponding `VK_KHR_*_surface`
> extension); the rest is platform-independent.

## Build — Visual Studio

1. Install the LunarG **Vulkan SDK** (sets the `VULKAN_SDK` environment variable).
2. Open `VulkanWater.sln`.
3. Build & run (`F5`). Output goes to `build\x64\<Config>\water.exe`.

Shaders are compiled to SPIR-V automatically before each build (a pre-build
target invokes `glslangValidator`); the `.spv` files land next to the
executable in `build\x64\<Config>\shaders\`.

## Build — CMake (Windows + MSVC)

```bat
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
build\Release\water.exe
```

## Run

```bat
water.exe                 :: just the water + sphere
water.exe model.gltf      :: also load a glTF/GLB object into the pool
```

`.gltf` (with an external or base64 `.bin`) and binary `.glb` are both
supported; POSITION, optional NORMAL and indices of the first mesh primitive
are read. If normals are absent they are generated.

## Controls

| Input                | Action                          |
|----------------------|---------------------------------|
| Left-drag background | orbit camera                    |
| Left-click/drag water| make ripples                    |
| Mouse wheel          | zoom                            |
| `Space`              | pause/resume the simulation     |
| `G`                  | toggle gravity (sphere sinks)   |
| `L`                  | set light direction to the view |
| `P`                  | cycle pool shape (box/cylinder) |
| `Esc`                | quit                            |

## Project layout

```
src/main.cpp     Vulkan host: Win32 window, sim/caustic/main passes, physics
src/math3d.h     own vec2/vec3/vec4/mat4 (replaces GLM)
src/gltf_min.h   own glTF/GLB loader (replaces cgltf)
shaders/         GLSL (compiled to SPIR-V at build time)
```

## Credit

Algorithm and look ported from Evan Wallace's WebGL Water
(<https://madebyevan.com/webgl-water/>). This is an independent Vulkan
reimplementation.
