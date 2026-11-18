# LearnDirectX

一个面向实时渲染学习与实验的 Windows / DirectX 12 小型渲染器。

它的重点不是提供完整的游戏引擎编辑器，而是展示一条可以实际运行、调试和继续演进的现代渲染链路：从显式 D3D12 资源与同步管理，逐步组织出 PBR、IBL、Forward/Deferred、SSGI / 简化 Lumen 式漫反射 GI、Card / Surface Cache、Mesh/Global SDF 几何查询、级联软阴影、时域重建、DLSS、SMAA 和 Render Dependency Graph。

## 项目定位与意义

这个项目适合用于理解以下问题：

- D3D12 的 Device、SwapChain、Command Queue/List、Descriptor Heap、Fence 和三缓冲如何协同工作。
- 模型、材质、纹理、IBL、历史缓冲和瞬时 Render Target 如何进入 GPU 生命周期。
- 如何将渲染步骤从手写调用序列演进为具有依赖关系和资源状态管理的 RDG pass。
- Forward 与 Deferred 两条路径如何共享场景、阴影、透明物体、后处理和抗锯齿设施。
- TAA、TSR、DLSS、SMAA、Motion Vector 和时域 HBAO 在真实帧流程中的位置和差异。
- 如何在一个仍然足够小的学习工程中实践视锥裁剪、LOD、实例批处理、PBR/IBL 和 PCSS。

它目前更接近“模型查看器 + 渲染技术试验场”，而不是通用游戏引擎：已有轻量 ImGui 编辑器，支持模型选择、变换、改名和撤销，但没有 ECS、正式资产管线、多平台后端或动画系统。编辑器说明见 [Editor/README.md](Editor/README.md)。

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
| 渲染阶段 | `Passes/` | Shadow、PBR、GBuffer、Motion Vector、HBAO、Surface Cache 捕获/打光、屏幕探针 GI、Temporal、DLSS、SMAA 和 PostProcess。 |
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
| 构建平台 | **x64 Debug 或 x64 Release** | 必需 | Visual Studio / MSBuild | 工程和 NVIDIA SDK 依赖链仅提供 x64 配置。 |
| GPU 与驱动 | D3D12、Shader Model 6.6、直接索引 CBV/SRV/UAV descriptor heap | 必需 | GPU/驱动 | 设备虽可能达到 Feature Level 11_0，Root Signature 或 SM 6.6 shader 仍可能失败。 |
| NuGet | `Microsoft.Direct3D.D3D12` `1.619.1` | 必需 | `packages.config` | 缺少 Agility SDK、DXC props/targets 或运行时 DLL。 |
| NuGet | `directxtk12_desktop_win10` `2026.4.1.1` | 必需 | `packages.config` | 找不到 `ResourceUploadBatch.h`、`WICTextureLoader.h` 或 DirectXTK12 库。 |
| vcpkg | `assimp:x64-windows` | 必需 | 本机 vcpkg classic/user-wide integration | 找不到 Assimp 头文件、库或运行时 DLL。 |
| vcpkg | `meshoptimizer:x64-windows` | 必需 | 本机 vcpkg classic/user-wide integration | 链接时找不到 `meshoptimizer.lib`。仓库只 vendored 了头文件。 |
| Vendored 依赖 | `ThirdParty/` 完整存在 | 必需 | 随仓库提交 | 缺少 tinygltf、stb、JSON、SMAA LUT、NGX/DLSS 或 GI 使用的 STBN 数据、NRD 派生 helper。 |
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

如果当前 Visual Studio 不识别 `LearnDirectX.slnx`，可以直接打开 `LearnDirectX.vcxproj`。当前工程仅提供 x64 构建配置。

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
Models/Cornell_Box.glb
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

构建过程会将对应的 `nvngx_dlss.dll` 和编辑器字体复制到输出目录。NuGet/vcpkg 还会提供 DXC、Assimp 及其运行时依赖。不要只复制 `LearnDirectX.exe` 后期待它独立运行。

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

- 跟踪：生产目录中的 `*.cpp`、`*.h`、`*.inl`、`*.hlsl`、`*.hlsli`、JSON 配置、工程与 filters 文件、维护脚本和说明文档、`packages.config`、`ThirdParty/`。
- 不跟踪：`.vs/`、`x64/`、中间文件、PDB、NuGet 展开目录 `packages/`。
- 不跟踪：`Models/`、`HDRs/`、`Scene_Assets/` 等本地美术资源。
- 不跟踪：`tmp/`、`Captures/`、`Logs/`、`PassViewRun.json` 等验收产物，以及 Python 缓存。

`ThirdParty/` 中的 DLSS DLL/LIB、STBN 数据、SMAA LUT 和随附文档，以及 `Editor/Assets/Fonts/` 中的字体与许可证，都是有意保留的生产依赖。

因此，新 clone 中缺少模型和 HDR 是预期行为，不代表 clone 不完整。若未来需要分发示例场景，应单独提供资源下载地址、release 包或独立资产仓库，而不是重新提交大型二进制到主 Git 历史。

### 当前阶段的提交范围

生产文件按下列目录收录；更新时同时保留旧世界探针代码的删除记录，避免只加入新文件而遗漏工程清理。

| 路径 | 本阶段内容 |
| --- | --- |
| 根目录 | `PBR_Render.cpp` 帧接入，`LearnDirectX.vcxproj` / `.filters` 文件登记，README 和 Git 目录策略。 |
| `Core/`、`Graphics/`、`RDG/` | 应用状态、后台抓帧/有限帧计时、GI 管线与绑定、间接调度依赖。 |
| `Resources/` | `WorldGeometryRuntime`、Mesh/Global SDF、Card 生成/反馈/共享查询数据、屏幕探针布局、GI 来源状态和配置校验；删除旧世界探针与 owner/band 状态。 |
| `Passes/` | Surface Cache 捕获/光照，屏幕探针与 SSGI 调度，相关预览/后处理接入；删除旧世界探针更新/过滤 pass。 |
| `Shaders/` | Surface Cache 捕获/查询/光照，屏幕探针、自适应补点与屏幕/世界发光采样，统一 Hi-Z 追踪；删除停用的世界探针和 owner/band shader。 |
| `Editor/` | 统一 UI 中心的 GI 模式/阶段查看、几何查询诊断、架构检查及说明；删除旧世界探针可视化。 |
| `Settings/`、`ThirdParty/` | 当前启动配置、相机预设，以及相关依赖说明/许可证；不收录本地模型、HDR 或试验输出。 |

Surface Cache、屏幕探针和共享头文件必须同时出现在工程及 filters 中，Shader 保持运行时 DXC 编译。`tmp/` 中的验证脚本、fixture、报告与 `Captures/` 中的原始 atlas 是本地证据，不属于上述提交目录，也不是启动依赖。

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
- Deferred GI：SSGI 屏幕探针与时空重建；Hi-Z 未命中时追踪 Mesh/Global SDF，统一读取 Surface Cache 的直接漫反射与自发光。
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
| `rendering.use_deferred` | boolean | `true` | `true` 使用 Deferred，`false` 使用 Forward/PBR。 |
| `rendering.use_z_prepass` | boolean | `false` | 请求 Z Prepass；TSR 模式下当前会禁用。 |
| `DLSS SR.anti_aliasing` | `None` / `TAA` / `TSR` / `SMAA` / `DLSS` | `DLSS` | 互斥的抗锯齿或重建模式。 |
| `DLSS SR.dlss_quality` | `DLAA` / `Quality` / `Balanced` / `Performance` / `UltraPerformance` | `Quality` | 仅 DLSS 模式使用；不支持时初始化失败。 |
| `shadows.cascades.transition_fraction` | number，0～0.3 | `0.1` | 级联阴影交界处的混合比例。 |

**Debug → Pass → GI** 中只有 **SSGI、Lumen** 两项，点击切换实际模式并查看对应纯 GI。SSGI 只做 Hi-Z 屏幕查询，所有命中仍读取 Surface Cache；Lumen 是本项目现有的屏幕＋Mesh/Global 世界追踪路径名称，不代表完整 UE Lumen 实现。两种模式共用缓存、探针历史/过滤、SH、重建和降噪。菜单选择只作用于本次运行；启动模式由 `ssgi.mode` 设置，默认保留 Lumen。

GI 配置使用同一 `Settings/Pipeline.json`，下表列出当前仓库配置：

| 键 | 当前值 | 说明 |
| --- | --- | --- |
| `ssgi.mode` | `Lumen` | `SSGI` 仅屏幕追踪；`Lumen` 使用现有世界回退。 |
| `ssgi.enabled` | `true` | 启用 Deferred GI，要求 `world_geometry.enabled: true`。 |
| `ssgi.sampling_mode` | `screen_probes` | 当前使用屏幕探针；`legacy` 每接收点追踪四条射线，同样读取 Surface Cache。 |
| `ssgi.screen_trace_distance` | `2` | Hi-Z 屏幕段最大距离，单位为米；SSGI 未命中贡献零，Lumen 未命中后使用 `world_geometry` 世界段。 |
| `ssgi.max_trace_iterations` | `96` | Hi-Z 最大格子访问/层级迭代次数，正整数，与射线样本数无关。 |
| `ssgi.camera_fade.enabled` | `true` | 在最终合成时按接收点到相机的距离淡出 SSGI；不影响环境 IBL。 |
| `ssgi.camera_fade.start` | `16` | SSGI 开始淡出的相机距离，单位为米。 |
| `ssgi.camera_fade.end` | `24` | SSGI 完全淡出的相机距离，单位为米；须大于 start。 |
| `ssgi.screen_probes.screen_rays_per_probe` | `64` | 每个新鲜探针实际追踪的屏幕射线数，均匀分配到 8×8 个方向格。 |
| `ssgi.screen_probes.emissive_samples_per_probe` | `48` | 每个新鲜探针的显式发光样本数；SSGI 使用屏幕像素，Lumen 使用世界三角形；0 改由普通射线估计发射项。 |
| `ssgi.screen_probes.history_updates` | `4` | 屏幕探针方向光照的新鲜批次积累上限，不是最终像素历史上限。 |
| `ssgi.screen_probes.jitter_width_fraction` | `0.5` | 基础发射点抖动区域总宽度占屏幕块宽度的比例；0 关闭基础点抖动。自适应候选仍使用 UE 式时变序列。 |
| `ssgi.screen_probes.adaptive.enabled` | `true` | 按 UE 5.8.4 Mark/Spawn 流程，为基础探针支持不足的可见表面补点；缺少 adaptive 对象时默认关闭。 |
| `ssgi.screen_probes.adaptive.max_fraction` | `0.25` | 额外点池容量相对基础网格点数的比例，有限 0～1；0 等效关闭，实际点数由当前几何决定。 |
| `ssgi.trace_downsample_factor` | `16` | 每个 16×16 内部渲染像素块放置一个基础屏幕探针，独立于 DLSS 输出放大。 |
| `ssgi.denoise_downsample_factor` | `2` | 屏幕探针的像素重建、历史与降噪每轴缩小倍数，只接受 1 或 2；2 约处理四分之一像素，最后按深度/法线上采样。 |
| `ssgi.intensity` | `1` | 间接漫反射强度。 |

当前不再按摄像机距离切换近/远档共享块或射线数。每个屏幕块选择一个有效接收点，
以 8×8 半球方向格保存光照；当前每格一条真实屏幕射线，共 64 条。
每条方向射线先执行 Hi-Z。SSGI 模式未命中即贡献零；Lumen 模式未命中时，从发射点追踪近段 Mesh SDF，再追踪远段 Global SDF。
Lumen 模式下 Global SDF 关闭时，Mesh SDF 追踪完整世界距离。所有命中统一查询 Surface Cache Lo0，不读取 SceneColor；几何命中缺少 Card 覆盖时终止并贡献零。
Lo0 只含直接漫反射与限幅自发光，不含视角相关镜面、环境 IBL 或间接光反馈。
SSGI 模式的 48 个自发光样本从屏幕发光像素选择，按像素表面面积换算立体角权重，用 Hi-Z 检查到灯点的遮挡，发光值读取 Surface Cache。Lumen 模式按世界实例的物理三角形均匀选取，再按面积均匀采点；屏幕外光源同样参与。
有显式采样时，所有普通屏幕/世界命中的发射项都从缓存读数中扣除，避免重复累计；设为 0 或使用 legacy 时，普通射线保留完整 Lo0。
Lumen 显式自发光用已有 Mesh SDF 与三角形确认检查到灯点的有限线段遮挡，包含屏幕外遮挡物。SSGI 的 GI 采样不发射 Mesh/Global 查询，且暂停 Global 场更新；Surface Cache 自身的 Mesh 阴影打光继续保留。切换模式使旧 GI 来源历史失效；颜色 AA 与 DLSS 沿用各自的画面/相机失效规则，调试显示切换另行处理。
Hi-Z 迭代上限只约束屏幕段；世界距离由 `world_geometry.trace_distance` 控制。Global SDF 只提供远段候选，再用已有 Mesh 查询确认剩余线段的真实位置、法线与实例，之后查询 Card。不会把粗场位置和梯度直接当成真实表面取光。

自适应布设沿用 UE 5.8.4 的八候选 Mark/Spawn、前序候选覆盖去重、全局额外池和每 tile 索引。
它用同表面的几何插值权重判断是否补点，真实黑色 GI 不会触发补点。八个候选检查不发射 GI 射线。
基础网格与存储 atlas 分开；额外追踪、方向过滤和 SH 投影由 GPU 实际点数间接调度，未使用的池容量不运行重型 kernel。
过滤按屏幕邻域查找，重建在弱支持的四角中替换更合适的补点；历史按位置、表面和当前可见性匹配，随机采样 key 随有效历史延续。
当前 1707×1067 内部尺寸对应 7169 个基础槽位、最多 1792 个补点；25% 是数量上限，不能据此推算耗时增幅。
最终上采样排除无效像素历史；低分辨率完全漏掉细面时，只对缺少同面支持的完整分辨率像素用已有探针 SH 重建。
深度差分跨越一像素轮廓、与 GBuffer 法线严重不一致时，布设、历史检查和重建共用 GBuffer 法线回退。

相机距离淡出使用 `1 - smoothstep(start, end, distance)`：当前 16 米以内保持完整强度，20 米约一半，24 米及更远为零。
普通反弹与自发光在降噪后的最终合成中使用同一权重；纯 GI 显示也应用该权重，线性 `gi-filtered` 导出和历史缓存保持未淡出的 E/pi。
它独立于 Hi-Z 的 2 米长度与 96 次上限，也不停止世界段追踪或降噪计算。

屏幕射线数须至少为 64 且为 64 的倍数，以便均匀分配到固定的 8×8 方向格；64/128/192 分别代表每格 1/2/3 条真实射线。它与自发光样本数独立，不要求两者整除。
自发光样本数接受 0～64；探针历史批次上限接受 1～32；抖动宽度比例接受 0～1。
探针网格降采样因子接受可表示为 `int` 的正整数；重建/降噪因子仅接受 1 或 2，独立于探针密度；实际可用预算仍受显存与 GPU 时间约束。
屏幕追踪距离须为有限正数。旧世界烘焙探针已删除；Surface Cache 直接光照由现有屏幕探针消费，没有加入世界探针或 radiosity。

调节 SSGI 时优先看以下字段，每次只改变一个：

| 目的 | 字段 | 调节方向 |
| --- | --- | --- |
| 调整体强度 | `ssgi.intensity` | 越大越亮；设为 0 不会跳过计算，停用应使用 `enabled: false`。 |
| 调接收点密度 | `ssgi.trace_downsample_factor` | 当前 16 表示每个 16×16 内部像素块一个基础屏幕探针；增大通常降低采样开销，也降低空间细节，不改变像素重建/降噪分辨率。 |
| 调局部补点额度 | `ssgi.screen_probes.adaptive.max_fraction` | 当前 0.25；增大只扩大上限，实际使用量取决于几何支持不足的候选。 |
| 调重建与降噪分辨率 | `ssgi.denoise_downsample_factor` | 当前 2 在内部渲染分辨率的一半宽高上重建、重投影并降噪；1 恢复完整内部尺寸以作画质/性能对照。可能损失细薄几何及法线贴图上的间接光细节。 |
| 调屏幕查询次数 | `ssgi.screen_probes.screen_rays_per_probe` | 当前 64；增加可增加方向观测，但也增加追踪开销。 |
| 调自发光采样 | `ssgi.screen_probes.emissive_samples_per_probe` | 当前 48，允许 0～64；越多通常越稳定、开销越高，0 关闭额外采样。 |
| 调方向历史积累 | `ssgi.screen_probes.history_updates` | 当前 4，允许 1～32；增大可降低噪声，也可能延迟光照变化的响应。 |
| 调屏幕射线范围 | `ssgi.screen_trace_distance` | 当前 2 米；SSGI 未命中贡献零，Lumen 未命中后继续世界查询。 |

屏幕探针配置包含实际屏幕射线数、自发光样本数、自发光可见性算法、历史批次和抖动宽度，不再保留虚拟总预算或重复计入样本。
未识别的 `screen_probes` 字段会报错，避免无效配置被静默忽略。
建议先保持 `sampling_mode` 和 `jitter_width_fraction` 不变，再按上表逐项调节。
配置在启动时加载，修改后需要重启程序。

屏幕探针采用有历史验证的棋盘格刷新：正常情况下约两帧更新一轮，历史失效时重新追踪。
方向光照先进行时间积累与几何约束的空间过滤，再积分到九系数 SH。像素重建、重投影、RELAX 风格的快慢历史积累、历史钳制和两轮 à-trous 均按 `denoise_downsample_factor` 选择尺寸。
当前因子 2 使用每个 2×2 内部像素块的固定左上接收点，按实际深度和法线进行历史验证；奇数尺寸向上取整。最终通过完整内部尺寸的深度/法线引导上采样并施加材质响应，随后进入所选抗锯齿/重建模式。统计邻域的采样间距也换算到低分辨率，以保留探针尺度的覆盖范围。

后台连续验证可使用 `--frames=N --capture-frame=S --capture-every=K`，从第 S 帧开始按 K 帧间隔导出，普通单帧抓取语义保持原样。
GI 的 `--capture-compact` 只保留像素信号和小计数，`--capture-roi=x,y,width,height` 按内部像素区域裁剪信号读回，并在每个输出中记录 source_region；预览 PNG 保留完整画面。
`--camera-step=x,y,z` 在有限帧运行中按世界坐标每帧平移相机，需要显式的 `--camera-preset`，可复现遮挡揭露和历史复用。抓取帧从 GPU 性能采样中排除；连续画质抓取不用于性能结论。

顶部 **Debug → Pass** 按完整 Pass 选择本步结果或截至本步的累计画面，
未启用项置灰。抗锯齿四选一、与后处理一样只提供累计结果；ToneMap 归后处理。
GI 内只有 SSGI、Lumen 两项，分别切换模式并展示对应的 Surface Cache GI；独立诊断历史保留在 CLI。
纯 GI 显示经过配置中的抗锯齿和 ToneMap；CLI 累计捕获保留 GI 边界快照。抗锯齿 `None` 保持关闭。
后台验收可通过命令行导出 PNG、原始数据与元信息至 `Captures/`，详见 [阶段查看与后台导出](Editor/README.md)。
T 快捷键、Details 的旧 source 选择、JSON 的 `gi_only` 和 `vxgi.hybrid.mode` 已移除；启动为默认正常合成。
世界烘焙探针的代码、初始化、更新、过滤、调试、抓帧读回与工程登记已全部删除；旧 world-owner/band 缓存及共享/修补 PSO 同步移除。
Mesh/Global SDF 的几何构建、实例 BVH、GPU 上传与滚动更新已恢复。当前 Mesh 查询用距离场跳步并以三角形精确确认命中，Global 几何诊断仍返回粗场命中；Lumen 模式的远段在粗命中后补做 Mesh 确认。Surface Cache 已有 Card 生成、材质捕获、统一查询和直接光照 atlas，并接入 SSGI 屏幕/世界取光。
GI 历史跟踪真实场景/材质/光照内容、模式和有效采样/追踪设置；Card 换档、共享、排布、回收及捕获/重光照批次不作为来源内容变化，不主动重置 GI、TAA 或 DLSS。未被当前模式使用的追踪参数不引起无关的 GI 失效。
Debug → Pass → GI 展示所选 SSGI 或 Lumen 模式；旧 Mesh SDF GI 条目已删除。几何诊断使用独立 CLI，不新增编辑器面板。

#### 当前阶段状态（2026-11-18）

本阶段以简化 Lumen 式漫反射 GI 收尾：屏幕探针 → Hi-Z / Mesh / Global 查询 → Surface Cache 取光 → 过滤、SH、重建和最终时空降噪已接通。实现参考本地 UE **5.8.4** 的渲染源码；编辑器外观参考仍为 UE **5.4.4 Starship**，两者版本用途独立。

兼容实例已共享实际材质/深度 Card 矩形，光照仍按实例保存。两个康奈尔盒子的 64×64 Card 受控测试中，24 张实例 Card 使用 12 份材质缓存，材质 atlas 面积减半；这不是整个引擎显存或帧率的测量。

收尾审查中，主时域、A-trous、探针过滤/SH/重建和最终合成与对应基线逐段核对；16/6 累计参数保持。10 项隐藏 GPU 回归通过，覆盖两种 GI 模式下的 TAA/DLSS 真实 Card 换档、关闭 GI/AO 后的 Forward/Deferred 缓存开关对照，以及屏外发光路由；D3D12 验证均为零错误/零警告。换档保留对应历史，关闭 GI/AO 的最终输出对照逐字节一致。AO 开启时存在少量 1 色阶差异，同配置重复运行也可出现；未修改其依赖帧时长的时域滤波。

Debug/Release 构建与配置/来源失效检查通过，测试设置已恢复。本地证据保留在 `tmp/lumen-final-review/` 和 `tmp/surface-cache-sharing/`，这些目录不随 Git 分发；验证结论限于所列用例。

`Pipeline.json` 的 `world_geometry` 配置：`enabled` 开启几何模块，`mesh_cell_size` 设置局部距离场格长，`mesh_trace_distance` 设置混合追踪的近段范围，`trace_distance` 设置世界追踪上限。`global_sdf` 的 `enabled`、`cells`、`clipmaps`、`cell_size` 对应已有 Global SDF 配置。屏幕追踪仍使用 `ssgi.screen_trace_distance`。

格长和距离必须是有限正数，且 `mesh_trace_distance <= trace_distance`。Surface Cache 的 `min_card_resolution`、`max_card_resolution`、`atlas_size` 必须是 `[8,16384]` 内的整数和二次幂，并满足 `min <= max <= atlas_size`；先校验再转为无符号尺寸，拒绝超限、回绕或溢出为无穷值的输入。

`surface_cache.card_cell_size` 设置模型局部 Card 生成的目标格长（默认 0.1 米，与 Mesh SDF 格长独立），每轴最多 64 格；大资产会相应增加实际格长。`surface_cache.max_cards_per_mesh` 为六个方向共享的总预算（默认 12，可设 1～32）。启动时按共享几何资产构建，实例变换不重建局部 Cards。

Card 生成保留六方向多次相交采样、半球可见性、捕获近裁面分组与加权覆盖裁剪。内部 Cards 有独立捕获深度范围，一个方向可以有多张。当前只处理静态不透明几何，不引入植被、双面或透明材质专项生成策略。

使用 `--card-view=placement` 查看 Card OBB 的穿透线框，`--card-view=coverage` 查看几何捕获覆盖。覆盖图从每张 Card 的实际近裁面重新查询所属实例，能够看到的表面以 Card ID 着色，缺覆盖显示粉色。这是几何可见性参考，不是已经捕获的材质/光照 atlas；边缘处也可能出现少量数值匹配缺口。

材质捕获按相机到每个实例 Card 世界 OBB 的最近距离 `d` 选择基础密度，包含实例缩放，不能用 Card 中心距离代替。采用 UE 普通近场 Card 基础公式的米制等价形式：`目标 texel = max(surface_cache.world_texel_size, 0.02 * max(d, 1m))`。默认近处目标 0.1 米/texel，5 米以后随距离变粗。宽高分别使用 `ceil_pow2(floor(世界尺寸 / 目标 texel))`，然后限制在 `min_card_resolution`～`max_card_resolution`（默认每轴 16～512）。取整与上下限会使实际 texel 偏离目标；此设置独立于生成 Card 几何的 `card_cell_size`。

`surface_cache.max_distance` 默认 32 米，OBB 最近距离超过该值的 Card 不参与当前 atlas 捕获或查询。它独立于 GI 接收面的 16～24 米淡出，以及 SSGI 的 2 米 / Lumen 的 128 米追踪距离；Lumen 在缓存覆盖范围外命中且没有匹配 Card 时仍贡献零。范围裁剪不移除 Mesh/Global SDF 几何，也不移除用于直接光照阴影的遮挡物。

每张实例 Card 固定保留 256 个未使用反馈帧：成功的 GI 缓存查询记录使用，连续未使用后将该 Card 的矩形移出 Atlas，并沿用整图贪心重排回收空间。保留 Card 描述；GI 几何命中落在已回收 Card 的局部范围内时提交重新申请，后续帧重新分配并初始化。新进入 32m 范围的 Card 先申请一次。反馈按正常帧槽异步读回，过期判断以已完成反馈为准，因此 CPU 执行回收会有帧槽延迟；没有额外 GPU 等待。材质捕获、Lo0 重算及调试预览不会把 Card 标记为 GI 已使用。没有页表、锁定页、常驻基础层或空闲矩形增量分配器。回收、重新申请和重排不会主动清屏幕 GI/TAA/DLSS 历史。

tile 通过剩余矩形的贪心排布放入常驻的二次幂尺寸 atlas：分别尝试“面积/长边优先、短边余量最小”与“高度优先、左下放置”，放置后分割/清理剩余矩形以回填空隙。不旋转 Card，排布保持实例/Card 查询顺序。候选 atlas 按面积、长边从小到大尝试，实际宽高均不超过 `surface_cache.atlas_size`（默认 2048）；这是启发式排布，不保证全局最优。没有虚拟页或流送。

空间不足时先保留已驻留 Card，新增申请先尝试目标尺寸，再将宽高各减半，最多降两档，每轴不低于 `min_card_resolution`（默认 16）。仍放不下则保留描述，暂不加入 Atlas，不中止渲染；已有 Card 升档失败时继续旧档。实际分配尺寸会被记住，同一目标档位下沿用实际尺寸。拒绝的申请只在目标档位、驻留集合或容量变化后重试，普通 GI 请求不会反复触发排布和重捕获。256 帧未使用回收保持独立，没有增加预算驱动的提前淘汰。

捕获使用 Card 的局部正交相机和内部近裁面，只绘制所属模型的 LOD0，保留当前材质的纹理、系数、法线贴图及实例 cutout 设置。共享几何只生成一次 Cards；兼容实例共用材质 tile，光照 tile 按实例保存，法线按捕获实例的世界空间保存。场景、材质、Cards、上述配置或相机引起的 Card 集合/分辨率档位变化时仍整批重捕获材质。光照 Lo0 在换档/重排时按同一实例/Card 的局部 UV 从旧 atlas 双线性迁移，采样限制在旧 tile 的 texel 中心范围内；随后每帧覆盖一个至多 64×64 texel 的块，未更新的块继续使用迁移后的值，全部完成后停止，直到下一次来源或捕获变化。更新途中再次换档，迁移当前已有结果并重新开始这一轮。首次启动、几何/Card 重建或没有旧数据的新 Card 直接完整初始化。没有缓存端的 Radiosity 时域混合或反馈优先级调度。换档、重排、普通捕获/重光照批次不使屏幕 GI 或 TAA/DLSS 历史失效；真实场景/材质/灯光内容变化、模式/有效追踪参数变化仍沿用原有 GI 来源失效规则，TAA/DLSS 保持各自的正常失效规则。SSGI 不因未使用的世界追踪设置改变而失效，Lumen 不因关闭的 Global SDF 参数改变而失效。主时域降噪 shader 和 16/6 累计参数保持不变。移动相机但 Card 集合和档位未变时继续本轮光照更新或复用已完成的 atlas；当前不加档位迟滞。

相同模型资源、有效材质 ID 列表、cutout 设置、带符号缩放及 Card 分辨率一致的实例，共用同一张局部 Card 的材质/深度矩形；平移和旋转不拆分共享。只捕获一次，不为每个实例复制材质纹理。光照矩形、距离档位、预算接纳和使用反馈仍按实例管理；不同分辨率暂不跨档共享。法线纹理保留捕获实例的世界法线，查询和打光时经模型空间转换到目标实例，因此各实例仍独立计算阴影和 Lo0。共享者离开或被回收后，剩余实例会重新选择捕获实例。

导出中的 `rect`、`width/height` 表示实例光照布局；`material_rect`、`material_instance_index` 和 `material_width/material_height/material_tiles` 表示共享材质布局与捕获实例。材质预览和四张原始材质纹理使用材质布局，光照预览使用实例光照布局。全图重捕获及光照迁移/分帧更新策略保持原义。

| Atlas 输出 | 格式 | 通道 |
| --- | --- | --- |
| Albedo / Metallic | RGBA16F | 线性 base color RGB，metallic A。 |
| Normal / Roughness | RGBA16F | 世界空间有符号 shading normal XYZ，roughness A。 |
| Emissive / Source | RGBA16F | 未限幅线性 emissive RGB，允许作为 GI 光源的标记 A；沿用现有 unlit 排除规则。 |
| Depth | D32 / R32F SRV | Card 正交深度，近面 0、远面 1；清除值 1 表示无捕获表面。 |
| Radiance / Visibility | RGBA16F | 线性出射漫反射光源 Lo0 RGB，方向光可见性 A；有效表面仍由 depth 判断。 |

`--card-view=albedo|normal|emissive|depth|metallic|roughness|lighting|visibility` 查看真实 atlas；每次导出同时保存上述五组原始纹理和 Card 坐标/实例变换元信息。捕获坐标以 `right = axis_x`、`up = cross(axis_x, axis_z)`、前向 `-axis_z` 构成左手相机；tile 内像素从左到右、从上到下排列。深度可还原模型局部位置，再由实例矩阵还原世界位置。

`Shaders/SurfaceCacheQuery.hlsli` 提供统一的世界位置 / 单位法线查询。已知实例时只访问该实例的连续 Card 范围；实例未知时以位置容差查询现有实例 BVH，遍历所有重叠叶子。每张 Card 根据朝向、捕获深度和世界法线匹配，四点插值逐 tap 排除无效/不匹配表面，采样限制在该 tile 内；最后选取匹配权重最高的 Card，返回材质、UV 和还原的世界位置。缺少覆盖时返回未匹配。

`SurfaceCacheLightingPass` 使用 atlas 深度还原 texel 世界位置，以已有 Mesh SDF 的三角形确认/alpha 覆盖查询计算世界空间硬阴影，然后调用现有 `GIComputeSourceRadiance`。Lo0 只含方向光的直接漫反射和自发光；不含 IBL、AO、镜面或 GI 反馈。方向光沿用 `Lighting.json` 的方向和颜色。GI 自发光沿用现有峰值 2 的同比例限制，原始材质 emissive 继续保存未限幅值，unlit 不贡献 GI 光源。

场景、材质、Cards、捕获布局或方向光变化时整批重算光照，灯光变化不重捕获材质。移动相机但未改变捕获 Card 集合和分辨率档位时复用光照。阴影使用 `world_geometry.trace_distance` 范围内的完整不透明/cutout 世界几何，不依赖相机 CSM 或屏幕可见性。查询返回匹配 tap 的 Lo0 和可见性，并按同一权重返回逐 texel 限幅后的自发光，供后续 SSGI 估计器区分发射项。

查询诊断：`--card-query=mesh` 使用精确 Mesh 命中的实例；`--card-query=point` 隐去同一命中的实例以验证空间候选路径；`--card-query=global` 使用 Global SDF 粗命中与梯度法线。`--card-query-channel=albedo|normal|emissive|card-id|lighting|visibility` 选择显示。导出同时包含原始 atlas、查询材质、光照/可见性、输入位置、实例/Card/UV 和还原位置，未匹配命中显示粉色。Mesh/point 诊断使用 0.001 米位置容差；Global 诊断使用 `2 * (clipmap 格长 + mesh 格长)` 的显式粗邻域，该取值不保证所有变换下的误差上界。Global 仍会提前命中并扩大物体轮廓，查询结果保持近似。

例如隐藏运行并导出几何命中图：

```powershell
.\x64\Debug\LearnDirectX.exe --hidden --frames=32 --capture-frame=32 --sdf-view=hybrid-hit
```

`--sdf-view` 支持 `mesh-slice`、`mesh-hit`、`mesh-normal`、`mesh-shadow`、`global-slice`、`global-hit`、`global-normal`、`hybrid-hit`。切片位置通过 `--sdf-slice=z,3` 指定，轴可为 x/y/z。几何图直接显示查询结果，不经过 AA 或 ToneMap；命中位置按报告中的 mesh envelope 映射到 RGB，法线显示为 `N*0.5+0.5`，Mesh 遮挡/无遮挡显示红/绿。Global 的命中位置和法线属于粗距离场结果，精度受格长限制，不当作精确表面位置。

有限帧报告中的 `world_geometry` 记录实例/BVH、资产构建、Global SDF dispatch 与各层位置，以及 Card 回收、重载、预算降档和拒绝次数。几何资产保持常驻，Card 纹理驻留按上述规则管理。
恢复世界 GI 时需要接入新的表面缓存，并显式恢复对应执行入口，单独修改 JSON 不会恢复旧路径。

`hbao` 对象控制屏幕空间环境遮蔽（当前在 Deferred 路径使用）：

| 键 | 类型 | 默认值 | 说明 |
| --- | --- | --- | --- |
| `hbao.enabled` | boolean | `true` | 关闭时跳过计算、空间滤波、时域滤波及几何历史复制，保留材质 AO。 |
| `hbao.intensity` | number | `2.0` | 非负遮蔽强度；设为 0 不会跳过计算。 |
| `hbao.radius` | number | `1.0` | 正数，场景单位下的遮蔽半径。 |
| `hbao.quality` | integer | `1` | 只接受 1～4；缺失时保持原来的 4 × 4 采样预算。 |

| `quality` | 方向 × 每方向步数 | 每像素最大深度采样数 |
| --- | --- | --- |
| 1 | 4 × 4 | 16 |
| 2 | 6 × 6 | 36 |
| 3 | 8 × 6 | 48 |
| 4 | 12 × 8 | 96 |

四档都保留距离抖动，避免固定采样距离在角落形成明暗条带。1 档保留已经验证的距离抖动采样分布；2～4 档每个方向先采约 1、2 个像素的近处，再通过带距离抖动的平方分布覆盖剩余范围，让近处更密集。近处采样受搜索范围约束。切换质量不改变搜索范围、遮蔽半径、强度或滤波参数，仍沿用原来的 UV 半径估算及限幅。

采样数仅表示预算上限，不能据此直接推算 GPU 耗时或整帧性能比例。Bias 和降噪参数继续由实现固定；修改 JSON 后重新启动生效。

HBAO 的 5 × 5 空间模糊组合像素距离、法线一致性和两端切平面的距离权重。平面误差取两端点到对方切平面距离的最大值，容差为当前深度下一像素空间尺寸的一半（最小 `1e-4` 场景单位），通过软衰减保留斜面上的模糊并抑制跨表面混合。深度、AO 和法线按同一像素读取，位置与法线统一到视空间；共面距离和像素尺寸计算放在 `GeometryCommon.hlsli`，与 SSGI 共用。

TAA 和 TSR 共用 jitter 逻辑及一套双缓冲 temporal history，但拥有各自的 pass、shader、PSO、Root Signature 和常量。DLSS 使用独立 NGX 状态、jitter、历史有效性和输出资源。SMAA 在 tone mapping 后执行边缘检测、权重计算和 neighborhood blending。

后处理写入 8 位 SDR 目标前，以及 HDR Pass 转成显示预览时，统一在 ToneMap/Gamma 后加入低于半个色阶的蓝噪声抖动，减轻平滑渐变的量化轮廓。复用现有 STBN 纹理的固定第 0 层，RGB 共用噪声且 Alpha 不变；LDR 预览直接复制，原始 HDR 与有符号差值导出保留原值。此行为无需增加 Pipeline.json 参数。

### `Settings/Lighting.json`

| 键 | 类型 | 当前值 | 说明 |
| --- | --- | --- | --- |
| `direct light.light_dir` | `float[3]` | `[0.5, -0.5, 1.0]` | Directional light 方向。 |
| `direct light.light_color` | `float[3]` | `[4.0, 4.0, 4.0]` | 光照颜色/强度。 |
| `direct light.sun_angular_radius_degrees` | number | `0.266` | PCSS 使用的太阳角半径，影响软阴影半影大小。 |
| `environment.intensity` | number | `0.1` | 同时控制 SH 漫反射和环境高光强度；必须为有限非负数。 |

### `Settings/Scene.json`

| 键 | 类型 | 说明 |
| --- | --- | --- |
| `skybox_path` | string | 本地 HDR 环境贴图相对路径。 |
| `instances` | array | 场景实例列表。 |
| `instances[].name` | string | 兼容旧场景的名称字段；当前初始显示名取模型源文件名，运行时可在编辑器改名。 |
| `instances[].model_path` | string | 本地 `.glb` 相对路径。 |
| `instances[].pos` | `float[3]` | 平移。 |
| `instances[].rot` | `float[3]` | 按 `XMMatrixRotationRollPitchYaw` 传入，单位为弧度。 |
| `instances[].scale` | `float[3]` | 缩放。 |
| `instances[].is_transparent` | boolean | 进入透明队列并按相机距离从远到近排序。 |
| `instances[].is_cutout` | boolean | 使用 alpha test/cutout PSO。 |
| `instances[].material_override_index` | unsigned integer | 可选的全局材质索引；默认 `0xFFFFFFFF` 使用网格原材质，指定有效索引时覆盖实例材质。 |

当前加载路径面向二进制 `.glb` 与内嵌资源。外部贴图、复杂动画/骨骼和特殊材质扩展并不是完整支持目标。

## 每帧渲染流程

公共 CPU 阶段：

1. 等待当前 back buffer 对应的 fence。
2. 更新输入、相机、矩阵、jitter 和历史状态。
3. 计算 directional light 与 4 级 CSM。
4. 执行主相机/阴影裁剪和 LOD 选择。
5. 按透明性、模型、LOD 和 cutout 状态排序，生成实例数据。
6. 更新独立 Mesh/Global SDF 世界几何；按变化捕获 Card 材质 atlas，并更新其直接漫反射/自发光光照 atlas。
7. 构建并执行所选路径的 RDG；Deferred GI 使用屏幕探针或纯屏幕 legacy 追踪。

Deferred 路径：

```text
Shadow
ZPrepass optional
GBuffer
MotionVector
HBAO -> Blur -> Scalar Temporal Filter
DeferredLighting
SSGI (screen probes or legacy screen tracing)
GI reconstruction -> temporal/spatial filtering -> HDR composite
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

TAA、TSR、SMAA 和 DLSS 都支持 Forward 与 Deferred。HBAO 与上述混合 GI 当前只接入 Deferred 路径；关闭 GI 时一起跳过其重建、滤波和合成。DLSS 只有在配置中显式选择时才初始化 NGX，失败时不会自动切换到其他模式。

## 目录结构

| 路径 | 说明 |
| --- | --- |
| `PBR_Render.cpp` | WinMain、初始化、帧更新和两条 RDG 渲染路径。 |
| `Core/` | D3D12App、Win32 窗口和公共 D3D12 头。 |
| `Graphics/` | RenderDevice、PipelineManager、Shader compiler 和 DLSS 管理。 |
| `Resources/` | 模型/材质、ResourceManager、IBL、Mesh/Global SDF、Surface Cache、屏幕探针布局及配置/历史来源状态。 |
| `Passes/` | 可组合的 RDG pass，以及 Surface Cache 捕获/打光和屏幕探针调度。 |
| `RDG/` | Render Dependency Graph 与瞬时资源租约。 |
| `Logic/` | Camera、InputManager、SceneObject。 |
| `Shaders/` | 运行时编译的 HLSL。 |
| `Settings/` | 随仓库提交的 JSON 配置。 |
| `ThirdParty/` | 随仓库提交的头文件、SMAA LUT、DLSS SDK、STBN 数据、NRD 派生 helper 及许可证。 |
| `packages.config` | 可恢复 NuGet 依赖的版本清单。 |
| `Models/`、`HDRs/` | 本地运行资源；不随仓库提交。 |

## 操作方式

| 输入 | 功能 |
| --- | --- |
| 右键 + `W/A/S/D` | 前后左右移动相机。 |
| 右键 + `E/Q` | 沿世界竖直方向上升/下降。 |
| 移动时按住 `Shift` | 约 8 秒从原速逐渐加至 64 倍；松开或停止移动后恢复原速。 |
| 按住鼠标右键拖动 | 旋转视角。 |
| 鼠标滚轮 | 沿相机前向移动。 |
| `Esc` | 弹出退出确认。 |

## Shader 说明

HLSL 在运行时通过 DXC 编译为 Shader Model 6.6，不依赖 Visual Studio 的 HLSL build step。工程文件把现有 shader 标记为 `Document`。

新增 `.hlsl` 文件后，如果 Visual Studio 自动将其加入 HLSL build step，应把 Item Type 改为 `Does not participate in build` 或 `Document`，否则可能发生重复或参数不一致的提前编译。

## 已知限制

- 只维护 Windows/x64/D3D12；工程已移除不受支持的 Win32 配置。
- 没有正式资产管线或在线资源下载；模型和 HDR 由使用者在本机准备。
- vcpkg 仍使用 classic/user-wide integration，没有 manifest 锁定 Assimp/meshoptimizer 版本。
- 模型加载面向 `.glb` 内嵌资源；动画/蒙皮字段存在，但当前帧流程没有动画更新。
- 透明物体按距离排序以保证混合，可能降低 batching 效率。
- 配置和 shader 没有热重载，修改后需要重启。
- SSGI 模式的可见性和发光采样受屏幕限制；Lumen 模式使用 Mesh/Global 世界回退和世界三角形发光采样。
- 精简版 Surface Cache 已接入两种 GI 模式；当前只缓存直接漫反射与自发光，尚无多次间接反弹。
- 尚无缓存端 Radiosity 或世界空间 Radiance Cache；没有 SSR 或屏幕外场景反射链，现有镜面环境光来自 IBL。
- Surface Cache 仍使用未压缩纹理、单档 Card 分辨率，没有 BC 压缩、同时驻留的 mip 链或独立远场低密度层；不采用页表是当前设计选择。
- 资源路径大量使用相对路径，工作目录错误会导致初始化失败或资源为空。
- DLSS 是显式且严格的模式：NGX capability、quality/resolution 或 evaluation 失败都会停止相应流程，不会静默降级。

## 给自动化工具和 AI 的维护约定

- 不要提交 `.vs/`、`x64/`、`LearnDirectX/x64/`、`packages/`、PDB/OBJ/TLOG、`tmp/`、`Captures/`、`Logs/`、`PassViewRun.json` 或 Python 缓存。
- 不要提交 `Models/`、`HDRs/` 或 `Scene_Assets/`；修改场景前先确认目标资源只需存在于本机。
- `ThirdParty/` 是有意提交的依赖，不要把其中的 DLL/LIB 当作普通构建产物删除。
- 排查构建失败时，先区分 NuGet、vcpkg、vendored dependency 和 Windows SDK 四种来源。
- 使用 x64 构建；不要添加未配套验证依赖链的 Win32 configuration。
- 修改配置协议时，同时更新 `Resources/Settings_Manager.h`、`Settings/*.json` 和本 README。
- 修改 pass 或帧顺序时，以 `PBR_Render.cpp` 的 RDG 构建代码为准，并同步更新两条渲染流程。
