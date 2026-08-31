# LearnDirectX

一个面向实时渲染学习与实验的 Windows / DirectX 12 小型渲染器。

它的重点不是提供完整的游戏引擎编辑器，而是展示一条可以实际运行、调试和继续演进的现代渲染链路：从显式 D3D12 资源与同步管理，逐步组织出 PBR、IBL、Forward/Deferred、级联软阴影、时域重建、DLSS、SMAA 和 Render Dependency Graph。

## 项目定位与意义

这个项目适合用于理解以下问题：

- D3D12 的 Device、SwapChain、Command Queue/List、Descriptor Heap、Fence 和三缓冲如何协同工作。
- 模型、材质、纹理、IBL、历史缓冲和瞬时 Render Target 如何进入 GPU 生命周期。
- 如何将渲染步骤从手写调用序列演进为具有依赖关系和资源状态管理的 RDG pass。
- Forward 与 Deferred 两条路径如何共享场景、阴影、透明物体、后处理和抗锯齿设施。
- TAA、TSR、DLSS、SMAA、Motion Vector 和时域 HBAO 在真实帧流程中的位置和差异。
- 如何在一个仍然足够小的学习工程中实践视锥裁剪、LOD、实例批处理、PBR/IBL 和 PCSS。

它目前更接近“模型查看器 + 渲染技术试验场”，而不是通用游戏引擎：没有编辑器、ECS、正式资产管线、多平台后端或动画系统。

## 基础架构

```text
Settings/*.json + 本地 Models/HDRs
                  |
                  v
SettingsManager / ResourceManager / PBR_Model / IBLBaker
                  |
                  v
场景实例 -> 视锥裁剪 -> LOD -> 排序与实例批处理
                  |
                  v
       RDGBuilder（依赖、资源状态、瞬时资源）
          |                         |
          v                         v
   Forward/PBR passes         Deferred passes
          |                         |
          +---- AA / Upscale / PostProcess ----+
                                                |
                                                v
                                      D3D12 SwapChain / Present
```

| 层次 | 主要位置 | 职责 |
| --- | --- | --- |
| 程序与帧调度 | `PBR_Render.cpp` | 初始化、主循环、场景更新、裁剪/LOD、Forward/Deferred RDG 构建与提交。 |
| 平台与后端 | `Core/`、`Graphics/RenderDevice.h` | Win32 窗口、D3D12 设备、交换链、命令对象、描述符堆和同步。 |
| Pipeline | `Graphics/PipelineManager.h` | Root Signature、PSO 和运行时 Shader Model 6.6 编译。 |
| 场景逻辑 | `Logic/` | Camera、输入和场景实例。 |
| 资源系统 | `Resources/` | GLB 模型、材质/贴图、IBL 烘焙、持久/瞬时资源及历史缓冲。 |
| 渲染图 | `RDG/` | pass 依赖、读写声明、资源状态转换、外部资源注册和瞬时资源租约。 |
| 渲染阶段 | `Passes/` | Shadow、PBR、GBuffer、Motion Vector、HBAO、Temporal、DLSS、SMAA 和 PostProcess。 |
| Shader | `Shaders/` | 运行时由 DXC 编译的 HLSL 与共享函数。 |
| 配置 | `Settings/` | 窗口、渲染路径、光照和本地场景描述。 |

## 环境需求（迁移到新机器时必读）

> 仓库不包含 NuGet 展开目录、编译产物、模型或 HDR。仅 clone 仓库不能直接运行，必须完成下表中的依赖还原与本地资源配置。

| 类别 | 要求 | 必需性 | 来源或配置位置 | 缺失时的典型现象 |
| --- | --- | --- | --- | --- |
| 操作系统 | Windows 10/11 64 位 | 必需 | 本机 | Win32/D3D12 程序无法构建或运行。 |
| Visual Studio / MSVC | 安装 C++ 桌面开发工作负载，并提供 `v145` Platform Toolset | 必需 | `LearnDirectX.vcxproj` | 报告找不到 `v145`，需要安装该工具集或显式 retarget。 |
| C++ 标准 | C++20 | 必需 | `.vcxproj` 已配置 | 较旧工具链可能无法编译。 |
| Windows SDK | `WindowsTargetPlatformVersion=10.0` | 必需 | Visual Studio Installer / Windows SDK | 找不到 Windows、DXGI 或 D3D12 头文件/库。 |
| 构建平台 | **x64 Debug 或 x64 Release** | 必需 | Visual Studio / MSBuild | 工程仍残留 Win32 配置，但当前 NVIDIA SDK 和依赖链只按 x64 维护。 |
| GPU 与驱动 | D3D12、Shader Model 6.6、直接索引 CBV/SRV/UAV descriptor heap | 必需 | GPU/驱动 | 设备虽可能达到 Feature Level 11_0，Root Signature 或 SM 6.6 shader 仍可能失败。 |
| NuGet | `Microsoft.Direct3D.D3D12` `1.619.1` | 必需 | `packages.config` | 缺少 Agility SDK、DXC props/targets 或运行时 DLL。 |
| NuGet | `directxtk12_desktop_win10` `2026.4.1.1` | 必需 | `packages.config` | 找不到 `ResourceUploadBatch.h`、`WICTextureLoader.h` 或 DirectXTK12 库。 |
| vcpkg | `assimp:x64-windows` | 必需 | 本机 vcpkg classic/user-wide integration | 找不到 Assimp 头文件、库或运行时 DLL。 |
| vcpkg | `meshoptimizer:x64-windows` | 必需 | 本机 vcpkg classic/user-wide integration | 链接时找不到 `meshoptimizer.lib`。仓库只 vendored 了头文件。 |
| Vendored 依赖 | `ThirdParty/` 完整存在 | 必需 | 随仓库提交 | 缺少 tinygltf、stb、JSON、SMAA LUT 或 NVIDIA NGX/DLSS 头文件和库。 |
| DLSS 硬件 | 兼容的 NVIDIA RTX GPU 与驱动 | 仅选择 `DLSS` 时必需 | `Settings/Pipeline.json` | NGX capability/configuration 失败；程序不会自动降级到其他 AA。 |
| 模型资源 | 至少提供配置中引用的 `.glb` | 运行当前场景必需 | 本地 `Models/` | 模型为空或场景不能正确显示。模型不随 Git 仓库分发。 |
| 环境贴图 | 提供配置中引用的 `.hdr` | 运行当前 IBL 必需 | 本地 `HDRs/` | IBL 初始化失败。HDR 不随 Git 仓库分发。 |
| 工作目录 | 仓库根目录 | 运行时必需 | VS Debugging Working Directory 或启动命令 | 找不到 `Settings/`、`Shaders/`、`Models/`、`HDRs/`。 |

### 依赖的事实来源

当 README 与机器状态不一致时，按以下文件判断：

| 信息 | 权威来源 |
| --- | --- |
| NuGet 包名与版本 | `packages.config` |
| 编译器、平台、链接库和拷贝规则 | `LearnDirectX.vcxproj` |
| 本地运行配置键 | `Settings/*.json` 与 `Resources/Settings_Manager.h` |
| 帧流程和当前支持的渲染模式 | `PBR_Render.cpp` |
| 哪些目录不应提交 | `.gitignore` |

## 新机器搭建步骤

### 1. 安装 C++ 工具链

通过 Visual Studio Installer 安装：

- C++ 桌面开发工作负载。
- MSVC `v145` Platform Toolset。
- Windows 10/11 SDK。

如果当前 Visual Studio 不识别 `LearnDirectX.slnx`，可以直接打开 `LearnDirectX.vcxproj`。不要因为工程里存在 Win32 配置就选择 Win32；当前受支持的平台是 x64。

### 2. 安装 vcpkg 依赖

本项目当前没有 `vcpkg.json` manifest，依赖本机的 classic/user-wide vcpkg integration：

```powershell
git clone https://github.com/microsoft/vcpkg.git
cd vcpkg
.\bootstrap-vcpkg.bat
.\vcpkg install assimp:x64-windows meshoptimizer:x64-windows
.\vcpkg integrate install
```

如果已经有 vcpkg，只需要安装缺失包并确认 integration 指向当前 Visual Studio/MSBuild 用户环境。

### 3. 还原 NuGet 包

`packages/` 被 Git 忽略，必须在每个新 clone 中重新生成。推荐在 Visual Studio 中执行 Restore NuGet Packages。

如果使用 NuGet CLI，可以在仓库根目录执行：

```powershell
nuget.exe restore packages.config -PackagesDirectory packages
```

还原后应存在：

```text
packages/Microsoft.Direct3D.D3D12.1.619.1/
packages/directxtk12_desktop_win10.2026.4.1.1/
```

### 4. 准备本地模型和 HDR

`Models/`、`HDRs/`、`Scene_Assets/` 被有意排除在 Git 之外。创建目录并放入自己的资源：

```powershell
New-Item -ItemType Directory -Force Models, HDRs
```

当前 `Settings/Scene.json` 引用了：

```text
Models/V-22.glb
Models/Plain.glb
HDRs/citrus_orchard_road_puresky_4k.hdr
```

这些文件不随仓库提供。可以放置同名资源，也可以把 `Settings/Scene.json` 改为自己的相对路径。

### 5. 构建

Visual Studio 中选择 `x64`，然后选择 `Debug` 或 `Release`。

也可以从已配置好 MSBuild 环境的终端执行：

```powershell
MSBuild.exe LearnDirectX.vcxproj /p:Configuration=Debug /p:Platform=x64 /m
```

Release 构建：

```powershell
MSBuild.exe LearnDirectX.vcxproj /p:Configuration=Release /p:Platform=x64 /m
```

构建过程会将对应的 `nvngx_dlss.dll` 复制到输出目录。NuGet/vcpkg 还会提供 DXC、Assimp 及其运行时依赖。不要只复制 `LearnDirectX.exe` 后期待它独立运行。

### 6. 从仓库根目录运行

```powershell
.\x64\Debug\LearnDirectX.exe
```

或：

```powershell
.\x64\Release\LearnDirectX.exe
```

Visual Studio 调试时，应把 Working Directory 设置为 `$(ProjectDir)`。程序的配置、shader 和美术资源路径都是相对于仓库根目录解析的。

## 常见环境问题

| 现象 | 优先检查 |
| --- | --- |
| 缺少 `packages/...props` 或 `...targets` | 执行 NuGet restore，确认还原到了仓库根目录的 `packages/`。 |
| 找不到 `ResourceUploadBatch.h` / `WICTextureLoader.h` | DirectXTK12 NuGet 包没有还原。 |
| 找不到 `dxcapi.h`、`dxcompiler.lib` 或运行时 `dxcompiler.dll` | Microsoft.Direct3D.D3D12 NuGet 包没有正确还原/部署。 |
| 找不到 Assimp 头文件或 DLL | 安装 `assimp:x64-windows` 并执行 `vcpkg integrate install`。 |
| `LNK1104: cannot open file 'meshoptimizer.lib'` | 安装 `meshoptimizer:x64-windows`，确认正在构建 x64。 |
| 找不到 `nvsdk_ngx*.h/.lib` | `ThirdParty/NVIDIA/DLSS/` 不完整，或错误地选择了 Win32。 |
| Shader 编译时报文件不存在 | 从仓库根目录运行，检查 `Shaders/`。 |
| 模型或 IBL 初始化异常 | 检查 `Settings/Scene.json` 中的相对路径及本地 `Models/`、`HDRs/`。 |
| DLSS 初始化直接失败 | 检查 NVIDIA GPU/驱动、所选 `dlss_quality` 和输出分辨率；DLSS 不自动 fallback。 |
| `.slnx` 无法打开 | 直接打开 `LearnDirectX.vcxproj`，或升级 Visual Studio。 |

## Git 与资源策略

仓库只提交源码、工程配置和无法通过包管理器恢复的第三方依赖：

- 跟踪：`*.cpp`、`*.h`、HLSL、JSON 配置、工程文件、`packages.config`、`ThirdParty/`。
- 不跟踪：`.vs/`、`x64/`、中间文件、PDB、NuGet 展开目录 `packages/`。
- 不跟踪：`Models/`、`HDRs/`、`Scene_Assets/` 等本地美术资源。

因此，新 clone 中缺少模型和 HDR 是预期行为，不代表 clone 不完整。若未来需要分发示例场景，应单独提供资源下载地址、release 包或独立资产仓库，而不是重新提交大型二进制到主 Git 历史。

## 功能概览

- D3D12 后端与三缓冲同步。
- Shader Model 6.6、DXC 运行时编译和 bindless 纹理索引。
- Metallic/Roughness PBR：Albedo、Normal、ORM、Emissive、Unlit。
- HDR IBL：Equirectangular 到 Cubemap、Specular Prefilter、BRDF LUT、SH Irradiance。
- GLB 模型加载：Assimp 处理网格/材质/内嵌贴图，tinygltf 读取 glTF 元数据。
- 模型数据复用、场景实例以及透明/cutout 标记。
- meshoptimizer 自动生成多级索引 LOD。
- 主相机与各级阴影视锥裁剪、实例排序和批处理。
- 4 级 Cascaded Shadow Maps、texel snapping 与基于太阳角半径的 PCSS 软阴影。
- Forward/PBR 与 Deferred 两条完整 RDG 渲染路径。
- GBuffer、Motion Vector、HBAO、Blur 和时域 HBAO 稳定化。
- 抗锯齿/重建模式：None、TAA、TSR、SMAA、DLSS。
- RDG pass 依赖、资源状态转换、外部资源注册和瞬时资源生命周期。

## 配置文件

所有 JSON 在程序启动时读取，目前不支持热重载。未知或缺失字段通常使用代码默认值；路径仍按仓库根目录解析。

### `Settings/Window.json`

| 键 | 类型 | 当前值 | 说明 |
| --- | --- | --- | --- |
| `width` | integer | `2560` | 输出窗口宽度。 |
| `height` | integer | `1600` | 输出窗口高度。 |
| `fullscreen` | boolean | `false` | 是否全屏。 |
| `tsr_upscale_factor` | number | `2.0` | TSR 放大倍率；选择 TSR 时必须在 `[1.0, 4.0]`。 |
| `title` | string | `Self Made Render Engine` | 窗口标题。 |

### `Settings/Pipeline.json`

| 键 | 类型/可选值 | 当前值 | 说明 |
| --- | --- | --- | --- |
| `use_deferred` | boolean | `true` | `true` 使用 Deferred，`false` 使用 Forward/PBR。 |
| `use_z_prepass` | boolean | `false` | 请求 Z Prepass；TSR 模式下当前会禁用。 |
| `anti_aliasing` | `None` / `TAA` / `TSR` / `SMAA` / `DLSS` | `SMAA` | 互斥的抗锯齿或重建模式。 |
| `dlss_quality` | `DLAA` / `Quality` / `Balanced` / `Performance` / `UltraPerformance` | `Quality` | 仅 DLSS 模式使用；不支持时初始化失败。 |

TAA 和 TSR 共用 jitter 逻辑及一套双缓冲 temporal history，但拥有各自的 pass、shader、PSO、Root Signature 和常量。DLSS 使用独立 NGX 状态、jitter、历史有效性和输出资源。SMAA 在 tone mapping 后执行边缘检测、权重计算和 neighborhood blending。

### `Settings/Lighting.json`

| 键 | 类型 | 当前值 | 说明 |
| --- | --- | --- | --- |
| `light_dir` | `float[3]` | `[0.5, -0.5, 1.0]` | Directional light 方向。 |
| `light_color` | `float[3]` | `[5.0, 5.0, 5.0]` | 光照颜色/强度。 |
| `sun_angular_radius_degrees` | number | `0.266` | PCSS 使用的太阳角半径，影响软阴影半影大小。 |

### `Settings/Scene.json`

| 键 | 类型 | 说明 |
| --- | --- | --- |
| `skybox_path` | string | 本地 HDR 环境贴图相对路径。 |
| `stress_test` | boolean | `true` 时使用第一个实例的模型生成 `20×20×20`，即 8000 个实例。 |
| `instances` | array | 场景实例列表。 |
| `instances[].name` | string | 实例名称。 |
| `instances[].model_path` | string | 本地 `.glb` 相对路径。 |
| `instances[].pos` | `float[3]` | 平移。 |
| `instances[].rot` | `float[3]` | 按 `XMMatrixRotationRollPitchYaw` 传入，单位为弧度。 |
| `instances[].scale` | `float[3]` | 缩放。 |
| `instances[].is_transparent` | boolean | 进入透明队列并按相机距离从远到近排序。 |
| `instances[].is_cutout` | boolean | 使用 alpha test/cutout PSO。 |
| `instances[].material_override_index` | unsigned integer | 当前会被 JSON 解析，但尚未传递到 `ModelInstance`；属于保留字段，不应依赖。 |

当前加载路径面向二进制 `.glb` 与内嵌资源。外部贴图、复杂动画/骨骼和特殊材质扩展并不是完整支持目标。

## 每帧渲染流程

公共 CPU 阶段：

1. 等待当前 back buffer 对应的 fence。
2. 更新输入、相机、矩阵、jitter 和历史状态。
3. 计算 directional light 与 4 级 CSM。
4. 执行主相机/阴影裁剪和 LOD 选择。
5. 按透明性、模型、LOD 和 cutout 状态排序，生成实例数据。
6. 构建并执行所选路径的 RDG。

Deferred 路径：

```text
Shadow
ZPrepass optional
GBuffer
MotionVector
HBAO -> Blur -> Scalar Temporal Filter
DeferredLighting
Skybox
Transparent
TAA / TSR / DLSS optional
ToneMap -> SMAA when selected
PostProcess
Present
```

Forward/PBR 路径：

```text
Shadow
ZPrepass optional
PBR Opaque
Skybox
Transparent
MotionVector when TAA / TSR / DLSS is selected
TAA / TSR / DLSS optional
ToneMap -> SMAA when selected
PostProcess
Present
```

TAA、TSR、SMAA 和 DLSS 都支持 Forward 与 Deferred。HBAO 只属于 Deferred 路径。DLSS 只有在配置中显式选择时才初始化 NGX，失败时不会自动切换到其他模式。

## 目录结构

| 路径 | 说明 |
| --- | --- |
| `PBR_Render.cpp` | WinMain、初始化、帧更新和两条 RDG 渲染路径。 |
| `Core/` | D3D12App、Win32 窗口和公共 D3D12 头。 |
| `Graphics/` | RenderDevice、PipelineManager、Shader compiler 和 DLSS 管理。 |
| `Resources/` | 模型/材质、ResourceManager、IBL 烘焙与配置加载。 |
| `Passes/` | 可组合的 RDG 渲染 pass。 |
| `RDG/` | Render Dependency Graph 与瞬时资源租约。 |
| `Logic/` | Camera、InputManager、SceneObject。 |
| `Shaders/` | 运行时编译的 HLSL。 |
| `Settings/` | 随仓库提交的 JSON 配置。 |
| `ThirdParty/` | 随仓库提交的头文件、SMAA LUT 和 NVIDIA DLSS SDK。 |
| `packages.config` | 可恢复 NuGet 依赖的版本清单。 |
| `Models/`、`HDRs/` | 本地运行资源；不随仓库提交。 |

## 操作方式

| 输入 | 功能 |
| --- | --- |
| `W/A/S/D` | 前后左右移动相机。 |
| 按住鼠标右键拖动 | 旋转视角。 |
| 鼠标滚轮 | 沿相机前向移动。 |
| `Esc` | 弹出退出确认。 |

## Shader 说明

HLSL 在运行时通过 DXC 编译为 Shader Model 6.6，不依赖 Visual Studio 的 HLSL build step。工程文件把现有 shader 标记为 `Document`。

新增 `.hlsl` 文件后，如果 Visual Studio 自动将其加入 HLSL build step，应把 Item Type 改为 `Does not participate in build` 或 `Document`，否则可能发生重复或参数不一致的提前编译。

## 已知限制

- 只维护 Windows/x64/D3D12；`.vcxproj` 中的 Win32 配置不是当前受支持目标。
- 没有正式资产管线或在线资源下载；模型和 HDR 由使用者在本机准备。
- vcpkg 仍使用 classic/user-wide integration，没有 manifest 锁定 Assimp/meshoptimizer 版本。
- 模型加载面向 `.glb` 内嵌资源；动画/蒙皮字段存在，但当前帧流程没有动画更新。
- `material_override_index` 已进入配置结构，但目前没有应用到创建出的场景实例。
- 透明物体按距离排序以保证混合，可能降低 batching 效率。
- 配置和 shader 没有热重载，修改后需要重启。
- 资源路径大量使用相对路径，工作目录错误会导致初始化失败或资源为空。
- DLSS 是显式且严格的模式：NGX capability、quality/resolution 或 evaluation 失败都会停止相应流程，不会静默降级。

## 给自动化工具和 AI 的维护约定

- 不要提交 `.vs/`、`x64/`、`LearnDirectX/x64/`、`packages/`、PDB/OBJ/TLOG。
- 不要提交 `Models/`、`HDRs/` 或 `Scene_Assets/`；修改场景前先确认目标资源只需存在于本机。
- `ThirdParty/` 是有意提交的依赖，不要把其中的 DLL/LIB 当作普通构建产物删除。
- 排查构建失败时，先区分 NuGet、vcpkg、vendored dependency 和 Windows SDK 四种来源。
- 使用 x64 构建；不要根据 `.vcxproj` 中残留的 Win32 configuration 推断 Win32 已受支持。
- 修改配置协议时，同时更新 `Resources/Settings_Manager.h`、`Settings/*.json` 和本 README。
- 修改 pass 或帧顺序时，以 `PBR_Render.cpp` 的 RDG 构建代码为准，并同步更新两条渲染流程。
