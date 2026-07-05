# LearnDirectX

一个基于 DirectX 12 的实时渲染学习项目。当前工程已经不只是早期的 D3D12 入门示例，而是一个围绕 PBR、IBL、Deferred Rendering、级联阴影、HBAO、TAA 和 Render Dependency Graph 组织起来的模型查看器/小型渲染器。

默认场景从 `Settings/Scene.json` 读取，默认窗口标题为 `PBR IBL Model Viewer`。

## 功能概览

- DirectX 12 渲染后端：设备、SwapChain、Command Queue/List、Descriptor Heap、Fence 和三缓冲同步。
- PBR 材质：Albedo、Normal、ORM、Emissive、Unlit 材质路径和 bindless 纹理索引。
- IBL：HDR 环境贴图、Cubemap 转换、Specular Prefilter、BRDF LUT 和 SH Irradiance 计算。
- 模型加载：以 `.glb` 为当前入口格式，使用 Assimp 解析网格/贴图，使用 tinygltf 读取 glTF 元数据。
- 场景实例化：相同模型数据复用，按实例维护 transform、透明/裁剪标记、可见性和 LOD。
- 自动 LOD：通过 `meshoptimizer` 为每个 mesh 生成多级索引。
- 可见性裁剪：主相机视锥裁剪，阴影级联裁剪。
- 阴影：4 级 Cascaded Shadow Maps，带 texel snapping 以减少阴影抖动。
- Deferred 路径：GBuffer、HBAO、Deferred Lighting、Skybox、Transparent、TAA、PostProcess。
- Forward/PBR 路径：可关闭 Deferred，用 PBR pass 直接渲染不透明物体。
- RDG：`RDG/RDG.h` 提供 pass 依赖、资源状态转换、外部资源注册和瞬时资源生命周期管理。
- 运行时 shader 编译：通过 DXC 编译 Shader Model 6.6 的 HLSL 文件。

## 目录结构

| 路径 | 说明 |
| --- | --- |
| `PBR_Render.cpp` | WinMain、应用初始化、主循环、每帧更新和渲染调度。 |
| `Core/` | D3D12App、Win32 窗口和公共 D3D12 预编译头。 |
| `Graphics/` | `RenderDevice` 和 `PipelineManager`，负责 D3D12 设备、PSO、Root Signature、shader 编译。 |
| `Resources/` | 模型、贴图、IBL、GBuffer、HBAO、TAA 历史缓冲等资源管理。 |
| `Passes/` | Shadow、PBR、GBuffer、DeferredLighting、HBAO、Skybox、TAA、PostProcess 等渲染 pass。 |
| `RDG/` | Render Dependency Graph 实现。 |
| `Logic/` | Camera、InputManager、SceneObject。 |
| `Shaders/` | 运行时编译的 HLSL shader。 |
| `Settings/` | 窗口、渲染管线、光照和场景 JSON 配置。 |
| `Models/` | 默认和测试用 `.glb` 模型资源。 |
| `HDRs/` | IBL/天空盒用 HDR 环境图。 |
| `ThirdParty/` | vendored header：`tiny_gltf`、`stb_image`、`json.hpp`、`meshoptimizer.h`。 |
| `packages/` | NuGet 还原后的 DirectX Agility SDK 和 DirectXTK12 包。 |

## 环境要求

- Windows 10/11。
- 支持 DirectX 12 的 GPU 和驱动。
- Visual Studio，安装 C++ 桌面开发工作负载。
- 当前 `.vcxproj` 使用：
  - C++20。
  - `PlatformToolset` 为 `v145`。
  - `WindowsTargetPlatformVersion` 为 `10.0`。
- Windows SDK。
- NuGet 包：
  - `Microsoft.Direct3D.D3D12` `1.619.1`。
  - `directxtk12_desktop_win10` `2026.4.1.1`。
- Assimp x64 开发库。推荐用 vcpkg：

```powershell
git clone https://github.com/microsoft/vcpkg.git
cd vcpkg
.\bootstrap-vcpkg.bat
.\vcpkg integrate install
.\vcpkg install assimp:x64-windows
```

如果不用 vcpkg，需要自己保证 Assimp 的 include、lib 和运行时 DLL 能被 Visual Studio/MSBuild 找到。

## 构建和运行

1. 用 Visual Studio 打开 `LearnDirectX.slnx`。如果你的 VS 不识别 `.slnx`，可以直接打开 `LearnDirectX.vcxproj`。
2. 还原 NuGet 包。仓库里已经有 `packages.config`，缺包时 Visual Studio 会提示还原。
3. 选择 `x64` 平台，配置选择 `Debug` 或 `Release`。
4. 确认 Assimp 已安装并能被当前平台找到。
5. 构建并运行。

也可以直接用 MSBuild 构建项目文件：

```powershell
MSBuild.exe LearnDirectX.vcxproj /p:Configuration=Debug /p:Platform=x64 /m
```

运行时需要从仓库根目录启动，或者在 Visual Studio 的调试属性里把 Working Directory 设为项目根目录。程序会用相对路径读取 `Settings/`、`Shaders/`、`Models/` 和 `HDRs/`。

## Shader 注意事项

本工程的 HLSL 文件在运行时由 DXC 编译，不依赖 Visual Studio 的 HLSL build step。工程文件已经把现有 shader 设置为 `Document`。

新增 `.hlsl` 文件时，如果 Visual Studio 自动把它加入构建，请在文件属性中把 Item Type 改为 `Does not participate in build` 或 `Document`，否则 VS 可能会尝试用错误的方式提前编译 shader。

## 配置文件

所有配置都在程序启动时读取。修改 JSON 后重新运行程序即可生效。

### `Settings/Window.json`

```json
{
  "width": 2240,
  "height": 1400,
  "fullscreen": false,
  "title": "PBR IBL Model Viewer"
}
```

### `Settings/Pipeline.json`

```json
{
  "use_deferred": true,
  "use_z_prepass": true,
  "use_taa": false
}
```

- `use_deferred`: 使用 Deferred 路径。关闭后走 Forward/PBR 路径。
- `use_z_prepass`: 启用 Z Prepass。
- `use_taa`: 启用 TAA。默认关闭。

### `Settings/Lighting.json`

```json
{
  "light_dir": [ -0.5, -1.0, 0.5 ],
  "light_color": [ 5.0, 5.0, 5.0 ],
  "shadow_radius": 40.0
}
```

### `Settings/Scene.json`

```json
{
  "skybox_path": "HDRs/citrus_orchard_road_puresky_4k.hdr",
  "stress_test": false,
  "instances": [
    {
      "name": "Character",
      "model_path": "Models/Blender_Chan.glb",
      "pos": [ 0.0, 0.01, 0.0 ],
      "rot": [ 0.0, 0.0, 0.0 ],
      "scale": [ 1.0, 1.0, 1.0 ],
      "is_transparent": true,
      "is_cutout": false
    },
    {
      "name": "Ground",
      "model_path": "Models/Plain.glb",
      "pos": [ 0.0, 0.0, 0.0 ],
      "rot": [ 0.0, 0.0, 0.0 ],
      "scale": [ 1.0, 1.0, 1.0 ],
      "is_transparent": false,
      "is_cutout": false
    }
  ]
}
```

- `skybox_path`: HDR 环境贴图路径。
- `stress_test`: 为 `true` 时，会基于第一个实例的模型生成 20 x 20 x 20 的性能测试场景。
- `model_path`: 当前 `Model` 构造器只接受 `.glb`。
- `pos`、`rot`、`scale`: 实例 transform。`rot` 会按 `XMMatrixRotationRollPitchYaw` 的顺序传入，单位按弧度处理。
- `is_transparent`: 透明对象会被放到透明队列，按相机距离从远到近排序。
- `is_cutout`: 使用 alpha test/cutout 相关 PSO。

## 每帧渲染流程

程序每帧大致执行：

1. 等待当前 back buffer 对应的 GPU fence。
2. 更新相机输入、View/Projection、TAA jitter、上一帧矩阵。
3. 计算 directional light 和 4 级 CSM 矩阵。
4. 进行主相机视锥裁剪、阴影级联裁剪和 LOD 选择。
5. 按透明/不透明、模型指针、LOD、cutout 状态排序，尽量保持实例化批次。
6. 写入 pass constants 和 instance buffer。
7. 根据 `use_deferred` 选择渲染路径。

Deferred 路径：

```text
Shadow
ZPrepass optional
GBuffer
HBAO + Blur
DeferredLighting
Skybox
Transparent
TAA optional
PostProcess
Present
```

Forward/PBR 路径：

```text
Shadow
PBR Opaque
Skybox
Transparent
TAA optional
PostProcess
Present
```

## 操作方式

| 输入 | 功能 |
| --- | --- |
| `W/A/S/D` | 前后左右移动相机。 |
| 按住鼠标右键拖动 | 旋转视角。 |
| 鼠标滚轮 | 沿相机前向移动。 |
| `Esc` | 弹出退出确认。 |

## 添加模型或 HDR

1. 把 `.glb` 模型放到 `Models/`。
2. 把 `.hdr` 环境图放到 `HDRs/`。
3. 修改 `Settings/Scene.json` 中的 `instances` 和 `skybox_path`。
4. 重新运行程序。

当前模型加载路径更偏向 `.glb` 内嵌资源。如果使用外部贴图、非 glb 格式、复杂动画/骨骼或特殊材质扩展，可能需要补充加载逻辑。

## 已知限制

- 当前工程仍依赖 Assimp 解析网格和内嵌贴图，同时用 tinygltf 读取部分 glTF 元数据。
- `Vertex` 中保留了骨骼字段，但当前渲染路径没有实现动画/蒙皮更新。
- 透明物体为了正确混合会按距离排序，这会牺牲一部分 batching。
- TAA 默认关闭，开启后会使用 history buffer 和 jitter。
- 配置和 shader 没有热重载，修改后需要重新运行程序。
- 资源路径大量使用相对路径，工作目录不正确时会出现找不到 shader、模型或 JSON 的问题。
