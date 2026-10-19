# VXGI：固定探针、距离场追踪与混合 SSGI

阶段 1 建立 CPU 世界几何缓存，并绘制占据格子和探针；阶段 2 加入距离场与 GPU 表面追踪；
阶段 3 按真实命中 UV 查询材质、计算直接漫反射与自发光 `Lo0`；
阶段 4 在 GPU 上分类固定探针、采集 `Lo0` 并保存 4 个 RGB SH 系数。
阶段 4 的独立 CPU/GPU 数值测试及 Debug/Release 主工程构建已通过，主场景分类、更新与编辑失效已有运行记录。
后台集成检查已完成；下文分别记录实际 UI、headless 数值验证与有限性能采样的覆盖范围。
阶段 5 已接入六邻居可见性缓存、独立 SH 滤波诊断和逐贡献者可见的八角点表面查询。
阶段 1—6 的代码接入与后台验收已完成；第五、六步 CPU/GPU 数值与 production wrapper 后台验收、shader 编译和 Debug/Release 主工程构建均通过。
第五步按用户要求只做后台验证，未检查新增诊断的实际画面。第六步已接入混合 SSGI，具体契约和验收范围见文末。
阶段 7.3 的 alpha-cutout 全链路和后台回归已完成；当前覆盖与失效契约见文末。
阶段 7.4 已完成 post-alpha 全场基线和真实离屏验收；当时发现实景数值可用性与世界查询性能的阻断项，见文末。
阶段 7.5 已加入密集格子 BVH 和可配置的 GI 追踪分辨率，后台相同输入对照显示世界查询成本显著降低。
阶段 7.6 已按用户要求纠正回退语义：只将原 SSGI 有限射线变换到世界空间，命中后读取探针缓存；没有现场命中点着色或独立世界距离扩展。
阶段 7.7 进一步按用户要求精简：世界命中后只读缓存、插值并求值SH，不再向八邻探针发出可见性射线。
当前保持每轴 1/2 的 GI 追踪与历史分辨率，调试绘制 Off。旧 camera anchor 不参与生产回退；严格探针端点查询仅供旧辐照度诊断。
主程序含探针更新、DLSS 和编辑器的整帧预算尚未测量。下文阶段 6—7.6 的回退与性能报告保留为当时记录，当前契约见阶段 7.7。
当前生产探针调度已改为事件驱动的有限缓存：首次填充达到配置的成功更新次数后停止；模型、材质或光照事件才重新排队。
循环批次及“32 次只限制混合权重”的下文描述属于旧版本；当前约定与后台回归见文末“探针事件驱动缓存”。
阶段 1—7.2 的验收数字保留为当时的记录：完整 cutout 遮挡、mip 只清 SH 的旧规则及 7.2 性能均属于 alpha 接入前版本，不能当作当前结果。

## 分配规则

- 从 `ResourceManager::GetSceneInstances()` 登记全场景实例，使用各 mesh 的 LOD0。
  相机剔除、`isVisible`、渲染 LOD 和 `isDirty` 不决定 GI 参与状态。
- 世界格子以世界原点为锚，坐标为 `floor(position / voxel_size)`。
  缓存 bounds 是当前占据格子的包围范围，不裁掉范围外的场景实例。
- 先用三角形 AABB 枚举候选，再用 13 轴三角形/盒子 SAT 判断真正相交。
  表面恰好位于格子边界时，保守地分配两侧格子；带有微小数值容差。
- 空格子不分配 SH；占据格子按整数坐标排序，每格一个固定中心探针。
  保留每格三角形候选列表、实例 ID、源 mesh/三角形/顶点索引和材质覆盖 ID。
- Cutout 按完整三角形保守登记并保留标记；阶段 7.3 在精确射线候选命中时评估 alpha 洞。透明实例暂时跳过。
- TRS、模型指针或几何元数据变化触发完整重建，相机变化复用缓存。
  材质因子、已有纹理身份、unlit、材质覆盖和 cutout 标记刷新材质/命中身份，不重建表面格子、BVH 或距离数据。
  阶段 7.3 中影响 alpha 覆盖的变化另使探针分类及邻边失效。单改探针格子尺寸只重分配探针格子，复用独立的 SDF。
  原地修改静态资产的顶点/索引内容后，调用 `VXGIWorld::Invalidate()`。
  阶段 4 分类后可禁用探针，探针不迁移；阶段 5 的邻居和表面查询均单独验证几何可见性。

## 使用

配置放在已有的 `Settings/Pipeline.json`：

```json
"vxgi": {
    "enabled": true,
    "voxel_size": 0.5,
    "debug_view": 0,
    "probes": {
        "probes_per_frame": 256,
        "classify_rays": 64,
        "rays_per_update": 64,
        "max_history_updates": 32,
        "backface_threshold": 0.25,
        "filter_neighbor_weight": 0.25
    },
    "sdf": {
        "cell_size": 1.0,
        "slice_axis": 2,
        "slice_position": 0.0
    },
    "surface": {
        "material_mip_level": 0.0,
        "ray_min_distance": 0.0001,
        "max_ray_distance": 1000.0
    }
}
```

`debug_view` 为 0 关闭、1 探针、2 格子、3 格子与探针、4 距离切片、5 命中位置、
6 命中几何法线、7 太阳可见性、8 表面 albedo、9 材质法线、10 metallic、11 未截断自发光、
12 直接漫反射＋GI 源自发光、13 GPU 探针状态、14 探针 SH、15 更新年龄、16 滤波 SH、
17 邻探针可见性、18 表面查询 `E/pi`、19 可见支持权重、20 缺失/负值标记。Details 面板的 **VXGI allocation**
可即时切换视图，并显示实例数、LOD0 三角形数、格子/探针数、CPU 内存估算、构建时间和 revision。
配置文件中的启用状态与格子尺寸在启动时读取。

追踪预算放在同一文件已有的 `ssgi` 对象中，阶段 7.6 已恢复原半分辨率：

```json
"ssgi": {
    "trace_downsample_factor": 2
}
```

该片段只展示新增字段，其余已有 `ssgi` 参数继续保留。合法值为整数 2、4、8，表示相对内部渲染分辨率的每轴缩小倍数，
追踪、重建、降噪和两套历史资源使用 `ceil(width/factor) × ceil(height/factor)`；深度金字塔仍逐级缩小 2 倍。
现有 C++ 默认值与当前项目 JSON 均为 2；修改文件后重启应用读取。该预算由配置选择，尚无按 GPU 时间自动切换档位的机制。

顶部 **Debug → Pass** 是临时查看入口：默认显示正常场景合成，关闭 VXGI 保留正常合成和 SSGI、仅关闭世界补充，
仅 SSGI 显示纯屏幕间接光，
仅 GI 显示屏幕空间与世界探针补充的完整间接照明 `E/pi * intensity`，不乘接收表面原色。
查看状态只在运行时保存，启动为默认。T、Details 的旧 source 选择、`ssgi.gi_only` 与 `vxgi.hybrid.mode` 配置入口已移除。
GI 需要 deferred 与 `ssgi.enabled`，否则三个 GI 相关选项禁用。世界补充仍需 `vxgi.enabled` 和有效缓存，缺失时回到 screen-only。
命中点缓存查询只做八角点三线性读取与归一化，不进行逐探针几何验证。
`vxgi.surface.max_ray_distance` 仍供独立缓存更新/材质诊断使用，
不会延长 SSGI 的回退射线；它的长度来自 `ssgi.radius` 和原屏幕追踪的近裁剪限制。

蓝色线框表示占据格子，黄色十字表示固定探针。此阶段使用不测深度的调试绘制，能看到物体内部的候选探针。
调试几何在后处理之后、选择轮廓和编辑器 UI 之前绘制，使用未抖动相机矩阵。
所有格子都通过 GPU instancing 绘制，每个在途帧的中心上传数据为 `12 * cellCount` 字节。
CPU 内存统计包含保留容器容量与 hash 节点估算，不包含资产本体、临时构建峰值或 GPU 缓冲。

## 验证

独立 CPU 测试在 VS Developer 命令行中运行：

```bat
if not exist tmp mkdir tmp
cl /nologo /std:c++20 /EHsc /W4 /WX /Fe:tmp\VXGIWorldTests.exe /Fo:tmp\VXGIWorldTests.obj Tests\VXGIWorldTests.cpp
tmp\VXGIWorldTests.exe
```

测试覆盖三角形 SAT、空 AABB 角、格子边界、稀疏索引、零 SH、变换/材质覆盖、
相机 LOD 与可见标记不影响 revision、编辑失效、cutout/透明策略及失败后保留缓存。
应用通过 `LearnDirectX.slnx` 构建，工作目录使用仓库根目录。
`--ui-scale=1.0` / `--ui-scale=1.5` 可在不改变系统 DPI 的情况下检查实际编辑器组件。

当前静态小场景采用同步 CPU 重建；大量编辑或密集几何可能出现重建停顿。
超过 1 亿次三角形/格子候选测试或 200 万格子会报告错误，提示增大格子尺寸。
阶段 2—5 的距离场、源表面查询、探针采光和 SH 表面查询分别在下文说明。

### 阶段 1 验收记录（历史成本）

- Debug 与 Release 主工程构建通过；UI 架构检查通过。既有编译警告仍存在。
  沙箱命令行漏载用户 vcpkg 集成时，本机临时构建参数为
  `/p:VCLibPackagePath=C:\Users\28499\AppData\Local\vcpkg\vcpkg.user`，未改变项目依赖配置。
- CPU 独立测试优化版本以 `/W4 /WX` 通过 6 组测试，包括镜像变换、远坐标与非二进制格子尺寸的精度检查。
- 当前汽车与地面场景：2/2 实例，1 个 cutout 实例，129,386 个 LOD0 三角形，
  格子尺寸 0.5，53,306 个格子/固定探针。CPU 保留缓存约 19.47 MiB；两次 Release 启动构建约 155–167 ms。
  这些是当前场景的启动观测值，不代表后续 SDF/光照的成本。
- 已检查实际蓝色格子/黄色探针绘制和关闭调试后的原场景；相机拉远后分配数、几何 hash 与 revision 不变。
- 已检查 100%/150% UI 缩放、窄 Details 和 F10 中的共享只读属性/换行说明组件。
  窄布局发现的说明裁切已通过中心组件换行修正。

## 阶段 2：距离场与表面命中

`Resources/VXGISDF.h` 为世界三角形建立 CPU BVH，烘焙真实最近表面距离。
保存无符号节点距离，开放地面与封闭模型均能参与遮挡；此阶段不使用符号判断探针内外。
距离体是独立的 dense 节点格，覆盖几何 bounds 加一格边界；它不分配探针或 SH。
原有探针仍只存在于真正与模型三角形相交的稀疏格子中。

- `sdf.cell_size` 独立于 `voxel_size`。每个模型按自己的局部几何包围盒和格距，
  自动计算 XYZ 格数与节点数，并保留两侧各一格的边界；不需要配置 `max_resolution`。
  格距不会为了适应固定的 128/256 轴上限而被放大。
  内部仅保留总节点分配预算和索引/坐标精度校验，避免极小格距产生不可分配的缓冲。
  自动尺寸超过总预算时给出所需尺寸和格距，过小尺寸与远坐标组合无法可靠区分 float 格子时也报告精度错误。
- 节点按最近三角形距离烘焙，扣除 float 节点位置重建误差并向下舍入。
  GPU 行进使用最近节点距离减去到该节点的距离作为保守下界，乘 0.9 安全因子。
  三线性插值只用于距离切片，不驱动追踪。
- 每个距离格保存真正与闭格相交的所有三角形候选 CSR；没有固定数量截断。
  当前格候选在射线 entry/exit 区间内做精确双面复核，命中按最近距离和确定的三角形索引选择。
  下界接近零时沿 DDA 格边界推进，不用固定最小步长越过薄墙；边/角同距轴同时跨越。
- `Shaders/VXGISDFCommon.hlsli` 返回世界距离、位置、原始几何法线、正反面、重心坐标和三角形索引。
  `VXGISDFResolveSurface` 从命中恢复材质/实例 ID、UV、着色法线和 TBN。
  顶点属性采用 GBuffer 相同的逆转置变换，插值后调用共有的 `TangentBasis.hlsli`；不会重复翻转导入 UV。
- 零向量或非有限射线为 invalid；命中、正常 miss、invalid 与迭代耗尽有独立状态。
  辐亮度不参与命中判断，因此黑色表面仍终止射线。诊断中 invalid/exhausted 显示品红色。

Details 的 **Distance field** 显示尺寸、节点数、表面格数、引用数、CPU 估算与构建时间。
切片可选渲染世界 X/Y/Z 轴和世界坐标；这些轴与距离体/HLSL 一致（Y 为竖直方向）。
切片橙色表示接近表面、蓝色表示远离；命中位置按 bounds 映射到 RGB；法线用 `n*0.5+0.5`。
太阳可见性绿色表示通向太阳无遮挡，红色表示被三角形遮挡，使用当前太阳方向。
所有新视图在后处理之后替换背景，使用未抖动逆 VP；已有选择和编辑器 UI 随后绘制。

阶段 4 起由 `Resources/VXGIGPUScene.h` 统一管理每个在途帧的 DEFAULT buffers，
探针 compute 与 SDF/surface 诊断共享节点、CSR、三角形和中心位置；revision 不变时复用，
staging 在对应 fence 完成后释放，避免诊断另存一套相同场景数据。
静态首版几何变化同步完整重建。阶段 3 起材质身份变化只更新 GPU 三角形记录，
距离节点和候选 CSR 继续复用；太阳和材质采样参数变化不上传几何。
阶段 2 交付时 alpha cutout 按完整三角形处理；阶段 7.3 已加入候选覆盖复核。透明/骨骼动画仍在后续范围。
全分辨率诊断逐像素精确查候选，成本取决于当前视图中的三角形密度，不代表未来分帧探针更新的预算。

### 阶段 2 测试

CPU：`Tests/VXGISDFTests.cpp`，VS Developer 命令行同阶段 1 的 C++20 `/W4 /WX` 编译方法。
Debug 与 O2 通过 4 组测试，包括 700 条随机 BVH 对照、开放平面两侧、黑薄墙、内部/表面起点、
负/非均匀缩放、格面/边/角、64 个同格候选、远坐标与非二进制格子尺寸、revision/失败保留。
世界三角形新增属性后，阶段 1 的 6 组稀疏分配测试仍通过。

GPU：从仓库根目录运行 `Tests/RunVXGISDFGPUTests.ps1`。
headless D3D12 compute 上传与诊断相同的数据，调用同一个 `VXGISDFTrace` 和表面恢复函数，
readback 后与独立 CPU BVH 对照；报告保存到 `tmp/VXGISDFGPU-report.json`。
测试优先硬件；若只能使用 WARP，报告会明确记录适配器类型。

### 阶段 2 验收记录

- 最终 Debug / Release 主工程与 UI 架构检查通过，保留既有编译警告。
  Debug 单个翻译单元达到 COFF 节数限制，因此仅给 `PBR_Render.cpp` 添加标准 `/bigobj`。
- 真硬件 NVIDIA GeForce RTX 5070 Laptop GPU，启用 D3D12 debug layer 和 GPU-based validation：
  7 组 925 条射线通过，0 warning/error。命中/材质/实例/三角形/背面身份一致；最大距离误差
  `7.15e-7`，最大重心误差 `3.58e-7`，最大 UV/TBN 属性误差 `2.98e-7`。
  这是测试 harness 的验证结果，不是对整个主程序所有 D3D12 调用的无错误声明。
- 最终当前场景 SDF cell size 1.0：83 × 8 × 83，共 55,112 节点；
  13,555 表面格，165,487 三角形引用。一次最终 Release 启动构建约 129.62 ms，SDF CPU 保留估算 39.05 MiB。
  世界缓存新增 UV/TBN 后约 36.88 MiB（阶段 1 的 19.47 MiB 是扩展前记录）；这两个 CPU 缓存各自保留世界三角形，
  不包含临时构建峰值、模型资产或 GPU 分配。
- 已实际检查距离切片、命中位置、几何法线、太阳可见性；相机拉远后 world/SDF revision 不变。
  已检查 100%/150%、窄 Details、切片轴选择与单击数值编辑、F10 的共享组件。
- 关闭程序现在等待全部帧槽 fence，保证在途诊断缓冲、DLSS 和 ImGui 资源可安全释放。

## 阶段 3：命中材质与 `Lo0`

`Shaders/VXGISurfaceCommon.hlsli` 提供 `VXGISampleSurfaceMaterial` 和 `VXGIEvaluateSurfaceSource`。
它们从 SDF 的真实三角形命中恢复 UV/TBN、材质和实例身份，查询现有全局材质池及 bindless 纹理。
查询覆盖完整 GI 几何，不依赖 GBuffer 或当前可见实例。
当前按命中点即时计算源光，后续由探针保存采集结果；没有给每个三角形存一个均匀光照值，也没有表面纹理图集。

- 材质记录与现有 `MaterialData` 同为 48 字节。albedo/emissive 沿用 GBuffer 的 `pow(abs(rgb), 2.2)`
  解码及材质因子；ORM 沿用原通道语义；法线图沿用 Y 翻转和共有 TBN 构建。
  着色法线根据命中正反面翻转，原始几何法线和 `backFace` 仍保留真实方向。
- 所有纹理均使用显式 `SampleLevel` 和 linear wrap sampler。首版 footprint 约定为固定 mip，
  默认 0，可在 **World surface / Material mip** 修改，支持小数 LOD；不依赖屏幕导数或相机距离。
  这尚未实现基于射线锥或纹理尺度的自适应 footprint，远距离诊断可能存在纹理混叠。
- `GISourceCommon.hlsli` 定义共享的漫反射源公式：

  ```text
  F0 = lerp(0.04, albedo, metallic)
  rho_d = (1 - F0) * (1 - metallic) * albedo
  directDiffuse = rho_d / pi * sunRadiance * max(NdotL, 0) * visibility
  Lo0 = directDiffuse + cappedEmissive
  ```

  此处不使用相机视向 Fresnel、环境 SH、镜面、AO 或 GI 反馈。GI 源自发光 RGB 峰值上限为 2，
  保持 RGB 比例；材质查询和原场景可见自发光保留未截断值。
  unlit 保持既有约定：终止射线、可见 albedo，但 GI 源为零。
- Deferred 的 `BounceSource` 已调用同一公式；原场景颜色的直接/环境 BRDF 和 SSGI 接收端 `E/pi` 合成保持既有路径。
  共享的是源材质响应：屏幕源仍从 GBuffer/CSM 取得采样材质和阴影，世界源使用固定 mip/SDF 遮挡，
  两条分支的 footprint 与阴影采样方式尚未统一。
- 从真实 `hit.position` 朝太阳发射 SDF 射线，以 `ray_min_distance` 排除起点自命中，
  不把表面点或探针沿法线移动。`max_ray_distance` 是世界阴影查询距离，默认 1000 世界单位。
  当前是二值硬遮挡，没有采样太阳角半径。invalid/exhausted 不能当作可见，调试中显示品红色。
- 阶段 3 交付时只恢复 alpha 值，cutout 仍完整遮挡；阶段 7.3 的追踪现按真实命中 UV 复核覆盖。透明实例仍排除。

`Resources/VXGISurface.h` 管理独立的 geometry/material/lighting/sampling revisions，阶段 7.3 增加 coverage revision。
相机不是状态输入；太阳颜色/方向改变 lighting，射线区间改变 sampling，材质改变 material。
固定材质 mip 同时改变 sampling 和 coverage；albedo descriptor、baseColor alpha、resolved material ID 或 cutout 标记变化也使 coverage 失效。
世界几何与 SDF 各有独立 geometry revision，CPU 和 GPU 距离/候选数据在材质编辑时复用。
现有已注册纹理与材质因子变化会同步刷新屏幕/世界共用材质池；首版少量材质编辑等待全部帧槽后同步上传。
纹理像素内容仍按静态资产处理，替换未注册资产需要走资源注册/加载流程。

### 阶段 3 验证

CPU：`Tests/VXGISurfaceTests.cpp`，C++20 `/W4 /WX`，Debug/O2 通过 4 组测试。
测试不仅检查 revision，还检查距离节点、CSR、三角形及格子容器地址保持不变；
覆盖材质因子/纹理/unlit/发光/覆盖/cutout、太阳、显式 LOD、探针尺寸、实例重排、TRS 和显式几何失效。
原 World 6 组、SDF 4 组与 700 条随机参考射线的优化回归全部通过。

GPU：运行 `Tests/RunVXGISurfaceGPUTests.ps1`，报告为 `tmp/VXGISurfaceGPU-report.json`。
RTX 5070 Laptop 硬件、D3D12 debug layer 与 GPU-based validation 下，25 个真实 bindless 查询通过，0 warning/error。
覆盖纯色和空间 UV 纹理、真实 mip 链的 0/0.5/1/2/越界 LOD、ORM、法线图/TBN/背面、RGB 自发光截断、unlit、
材质覆盖、同表面不同观察射线、光源变化、黑薄墙阴影、黑色命中与 miss。
独立 CPU 材质参考最大误差 `1.19e-7`，直接光/自发光/`Lo0` 最大误差 `8.94e-8`。
共同源公式的 screen/world 参数结果误差为零；此项是 compute 数值检查，不是 Deferred 全帧像素一致性声明。
测试 harness 的 0 warning/error 也不代表整个主程序所有 D3D12 调用都已被完整验证。

最终 Debug/Release 主工程和 UI 架构检查通过，保留既有编译警告。
已在主程序实际检查五个新增视图与相机拉远后的 revision 稳定；检查 100%/150% UI 缩放及窄 Details，
包括完整下拉选项、独立折叠区、说明换行与单击数值编辑。阶段 3 的历史实测中，mip 从 0 改为 1 只增加 sampling revision，
world/SDF 几何和材质/光照 revisions 不变；当前 mip 还会改变 coverage revision，见阶段 7.3。

## 阶段 4：固定探针分类与 RGB SH 采光

`Resources/VXGIWorld.h` 继续负责稀疏格子、中心位置和候选三角形。
它保留的 CPU `Probe` 是分配模板，零 SH/`Unclassified` 不代表运行中的 GPU 探针状态。
实际分类、SH 和计数存在 `Passes/VXGIProbePass.h` 的两个 GPU 历史缓冲中；
`Resources/VXGIProbeState.h` 只管理提交计划、失效 epoch 和已完成 readback 的汇总，不保留整份 CPU 光照副本。

### 分类与固定起点

- 默认每个首次更新的探针发射 64 条分类射线；`classify_rays` 和 `rays_per_update` 均支持 8—256 的偶数。
  分类使用固定的 hemisphere Fibonacci 方向及其相反方向，按 probe index 选择稳定旋转；相机和帧调度不是输入。
- 从真实格子中心以 `tMin=0` 追踪，统计原始几何法线的真实 `backFace` 命中。
  `backfaceRatio = backfaceHits / classificationRays`，分母包括 miss；比例严格大于 `backface_threshold`
  时禁用，默认阈值 0.25。这是基于表面朝向的启发式分类，不要求开放模型具有距离场符号。
- 真实最近命中距离 `<= surface.ray_min_distance` 时禁用为 `UnsafeOrigin`。
  靠近表面但仍大于该偏置的探针保留；全 miss 时距离记为 `surface.max_ray_distance`，不会因此判为贴面。
- 分类出现 invalid/exhausted 时禁用。原因优先级为追踪失败、起点不安全、背面比例超限。
  状态为 0 `Unclassified`、1 `Valid`、2 `Disabled`；reason 为 0 无、1 背面、2 起点不安全、
  3 分类追踪失败、4 采光失败。任何分类或采光过程都不改变探针中心。
- 有效探针采光以 `surface.ray_min_distance` 为 `tMin`，从同一中心发射。
  真实命中 UV 调用阶段 3 的 `VXGIEvaluateSurfaceSource` 取得 `Lo0`；正常 miss 贡献零。
  黑色或 unlit 表面仍终止射线，unlit 的 GI 源为零。invalid/exhausted 或无效源查询使探针禁用为 reason 4 并清 SH。
  阶段 4 交付时 cutout 仍完整遮挡；阶段 7.3 已改为 alpha 覆盖复核。透明实例仍排除；没有环境、镜面、AO 或探针反馈项。

### SH 契约与历史

`Shaders/VXGIProbeCommon.hlsli` 统一定义世界空间基底，数组顺序为 `[constant, X, Y, Z]`：

```text
Y(d) = [0.2820947918, 0.4886025119*dx, 0.4886025119*dy, 0.4886025119*dz]
ck = (4*pi/N) * sum_i Lo0(di) * Yk(di)
E(n)/pi = Y00*c0 + (2/3)*0.4886025119*(nx*c1 + ny*c2 + nz*c3)
```

采样覆盖均匀球面，投影权重为 `4*pi/N`。每对射线使用严格相反的方向，
通过 radiance 的和/差分别累加 constant/L1，使常量输入的 L1 系数严格为零。
采光方向按 probe index 与累计成功 update count 做 hash quaternion 旋转；分类方向固定，采光每次更新旋转。
同一探针得到相同更新次数时方向相同，不随相机、全局 frame index 或更新预算改变。

保存的是未卷积的入射辐亮度 SH，系数允许负值；余弦卷积及逐 RGB 非负钳制只在消费 `E/pi` 时进行。
每次成功采光先计算完整新估计，再以 `1/min(updateCount+1,max_history_updates)` 混合。
默认 history window 为 32，达到上限后继续以固定权重更新，并非保留最近 32 份独立记录。
`updateCount` 和 `sampleCount` 是本次 reset 后累计成功更新/采光射线数，达到 uint 上限时饱和；
`lastUpdateFrame` 仅记录成功采光。年龄按已提交探针帧减去最后成功采光帧计算。

GPU record 为 96 字节：4 个 `float4` SH 占 64 字节（RGB 系数、w 恒为零），
其余为 state、updateCount、sampleCount、lastUpdateFrame、backfaceRatio、minHitDistance、classificationRays、reason。
仅两份历史的内存为 `192 * probeCount` 字节，另有每帧 snapshot/timestamp 和共享场景资源。
compute constants 为 128 字节/32 DWORD，包含独立 SDF grid、太阳、显式材质 mip、射线区间、批次及分类参数。
绑定为 `b0`、`t0..t6`（节点、offsets、refs、208B 三角形、48B 材质、12B 中心、上一份记录）、`u0` 下一份记录、`s0` linear wrap；
9 个 root parameters 共 48 DWORD，使用直接索引纹理堆，先绑定 engine heap 再绑定 root signature。

### 分批更新、失效与读回

默认每帧计划最多 256 个探针，沿 compact 索引推进，到末尾后从头开始。
未分类探针先分类，再对有效探针采光；已禁用探针在可靠分类未失效时保持禁用。
RDG 每次先用 `CopyResource` 将 prev 完整复制到 next，保留所有未选中的记录，再按需对全体执行 `CSReset`，
插入 UAV barrier，最后用 production `CSUpdateParallel` 更新选中批次。
每个选中探针对应一个 64 线程 group，分类与采光都由组内协作：lane 处理 `pairIndex=lane+64*m` 的相反射线对，
对背面数、最近命中、追踪状态和 4 RGB SH 做 shared tree reduction，lane 0 决定分类并写历史与计数。
`CSReset` 按 64 线程批量清记录；原单线程负责一个探针的 `CSUpdate` 保留为数值参考。
production dispatch 为 `Dispatch(selectedProbeCount,1,1)`，超过每轴 65,535 groups 时切成独立批段，
更新 root constants 的 start/count；分配序号、rotation、投影、历史与 reset 语义保持一致。
同帧 reset 后的采光读取已清空的 next 记录，不能重新混入 prev 的旧历史。
完成后 history 回到 pixel/non-pixel SRV 状态，供诊断和下一次提交使用。

- `resetMode=1` 清整条记录并重新分类：初次运行、世界/探针分配/SDF 几何或 coverage 变化、分类射线数或阈值变化、射线区间变化。
- `resetMode=2` 清 SH、计数和年龄，保留可靠的 valid/backface/unsafe-origin 分类：不影响覆盖的材质 RGB/自发光/unlit、太阳、采光射线数或 history window 变化。
  reason 3/4 的失败分类回到 unclassified，以便在同帧选中批次中重试。
- 单改 `probes_per_frame` 只改预算；相机变化不失效历史。

CPU `Prepare` 不消耗批次或 reset；只有命令执行后 frame fence `Signal` 成功，
`OnSubmitted` 才推进 cursor、frame index 和读写历史索引。
source/history epoch 只在成功提交 reset 时增加，普通采光帧不改变 epoch。
共享场景 staging、每帧读回和 timestamp 等各自帧槽 fence；历史容量变化时等全部帧槽完成后重分配。
每 32 个探针提交帧及 reset 帧异步复制统计 snapshot，带实际 frame/epoch；读回拒绝旧 epoch 或乱序结果。
Details 显示的是最后一份完成的 GPU 汇总，而非 CPU 模板状态或每帧同步读回。

**Probe cache** 显示 valid/disabled/unclassified、各禁用原因、累计更新/射线数、更新年龄、epoch、GPU 内存与时间，
并通过共享数值组件调整预算、两个射线数、history window 和背面阈值。
GPU timestamp 从完整历史 copy 前到选中批次更新后，包含历史 copy、之间的状态转换及可选 reset/分类/采光；
不包含共享场景上传、后续统计 snapshot、诊断绘制或主场景渲染。

视图 13 用十字显示绿色 valid、红色 disabled、灰色 unclassified；
14 在有成功采光的有效探针处绘制八面体，各世界轴顶点重建 `E/pi`，显示时压缩 HDR 并编码 gamma；
15 用蓝到橙显示最后成功更新后的年龄，未采光探针为灰色。
八面体颜色是四系数重建的诊断，不是最终表面 GI。
三种视图均在固定中心用未抖动 VP 绘制，忽略深度，在后处理之后、选择与 UI 之前呈现。

### 阶段 4 独立测试

CPU：`Tests/VXGIProbeTests.cpp`，C++20 `/W4 /WX`，Debug/O2 通过 5 组。
覆盖事务提交/游标和预算、geometry/source revisions 与 epoch、两种 reset、完成帧读回及旧结果拒绝、
常量/有符号 X/Y/Z SH、`E/pi` 的 `2/3` 因子和消费端钳制。

后台场景集成：运行 `Tests/RunVXGIProbeSceneTests.ps1`，Debug 加 `-DebugBuild`；
`Tests/VXGIProbeSceneTests.cpp` 在 Debug/O2、C++20 `/W4 /WX` 下通过 3 组。
真实 World/SDF fixture 使用 voxel 0.5、SDF cell 0.25，实例 engine X 增加 0.17 后返回原变换；
探针数 4→5→4，geometry/allocation/SDF revision 与 epoch 1→2→3 递增，移动与恢复均 reset1/start0。
恢复后的中心、cells、节点与 CSR 精确一致，参考射线 hit→miss→原 hit，恢复同数量后仍拒绝旧 epoch 的读回。
这组恢复 fixture 直接修改场景输入；另一个测试独立调用真实 `EditorTransform`/`EditorHistory`，
对 `ModelInstance` 验证单字段事务只有一条记录、stable ID 的 Undo/Redo 恢复变换。
这些是无资产、无设备、无窗口的后台 CPU 检查，不是实际编辑器按钮操作或 GPU 生命周期测试。

GPU：运行 `Tests/RunVXGIProbeGPUTests.ps1`，Debug 加 `-DebugBuild`；报告为 `tmp/VXGIProbeGPU-report.json`，
分别保留 Debug/O2 结果于 `tmp/VXGIProbeGPU-Debug-report.json` / `tmp/VXGIProbeGPU-O2-report.json`。
硬件 RTX 5070 Laptop、D3D12 debug layer 与 GPU-based validation 下，Debug/O2 均通过 13 组，0 warning/error。
测试直接执行 production `CSReset`/`CSUpdateParallel` 和同一份 common helper，
并用保留的 serial `CSUpdate` 对同一输入核对分类身份、计数、最近距离与 SH 浮点归约误差；
覆盖常量与有符号三轴、封闭盒/薄墙/贴面及安全间距分类、真实 hit-UV emission 的 CPU 三角形参考投影、
完整 copy 与分批保留、frame 无关方向、循环批次、历史窗口上限和累计计数、
光照/几何 reset、真实采光失败后同帧重试、真实分类 invalid 处理。
生产测试覆盖 8/64/128/256 rays；8 和 256 的分类/采光边界验证仅少数 lane 工作及每 lane 多 pair 的分工。
40 条 parallel/reference 记录的 classification/counters 完全一致、最近距离差为零，最大 SH 归约差 `1.4305e-6`。
最新报告最大数值 SH/`E/pi` 误差 `1.5479e-4`，真实表面 SH 投影误差 `9.3579e-6`，历史混合误差 `2.3842e-7`。
这些是有限球面投影和 fixture 的数值误差，不是主场景逐像素光照误差；0 warning/error 仅对应测试 harness。
DXC SM6.6 编译通过，并从 DXIL 核对 96B record、128B constants 及各资源 stride。

第 13 组为 production wrapper 集成，直接使用 `RenderDevice`、`VXGIGPUScene`、
`VXGIProbePass` 与 `RDGBuilder`；最终 Debug/O2 均以 `/utf-8 /W4 /WX` 通过。
fixture 使用不可见窗口且不调用 Present 或操作窗口，3 个帧槽完成 32 次提交；
真实 World/SDF 的分配数 32→9→32，实际 GPU 中心与 CPU 分配一致，历史缓冲随数量变化重分配。
978 条 GPU 记录与独立 CPU 三角形/源光参考核对，最大 source/history SH 误差
Debug `5.51343e-7`、O2 `5.36442e-7`；
分批未选中记录、ping-pong、reset、最后部分批次、旧 epoch snapshot 丢弃及完成汇总匹配均通过。
两个测试设备的 D3D12 debug/GPU validation 均为 0 warning/error。
本次未实际观测到扩容时仍有未完成的 GPU frame（`resize_observed_in_flight=false`），
因此覆盖正常 fence/资源复用与容量变化，不宣称已完成在途扩容压力测试。

### 阶段 4 调度成本与主程序验收

旧 serial kernel 默认 256 probes/frame 仅产生 4 个 64 线程 groups，每个线程串行完成整份分类与 64 条采光射线，
每个采光命中还可能发射太阳遮挡射线。Release 实际批次 timestamp 从地面约 2.8 ms、后续约 17 ms，
到密集车身约 230—240 ms（一次记录为 229.94 ms）；低 group 数和密集格候选使首版无法维持实时预算。
因此 production 改为每 probe 一个 group，并把分类和采光一起并行，默认选中 256 个探针时 dispatch 256 groups。
最终 parallel Release 在相同 256 probes/frame、64 capture rays/update 设置下，已观测的 batch timestamp 为约 1.03—4.61 ms。
这些是几个独立运行采样，不是正式基准的最小值/最大值、平均值或分位数；首次分类与稳定采光也未分别统计。
不据此推断性能倍数，也不从数值一致性测试推断性能。
密集 CSR 与近表面 DDA 仍可能产生较大成本，需区分首次分类与稳定采光，并测候选数/追踪步数的长尾。
Debug shader 编译启用 `-Zi/-Od`，实际耗时可到数百 ms；生产性能以 Release 场景计时为准。

- 最终 Debug/Release 主工程及随构建运行的 UI 架构检查通过，保留既有编译警告。独立 GPU harness 的 0 warning/error
  不扩展为整个主程序所有 D3D12 调用均已完整验证的声明。
- 阶段 4 alpha 接入前，汽车与地面场景的 53,306 个固定探针完成分类后，valid 27,181、disabled 26,125；
  禁用原因是 backface 26,123、unsafe origin 2，classification/sample trace failure 均为零。
  valid 约占已分配探针的 51%，这是严格表面格子的有效探针比例，尚不等于表面查询支持率。
  第五步的小型 fixture 已检查墙两侧、拐角和稀疏边界；后来的 7.2 提供 alpha 接入前的质心抽样，不是当前画面支持率。
- 最终 parallel Release 的 Probe cache 显示约 24.401596 MiB GPU 内存，包含双历史、各帧 snapshot 与 timestamp，
  不包含 shared scene buffers、纹理或其它场景资产。batch 时间范围采用上面的有限采样。
- alpha 接入前的 material mip 0→1→0 实际编辑使 source/history epoch 1→2→3，保留已有可靠分类并重置 SH/计数；当前 mip 编辑须重新分类并清邻边。
  编辑器 Location.X 从 0→1 后，world revision 1→2，epoch 到 4 并重新分类；探针总数仍为 53,306，
  新分类 valid 27,178、disabled 26,128。这个实际变换检查覆盖同数量的几何重建，不覆盖历史容量变化。
- 相机移动时 world/SDF/source epoch 不变的运行检查来自旧 serial kernel；相机不参与方向/源光计算的
  shared helper、frame 无关方向和新旧 kernel 数值一致性另外由 headless 测试覆盖。
  不将它描述为最终 parallel kernel 的新一轮相机运行计时。
- Debug 100% 在并行改造前实际检查了状态视图 13 与 Probe cache；最终 parallel Release 150%
  实际检查 SH 视图 14、年龄视图 15、宽约 258 的窄 Details 及 F10。
  最终 parallel 100% 启动时只检查了默认分配视图与部分缓存面板，随后按用户要求停止 UI 操作；
  未声称该版本在 100% 下完整重查所有视图或窄布局。
- 数值编辑与材质/模型失效已有上述实际记录；实际 UI undo 尚未检查。
  数量变化/返回原变换及独立的真实 EditorHistory Undo/Redo 由上面的 headless CPU 测试覆盖，
  不将两种 fixture 合称为实际 UI undo 或完整 GPU 资源扩容验证。
  `VXGIGPUScene` 抽取后，旧 SDF/surface 全屏诊断视图尚未重新进行实际像素检查；
  共享追踪/源光路径由现有 headless shader 数值测试覆盖，这不能代替完整诊断画面的视觉验证。

## 阶段 5：邻探针可见性与表面查询

`Resources/VXGIProbeTopology.h` 从已排序的稀疏格子建立 16B lookup 和 24B 六邻居记录。
lookup 使用世界原点锚定的整数格坐标，按 X、Y、Z 字典序二分查找，不使用 SDF bounds origin。
邻居顺序固定为 `[-X,+X,-Y,+Y,-Z,+Z]`，缺失索引用 `UINT_MAX`；同一 World 且尺寸/数量不变时，
拓扑只按 allocation revision 失效；替换 World 或改变尺寸/数量也刷新。太阳、材质、采光参数和相机不重建距离场或邻接表。
拓扑的 link 数只表示相邻已分配格子，不表示它们无遮挡。

### 六邻居边与滤波诊断

`Shaders/VXGIProbeQueryCommon.hlsli` 和 `Shaders_For_VXGI_Probe_Filter.hlsl` 共用可见性、滤波和查询语义：

- 每条邻探针边按较小索引中心→较大索引中心追踪，使用闭区间 `[0,distance]`。
  命中任意三角形，包括起点或终点命中，均遮挡；invalid/exhausted 不能当作可见。
  不移探针、不忽略同一物体或三角形，也不创建跳过薄墙的起点偏置。
- 每探针保存一个 4B mask：bits 0—5 是六条可见边，bits 8—13 是对应追踪失败，bit 31 表示该行已计算。
  静态几何且覆盖不变时可复用已知行；source-only reset 只清滤波结果，保留边，geometry/allocation/SDF 或 coverage reset 清两者。
- 滤波从当前不可变 raw history 读取自身和六个 raw 邻居，自身权重 1，邻居权重默认 0.25、范围 `[0,1]`。
  邻居必须有已知、可见且无失败的边，状态 Valid、至少一次成功更新且四个 SH 都有限。
  合法黑色 SH 仍是有效记录；Disabled、未更新或非有限记录不能参与。
- 四个有符号 RGB SH 用相同归一化权重平均，w 为零。有效自身保留分类和更新 metadata；
  无效自身输出整条零记录，不能被邻居变成伪有效探针。此 pass 只生成一次局部滤波诊断，没有递归滤波或反馈。

实际表面查询读取 **raw SH**，不读取预混合的滤波 SH。
例如 P→A 可见、A↔B 可见、P→B 被墙挡住时，滤波后的 A 可以含有 B 的颜色，
但 P 的查询只能接收 A 自身的 raw 贡献。只检查 P→A 再借用 filtered[A] 会造成绕角串色。
六邻居滤波视图用于观察拓扑和局部平均，不改变真实查询的遮挡规则。

### 八角点表面查询与薄墙端点

`VXGIProbeQuerySurface` 对真实 SDF 命中构造八个中心格角点：
`base=floor(position/voxel_size-0.5)`，按 x/y/z 小数部分计算普通三线性权重。
不存在、Disabled、未更新、非有限或不可见的贡献者被剔除，其余权重重新归一化。
没有 26 邻域扩展、远探针回退或 filtered anchor 借用；全部缺失时返回零间接项和 `hasSupport=0`。

每个候选独立检查 probe→target 的反向线段，从精确探针中心以 `tMin=0` 追踪到 `distance+tolerance`。
第一命中必须是目标 **triangleIndex**，并同时满足命中距离与目标距离、命中位置与目标位置的误差不超过 tolerance。
遇到其它三角形立即遮挡，即使属于同一实例、完全黑色或在容差范围内；miss、invalid、exhausted 或无法确认端点均保守拒绝。
容差为 `max(1e-6, sdf.cell_size*1e-5, 8*FLT_EPS*max(abs(surface),abs(probe),distance))`，
只用于确认末端及延长 rayMax，不在起点制造漏光区间。
共享边若第一命中选中了另一条等距三角形，也会保守拒绝；这可能减少支持，不让其它三角形穿透。

候选必须严格位于目标可见几何侧：`dot(backFace ? -geometricNormal : geometricNormal, probe-position)>0`。
原始几何法线和 backFace 决定这一侧；法线图只用于 `E/pi` 的余弦 SH 重建。
查询不做无条件法线位移，不忽略整个目标三角形或物体，也不沿用太阳射线的起点 rayMin 来跳过薄遮挡。

48B 查询结果分别保存非负 `E/pi`、未钳制 `E/pi`、RGB 负值 bitmask、支持权重及计数。
`candidateCount` 是权重大于零且实际存在的格角点数；`acceptedCount` 还要求有效记录和独立可见性；
`supportWeight` 是接受者的原始三线性权重之和，`hasSupport` 与亮度无关。
`traceFailures` 记录无法可靠确认的贡献者，正常几何遮挡不算追踪失败。
已分配格子数、有效探针比例、候选数量和真实表面支持率是不同指标；黑色结果不能当作 missing。

### GPU 集成、失效与成本口径

`Passes/VXGIProbeFilterPass.h` 在 raw 更新后执行 RDG：可选 full `CSResetFilter`→UAV barrier→
选中批次 `CSVisibility`→边缓冲转为 SRV→`CSFilter`，完成后回到 pixel/non-pixel SRV 状态。
`CSVisibility` 每个选中探针一个 64 线程 group，前六个 lane 检查边，全部 lane 参加两个 group barrier；
已知 mask 整组直接复用。`CSFilter` 和 full reset 均每线程一条记录、每组 64 线程。
超过 65,535 个 visibility groups 时分段设置 start/count；200 万探针上限下 reset/filter 不超过该限制。
未选中的滤波记录保持上次结果；表面查询始终使用当前 raw history，不依赖这些分批诊断结果。

compute constants 保持 80B/20 DWORD，依次保存 SDF origin/cell size、cell dimensions/probe count、
bounds maximum/triangle count、voxel size/start/count/reset mode、neighbor weight/material count/material mip/padding。
绑定为 `b0`，`t0..t9`：nodes、offsets、refs、208B triangles、12B centers、16B lookup、24B neighbors、96B raw、4B masks、48B materials；
`u0` masks、`u1` filtered，`s0` linear wrap。阶段 7.3 的 13 个 root parameters 共 44 DWORD，并启用直接索引纹理 heap；engine heap 在 root signature 之前绑定。
表面查询诊断重用已有 `t4` material SRV，仍为 192B constants 和八个 root SRV，恰好 64 DWORD。阶段 5 交付时的 filter root 为 42 DWORD。

reset1 清边和滤波结果；reset2 保留边、清滤波结果。filter 另比较 pending coverage revision，覆盖变化必清旧 known mask。
邻居权重变化只触发 filtered reset，不清 raw history 或 source epoch。
资源容量变化等待全部帧槽 fence，并明确检查 wait 后 completed value；只有成功 Signal 后提交滤波状态。
共享场景每帧还核对 World/SDF/Topology 对象身份，避免同 revision/同 count 的新对象复用旧 GPU payload；
revision 和身份变化只上传相应缓冲，不额外重烘 SDF。staging 等所属帧槽 fence 完成才释放。

新增 filter DEFAULT payload 为 `100*probeCount` 字节（96B filtered＋4B mask），另有每帧 16B timestamp readback。
三帧拓扑 DEFAULT payload 为 `3*40*probeCount` 字节（lookup＋neighbors）。
按历史场景 53,306 个探针估算，两项分别约 5.083656 和 6.100388 MiB，合计约 11.184044 MiB；
不包含上传 staging（最多另外约 6.100388 MiB）、资源对齐、纹理或其它共享场景数据。
这是字段 payload 计算，不是第五步实际显存测量；阶段 4 的 24.401596 MiB 记录发生在增加这些资源之前。

filter timestamp 从可选 reset/visibility 之前到 filter dispatch 之后，包含中间 UAV/SRV 状态转换；
不包含 raw 源采光、共享场景上传、最终回到 SRV 的转换或诊断绘制。
新边首次计算和已知边复用的实际成本、当前汽车场景的表面支持率均尚未测量。
阶段 4 的 batch 时间不包含新增第五步成本，不能据此宣称整套查询实时预算已达标。

### 阶段 5 后台验收

CPU：`Tests/RunVXGIProbeQueryTests.ps1`，Debug 加 `-DebugBuild`，C++20 `/W4 /WX`，Debug/O2 均通过 3 组。
覆盖世界原点锚定的负坐标、lookup/六邻居/八角点权重、缺失及范围保护、allocation-only 复用，
常量与有符号 SH、先插值后钳制的正负贡献抵消、滤波 metadata、无效自身整条清零、合法黑色支持、中心闭区间端点遮挡，
薄墙两侧颜色隔离，以及 P→A/ A↔B 可见、P→B 被挡的三点反例。
三点测试同时检查 raw 查询不借用 B，诊断 filtered[A] 可含 B，并证明不安全的 filtered anchor 查询会串色。
测试直接使用真实 World/SDF 的无资产 CPU fixture，参考遮挡用独立三角形实现；未操作窗口或编辑器 UI。

DXC SM6.6 / HV2021：三个 production CS 及 SDF/Probe 诊断 VS/PS 均以 Debug `-Od -Zi` 和 `-O3`、`-WX` 编译通过。
DXIL 核对 80B constants、96B record、16B lookup、24B neighbors 和 4B mask；Debug/Release 主工程及 UI 架构检查通过。

GPU：运行 `Tests/RunVXGIProbeQueryGPUTests.ps1`，Debug 加 `-DebugBuild`。
最终 Debug/O2 均以 `/utf-8 /W4 /WX` 通过 10 组，使用 RTX 5070 Laptop、D3D12 debug layer 与 GPU-based validation。
直接执行 production `CSResetFilter` / `CSVisibility` / `CSFilter`，查询调用同一 `VXGIProbeQuerySurface`，
并对照独立 CPU 三角形可见性、滤波和查询参考。
881 条 filter records 的 signed SH 最大误差为零；19 个表面查询的 `E/pi`/支持权重最大误差 `1.1920929e-7`。
覆盖薄墙两色、三点可见性反例、有限墙拐角、负坐标/格边界/稀疏归一化且无回退、合法黑色支持、
disabled/unclassified/未更新/非有限数据剔除、mapped normal 与先插值后消费端钳制、清历史后无支持与恢复。
最终报告为 [VXGIProbeQueryGPU-report.json](<D:/项目/LearnDirectX/tmp/VXGIProbeQueryGPU-report.json>)（最新 O2），
独立配置保留为 [Debug 报告](<D:/项目/LearnDirectX/tmp/VXGIProbeQueryGPU-Debug-report.json>) 和
[O2 报告](<D:/项目/LearnDirectX/tmp/VXGIProbeQueryGPU-O2-report.json>)。

其中一组直接使用 `RenderDevice`、`VXGIGPUScene`、`VXGIProbeTopology`、`VXGIProbeFilterPass` 与 `RDGBuilder`：
三个帧槽完成 10 次提交，真实 World/SDF 的探针数量 72→25→72，容量改变和恢复后的资源、几何边及滤波结果正确。
另将短墙 height 0.2→0.8→0.2，lookup/centers/count 不变但无遮挡边→遮挡边→无遮挡边，
验证同容量几何变化不会复用旧 known mask；neighbor weight 变化和 SH/source reset 则保留几何边、清滤波结果。
wrapper 的 raw 输入是已知 signed SH fixture，用于独立核对第五步；没有重复第四步的真实材质采光流程。
两套实际诊断 renderer 的 root/PSO 初始化通过，包括 64DWORD 表面诊断 root。
两台测试设备各自完整 InfoQueue 的 warning/error 均为零；该结论仅针对 harness，不扩展为全主程序无 D3D12 错误声明。
窗口始终不可见，Present 为零，没有 UI 工具或交互。
`resize_observed_in_flight=false`，覆盖正常帧槽复用和容量变化，不宣称已完成 outstanding queue 扩容压力测试。

第五步实现与后台验收已完成；新增控件和视图 16—20 尚未进行实际像素/UI 检查。

视图 16 使用 filtered SH 绘制八面体；17 绘制邻接边，绿色可见、红色遮挡、灰色未知、品红表示追踪失败。
18 显示 raw 可见贡献者重建的表面 `E/pi`，19 显示可见支持权重，20 显示缺失/负值标记；
无支持为红色，无法可靠追踪为品红色，负值重建也单独标记。它们是诊断输出，尚未加入场景 GI 合成。

当前仍只有严格表面格子中的一个固定探针，四系数 SH 截断与稀疏边界可产生缺失或负值；
阶段 5 交付时 cutout 完整遮挡；当前使用阶段 7.3 的覆盖追踪。正常 miss 为零局部辐亮度，没有环境、镜面、AO 或反馈。
以上是第五步交付时的状态；第六步现已消费 raw 查询，见下文。

## 阶段 6：屏幕与 SDF 混合漫反射 GI

以下保留阶段 6 的历史接口与算法记录；当前查看入口见前文 Debug → Pass。
当时的 `vxgi.hybrid.mode` 严格接受整数 0—2，默认 2。
SSGI 本身需要 deferred 和 `ssgi.enabled`，世界分支另需 `vxgi.enabled`；mode 0 可独立运行。
当时 Details 的 World surface / SSGI source 复用 `EditorUICenter::ChoiceProperty` 切换同一设置，现已移除。

| mode | 屏幕有效命中 | 屏幕射线未接受几何命中后的世界分支 |
| --- | --- | --- |
| 0 Screen only | 原 BounceSource `Lo0` | 零，保留原路径及 0.02 normal bias |
| 1 Screen + world | BounceSource `Lo0` | 真正 SDF 首次命中的 `Lo0` |
| 2 Screen + probes | `Lo0 + rho_d * Iprobe`，附加项需真实表面确认 | 真正 SDF 首次命中的 `Lo1` |

世界数据不完整时 effective mode 降为 0；只有 raw/topology 缺失时 mode 2 降为 1。
缺少可见探针支持时按表面保留 `Lo0`，不会改成穿透或远处光源。
原屏幕状态明确区分 Hit、Miss、OutOfScreen、RadiusLimit、IterationLimit 和 Invalid，并保存 hit pixel、位置、距离与置信度。
置信度代表几何接受，和 RGB、AO、平均权重无关。有效黑色/未点亮/unlit 命中始终终止该射线。
屏幕命中确认失败只剔除附加间接项，仍保留屏幕 `Lo0`；不会回退去找该表面后方的世界光源。

### 射线与表面身份

混合模式每个半分辨率接收像素，用其 jittered inverse VP 重建 GBuffer 世界点及对应 camera-near 点，
真实 SDF 从 near 点沿同一像素射线追踪穿过场景 bounds，只检查首次命中。
将真实命中用当前 view/proj 重投影，要求 clipW 为正、设备深度有限且在 0—1 内、
与 GBuffer D32 深度相差至多 2 ULP，UV 误差每轴不超过 `max(8*FLT_EPSILON, 0.01/sceneSize)`。
确认后使用真正命中位置作为屏幕与世界共用起点，没有 normal offset；同一余弦采样方向通过 inverse view 转为世界方向。
接收表面的 ray-facing 几何法线只决定该方向是否在几何正半球，贴图法线仍决定余弦采样。
确认失败时，屏幕仍从未偏移的 GBuffer 点追踪，但禁用该像素的世界回退。

屏幕 mode 2 命中同样沿 hit pixel-center 的 camera ray 做首次命中的投影深度确认，
得到真实 triangleIndex/barycentrics/geometric normal 后再调用第五步的 raw 八角点可见性查询。
没有伪造三角形 ID、从 normal map 推断几何法线、忽略目标整片三角形或挪动探针。
真实 SDF 表面到各 probe 的可见性仍使用第五步 `max(1e-6, cellSize*1e-5, 8*FLT_EPSILON*maxCoordinate)` 严格端点条件。
camera 确认不跳过任何前挡、不忽略三角形；首次命中无法通过投影核对时，整次确认失败。
薄墙不会因 normal offset 被越过；落入同一 D32 深度量化区间、或当前 2 ULP 核对范围内的极近前后表面，
仅靠 GBuffer depth 无法证明 raster primitive ID 相同。
本实现使用真实首次 SDF 命中及其身份，不宣称从 GBuffer 恢复了真正 raster primitive ID。

这里区分两种误差：D32 逆投影世界点不是精确几何端点；独立 CPU float diagnostic 中，near 0.1/far 1000 的逆投影在 z=100 误差约 0.0074，
远大于只考虑世界 float roundoff 的约 0.000097 容差。因此 camera 确认验证实际采样的投影深度，
probe 可见性则验证已经由 SDF 产生的真实目标点，不能把两者的容差混用。

屏幕半径仍由 `ssgi.radius` 控制（当前 0.5）；世界回退和世界 source shadow 使用 `vxgi.surface.max_ray_distance`（当前 1000），
世界首次命中从 `ray_min_distance`（当前 0.0001）起步，独立于屏幕裁剪或迭代预算。
世界命中颜色为黑、unlit、材质无效或 source shadow 失败时仍终止；正常 miss 为零局部辐亮度。
世界几何/材质查询、无可见 probe support 和 trace failure 在 helper 结果中分别保留状态。

### 光照与提交契约

两条路径的源面响应使用同一 `GIComputeSourceDiffuseWeight(albedo, metallic) * albedo * Iprobe`，unlit 附加项为零。
`Iprobe` 已经是 `E/pi`，不再除 pi、不乘 AO、没有环境/镜面或 probe 反馈。
world `Lo0` 由真实 UV/固定 mip 材质及 SDF sun shadow 求得；screen `Lo0` 仍用实际 GBuffer 和 CSM/PCSS。
这是同一 diffuse/emission 公式与反弹数量契约，不是所有场景逐像素数值一致承诺：两条路径的 LOD、texture footprint、
软/硬阴影和 alpha 的 texture footprint 仍可能不同；世界使用固定显式 mip，raster 使用屏幕导数采样。
因此 alpha 边缘的 camera 确认仍可能失败；此时只保留屏幕 `Lo0`，也可能失去第二次反弹。

每条射线选择一个分支，对全部采样射线（包括 miss）平均，RGB 仍为 `E/pi`；raw alpha 仍为命中比例。
原重建、temporal、history clamp、两轮 A-trous 和 composite 复用，接收材质响应及 intensity 由原 composite 应用一次。
没有同时叠加一次完整 SSGI 和一次完整世界 GI，也没有用低分辨率预混合的 screen source 代替逐目标可见性。

trace b0 为 608B：原 384B 前缀（albedo descriptor 复用 offset 264 的 padding）、inverse view/VP 各 64B、96B hybrid 参数。
root 为 CBV b0 + 八个 SRV 共 18 DWORD，加 s0 linear wrap；t0..7 依次 nodes/offsets/refs/208B triangles/48B materials/
12B centers/16B lookup/96B raw。所有实际世界输入均作为 external buffer 注册到 RDG，初始/最终为 pixel|nonpixel SRV。
engine heap 先于 root 和所有 root arguments 绑定，世界追踪消费本帧 raw 记录，filter 仍只用于诊断。

`SSGIHybridState` 比较 effective mode、screen radius/quality、实际本帧上传的 sun dir/color/angular radius/material mip bias/material revision、
世界 geometry/source/SDF revision 和 mode 2 的 pending probe reset epoch。相机运动使用原 camera-cut policy。
world source/range 在 mode 1/2 使用；probe epoch 仅在 mode 2 使用。普通 probe 批次 cursor/update frame 不进入 source key。
未提交的 recording 不消费 pending key；只有成功 Signal 后提交 source key、翻转各 temporal history、推进 SSGI sampleFrame。

### 后台验收记录

CPU 状态测试 Debug/O2 `/W4 /WX` 各 3 组通过，覆盖首次提交、失败重试、Invalidate、各类源变化与普通 probe 批次保留历史。
production trace 与验证 shader 的 Debug/O3 SM6.6 `-WX` 编译通过，主工程 Debug/Release 和 UI 架构检查通过。
主工程 Debug/Release 分别保留 7/6 个编译或链接警告，均为 0 error；这些与下述 D3D12 验证层统计分别记录。

RTX 5070 Laptop GPU 的 Debug/O2 配置均开启 D3D12 debug layer 与 GPU-based validation，各通过 11 组：
27 条 helper 射线、5 个实际 production trace kernel 输出像素；Debug radiance/kernel 最大误差为 0/0，
O2 为 `7.4505806e-9`/0。
覆盖屏幕外彩色源 `Lo0/Lo1`、世界距离独立于屏幕 radius、无支持保留 `Lo0`、黑色/unlit/无效 source 几何终止、
screen/world 同一源面响应、无额外 pi、全部采样平均、接收几何半球剔除、错误首次表面禁用回退。
旋转/平移相机与抖动投影直接运行 production CS；near 0.1/far 1000 下 z=10/50/100 的 receiver 确认通过，
加入早于目标的真实 blocker 时确认失败，未跳过前挡。这里使用合成 D32 GBuffer，未运行真实 raster GBuffer 的画面质量测试。

同一次验收还运行真实 `RenderDevice + ResourceManager + PipelineManager + VXGIGPUScene + SSGIPass + RDGBuilder + SSGIHybridState`：
12 次提交、3 帧槽、2 次尺寸变化（16×16→20×12→16×16），核对三模式、缺失输入降级、source/probe epoch 清历史及普通批次累积。
读取实际 FP16 history，最大误差 Debug `0.000945329666`、O2 `0.000960946083`；同时核对 composite 对接收材质仅应用一次。
每次提交都 wait/readback 后再继续，未测试在途扩容，不宣称压力覆盖。
probe raw 是已知 SH fixture，独立验证混合消费路径，没有重复第四步的材质采光。
两个测试设备的 InfoQueue 均为 0 warning/error，fixture 窗口始终不可见，Present 为 0。

[Debug 报告](<D:/项目/LearnDirectX/tmp/VXGIHybridGPU-Debug-report.json>) 与
[O2 报告](<D:/项目/LearnDirectX/tmp/VXGIHybridGPU-O2-report.json>) 分别保留；
[最新报告](<D:/项目/LearnDirectX/tmp/VXGIHybridGPU-report.json>) 为本次 O2。
未启动实际可见主程序，实际 UI、主场景边缘亮度与性能预算尚未验收。后续进入阶段 7 的编辑失效核对、实测预算与材质扩展。

## 阶段 7.1：编辑失效核对

修复两个实际时序缺口：原先 `RefreshGlobalMaterials` 只在 VXGI 开启时执行，且本帧 SSGI source stamp 在刷新前计算。
现在共享材质池在 `Update()` 开头刷新，不依赖 VXGI 开关，完成后才计算本帧 screen source stamp。
普通帧发现材质记录相同时直接返回；材质编辑沿用等待全部槽 fence 后同步上传的已有机制。

新增 `SceneEditState`，按稳定 editorId 记录已加载模型身份、TRS、材质 override、transparent/cutout 标记。
注册、删除或这些源输入变化会清现有 temporal histories；直接写 TRS 即使没有设置 isDirty，也会补齐 raster 矩阵更新。
只改变实例顺序、相机剔除可见性或 LOD 不清历史。静态 mesh 内容沿用不可变资产契约，不增加运行时新资产加载 API。
snapshot 更新不是 GPU history 提交；失效标记持续到成功提交，因此失败 recording 后重试仍须重置。

`RunVXGIProbeSceneTests.ps1` 的 Debug/O2 `/W4 /WX` 各 5 组通过：真实 World/SDF 的 add/remove/empty/restore，
probe epoch 1→2→3→4→5；geometry 编辑清分类，source-only 编辑保留可靠分类并清 SH；
材质 override/factor/texture descriptor、太阳和固定材质 mip 更新 surface/source key，几何及节点复用。
同时核对三个 SSGI 模式、普通 probe 批次、失败重试、实际 ModelInstance/EditorHistory 的 TRS/Undo/Redo。
CPU source fixture 的材质记录模拟已完成上传，实际 GPU 上传验收如下；没有调用可见编辑器或主程序 Update。
主工程 Debug/Release 构建通过，未改变编辑器控件。

`RunVXGIEditGPUTests.ps1` 的 Debug/O2 `/utf-8 /W4 /WX` 各 10 组通过，使用真实 ResourceManager/RenderDevice 加载 Plain.glb。
4 次 baseColor/emissive/unlit 编辑及还原上传、6 次无变化刷新、9 次实际 material buffer readback，1 个 48B 记录逐字节 0 mismatch。
刷新返回时 fence 已完成，Surface 在同次更新消费新 CPU 池，World/SDF 不重烘焙；无变化时 resource/revision/fence 都不变。
显式 override 选择同一个实际材质 ID，只验证身份失效；删除、空场景及恢复保留已加载资产的稳定材质池。
D3D12 debug layer/GBV InfoQueue 为 0 warning/error，hidden HWND、Present 0、draw 0、shader compilation 0。
这不是主程序 Update 或实际 UI 验收，不覆盖运行时新资产加载和 raster 实例缓冲扩容。
[Debug 报告](<D:/项目/LearnDirectX/tmp/VXGIEditGPU-Debug-report.json>)、[O2 报告](<D:/项目/LearnDirectX/tmp/VXGIEditGPU-O2-report.json>) 分别保留。

阶段 7.1 的上述验收属于 alpha 接入前：cutout 标记只验证 source identity 和 metadata 更新，遮挡仍使用完整三角形，mip 只清 SH。
阶段 7.3 已将覆盖复核接入 CSR 候选，并使 alpha/mip 编辑清分类及邻边；不能用 7.1 的旧失效规则解释当前行为。
透明和骨骼变形继续后续处理。

## 阶段 7.2：真实场景性能基线（alpha 接入前）

这份报告测量 alpha 接入前版本，不能代表阶段 7.3 的有效探针数、支持率或性能。
独立后台 fixture 加载当时 `Settings/Scene.json` 的汽车和地面，不启动主程序、不显示窗口、不 Present。
在 256 probes/frame、64 分类射线和 64 更新射线预算下完成一整轮分类，再收集稳定批次。
记录初建和不变帧 CPU 时间、production probe/filter GPU timestamp、真实 raw 分类、几何表面抽样支持率，以及公开 DEFAULT 资源的 committed allocation footprint。
性能配置关闭 debug layer/GBV，与开启验证层的数值验收分别运行；主场景 SSGI/整帧时间不在这份基线范围内。
本次重新加载的实际输入为 2 实例、129,386 三角形、53,306 已分配探针。
voxel size 为 0.5，实际 SDF cell size 为 1，节点网格 83×8×83、CSR 165,487 个 triangle refs；
固定材质 mip 0、max distance 1000、backface threshold 0.25，cutout 仍按完整三角形处理。
两轮各 209 批覆盖全部探针（末批 58），另提交 3 批排空 timestamp；前两轮 filter timestamp 都读齐，
只剩排空阶段的 3 个 filter timestamp 未纳入统计。原始批次的 start/count/reset 和时间都保留。

| production timestamp 范围 | 样本数 | 中位数 ms | p95 ms | 最大值 ms |
| --- | ---: | ---: | ---: | ---: |
| 首轮分类、采光及 history copy | 209 | 4.424096 | 14.779808 | 27.403648 |
| 首轮新邻边及 filter | 209 | 0.011232 | 1.313024 | 13.044736 |
| 已分类全场采光及 history copy | 209 | 3.957760 | 12.216992 | 19.609696 |
| 已知邻边复用及 filter | 209 | 0.008576 | 0.009728 | 6.808672 |

probe 时间不含异步 snapshot copy，filter 时间包括其 reset（只有首批）、选中边和 SH filter；均不是整个渲染帧时间。
每次提交都等待 fence 后才继续，不代表主程序多帧在途时的吞吐。
稳态 filter 最大值为末尾 partial batch 的实测 outlier，保留而未剔除；未锁频率/功耗且用户可能并发工作，不能直接归因于 kernel。
探针更新 start=26,112 的首轮/稳态批次分别为 27.404/19.610 ms，长尾值得继续定位；这份有限采样不证明具体耗时原因。
之前的 24 个 lexicographic prefix 稳态样本另存历史，预算判断只使用这里的全场第二轮。

CPU 初建 wall time 为 World 252.987 ms、SDF 181.010 ms、topology 20.575 ms；完整重建仍可能造成编辑停顿。
16 次不变帧 World/SDF/topology 合计中位数 0.0008 ms；不含材质刷新、scene snapshot 或其它主程序 CPU 工作。
完整 GPU raw 读取为 valid 27,181、disabled 26,125、unclassified 0，禁用原因 backface 26,123、unsafe origin 2，trace/sample failure 0。

几何支持抽样按每实例三角形面积分层选择 256 个质心，分别用正/背面几何半球调用真实 raw query，共 1,024 次。
汽车为 256 个独立质心，正面支持 126/256（49.22%），背面 11/256（4.30%）；地面正面 256/256、背面 0/256，
但地面只有两个独立质心重复采样，不能据此推断全面积覆盖。全部输出 finite、trace failure 0。
这是可重复的离散几何样本，既不是严格无偏面积估计，也不是 mapped-normal 或相机屏幕像素支持率。
汽车正面 49.22% 表示约半数选中质心能从可见、有效且已采光的 raw 探针取得间接项；其余位置保留 `Lo0`，缺少探针提供的第二次反弹。
这些是 alpha 接入前的离散样本；后续重测新覆盖，并区分稀疏候选缺失、禁用探针和可见性剔除，才能判断 alpha 洞或分配调整的收益。

对实际 25 个公开 DEFAULT 资源逐个调用 `GetResourceAllocationInfo`：payload 107,829,968B，
committed allocation footprint 108,789,760B（103.75 MiB，含资源对齐），不是物理 resident 显存测量。
范围为 3 槽各 7 个 shared scene buffers、2 份 raw histories、filter 和 edge；不含贴图、材质池、query heap、RDG pools 和主场景纹理。
私有 staging/snapshot/timestamp 另报 payload，不能与上述 allocation 混作同一总量。
其中三份 208B triangle buffers 占 77.0625 MiB，float SDF 节点仅占 0.75 MiB；当前保持 float32 距离节点，
压缩节点既不是主要内存收益，也尚未验证保守距离误差契约。

预算结论：保留现有 256/64 参数作基线，不宣称它满足 60 fps 或整个混合 GI 的预算。
阶段 7.3 已接入 alpha-cutout；后台回归完成后，下一步重测真实场景支持率并定位长尾，再评估加速结构或分帧工作量；不增加反馈反弹。
已知邻边缓存复用成本很低，但首轮新边和采光长尾仍需关注。

[最终报告](<D:/项目/LearnDirectX/tmp/VXGIStage7Benchmark-report.json>) 与
[29 项输入 SHA256](<D:/项目/LearnDirectX/tmp/VXGIStage7Benchmark-input-hashes.json>) 保留全部原始数据；
[复现 runner](<D:/项目/LearnDirectX/Tests/RunVXGIStage7Benchmark.ps1>) 只运行后台 fixture。
没有显示或操作窗口、Present 或实际主场景画面验收；未测主场景 SSGI、整帧成本或带反馈的白盒能量收敛。

## 阶段 7.3：alpha-cutout 全链路覆盖

### 候选接受与追踪

共享 `VXGISDFTrace` 的运行时重载在 CSR 候选精确 ray/triangle 命中后、最近命中选择前复核覆盖。
只对 `flags & 1` 的 cutout 三角形，用真实重心坐标插值 UV，读取 bindless albedo 的固定 `SampleLevel(materialMipLevel)`，
按 `sampledAlpha * baseColorFactor.a >= 0.5` 接受表面；不透明三角形始终忽略 alpha，合法黑色仍终止射线。
洞只剔除当前候选，不修改 `tMin`，继续检查同格候选及后续 DDA 格。
因此多个洞后仍能找到后方表面，共面的 opaque 候选也不会因跳过一个洞而丢失。
最近接受命中的距离/三角形索引排序和真实表面身份保持原规则，没有整物体忽略或起点位移。

cutout 的 material ID 越界、非有限 barycentric/UV、非有限 alpha、非有限或负 mip 返回 invalid trace，按失败关闭穿透：
分类禁用，太阳可见性和邻边关闭，raw 查询拒绝贡献者，混合接收点确认失败时禁用世界回退。
这不同于已验证的 alpha 洞；invalid 不允许露出后方亮面。
已注册纹理 descriptor 的合法性仍由资源系统保证；纹理像素内容仍遵守静态资产契约，没有新增同 descriptor 的运行时像素编辑失效机制。

CPU BVH、无符号节点距离、完整三角形 CSR 以及稀疏表面分配仍使用完整三角形。
接受覆盖的表面是这些三角形的子集，因此完整三角形距离仍是覆盖几何距离的保守下界；alpha/mip 编辑不需重烘焙节点或 CSR。
几何-only trace 重载保留给明确的诊断/golden；它不能作为 alpha 穿孔参考。
alpha 正确性使用有已知层次、真实材质纹理和 mip 的 GPU fixture 核对。

运行时覆盖一致接入固定探针的 serial/parallel 分类与采光、`Lo0` 源查询及太阳阴影、中心间闭区间邻边、
raw 八角点到真实目标端点的可见性、SDF/surface 诊断、SSGI camera 首次表面确认与世界回退。
屏幕的有效黑色命中仍终止射线，source anchor 失败只去掉附加间接项。
世界固定 mip 与 raster 导数 footprint 仍可能在 alpha 边缘取到不同覆盖，camera 确认对此保守失败；没有声称与实际 raster alpha 逐像素一致。

### 编辑失效与绑定

`VXGIWorld` 的 coverage fingerprint/revision 记录三角形 resolved material ID 与 cutout 标记；仅修改 override/flags 时仍复用几何。
`VXGISurface` 的独立 coverage revision 比较材质记录数量、albedo descriptor、baseColor alpha、World coverage revision 及固定材质 mip。
这些变化进入 `VXGIProbeState::Sources`，触发 reset1：清分类、SH 和计数，并重新分类选中批次。
filter 同时核对 pending coverage revision，清全部旧 known edge mask 与 filtered 记录，随后逐批重算；拓扑无需重建。
RGB、emission、unlit 或太阳变化只使源 SH 失效，保留可靠分类和已知边。
失败 recording 不消费 pending key、epoch 或滤波 coverage revision；只有成功 fence `Signal` 后才提交历史与缓存状态。

filter constants 保持 80B，在原 padding 中增加 material count/mip，新增 `t9` 材料、`s0` linear wrap 和 bindless heap flag。
root 为 20 DWORD constants、10 个 root SRV、2 个 root UAV，共 44 DWORD；实际材料缓冲作为 external SRV 注册进 RDG。
表面诊断重用已有 `t4` 材料，root 保持 64 DWORD；probe update 和混合 SSGI 保持已有材质绑定。
所有路径先绑定 engine shader-visible heap，再绑定 root signature 和 arguments。

### 后台验收记录

本阶段后台验收已完成：

- 扩展 CPU 场景联动 Debug/O2 `/W4 /WX` 各 5 组通过，覆盖 coverage 与 source-only 区分、几何/节点复用和失败重试。
- 15 个 production/validation shader entries 的 Debug/O3 SM6.6 `-WX` 编译通过，记录为 `tmp/VXGIAlphaShader-validation.log`；主工程 Debug/Release 构建通过。
- 独立 alpha GPU Debug/O2 各 14 组、38 次提交、37 个外层请求和 4 个实际 cached filter 编辑阶段通过。
  请求数量不等于内部分类/阴影总射线数；覆盖多层洞、0.499/0.5 阈值、baseColor alpha、共面 opaque、真实 UV/mip、invalid 关闭、阴影、严格端点、raw 查询及分类。
  filter 直接运行 production reset/visibility/filter kernels，确认覆盖改变清缓存、RGB-only 保留边；这项不是完整 production FilterPass wrapper 提交验收。
- Probe Debug/O2 各 14 组；新增 alpha shell 直接运行 production serial/parallel classification/capture，检查洞→covered→洞及 opaque alpha0，48 条 parallel/reference 记录一致。
  最大 SH/`E/pi` 数值误差 `0.000154789`、真实源 SH 误差 `9.35793e-6`、历史误差 `2.38419e-7`、parallel/serial SH 差 `1.43051e-6`。
  同次原 opaque production ProbePass wrapper 的 32 次提交、3 槽、978 条记录继续通过；该 wrapper 原有 move/resize 覆盖不等于 alpha shell 已走完整 ProbePass 绑定。
- Query/Filter Debug/O2 各 10 组、881 条滤波记录、19 次表面查询通过，SH 滤波误差 0、查询最大误差 `1.1920929e-7`。
  实际 FilterPass/RDG wrapper 的 13 次提交验证自动 coverage revision 触发 reset1、同容量缓存清空及只恢复选中批次，恢复覆盖后重建所有边，RGB-only reset2 保留已知边。
  这项使用 opaque 几何隔离失效调度；真实 alpha 几何与贴图穿孔由上面的专项 kernels 验证，未将两者合称为主场景画面验收。
- Hybrid Debug/O2 各 12 组、30 条 helper 射线、7 个 production CS 像素；Debug radiance/kernel 最大误差均为 0，O2 radiance 最大误差 `7.45058e-9`、kernel 误差 0。
  新增 alpha 洞后的 camera D32 确认、后方世界源、实际 trace kernel 与 invalid coverage 关闭；原 wrapper 仍为 12 次提交、3 槽、2 次尺寸变化。

已完成 GPU fixtures 的 D3D12 debug layer/GBV InfoQueue 均为 0 warning/error，窗口始终不可见、Present 为 0。
独立 [alpha Debug 报告](<D:/项目/LearnDirectX/tmp/VXGIAlphaCutoutGPU-Debug-report.json>) 与
[alpha O2 报告](<D:/项目/LearnDirectX/tmp/VXGIAlphaCutoutGPU-O2-report.json>)、
[Probe Debug 报告](<D:/项目/LearnDirectX/tmp/VXGIProbeGPU-stage73-Debug-report.json>)、
[Probe O2 报告](<D:/项目/LearnDirectX/tmp/VXGIProbeGPU-stage73-O2-report.json>)、
[Query Debug 报告](<D:/项目/LearnDirectX/tmp/VXGIProbeQueryGPU-stage73-Debug-report.json>)、
[Query O2 报告](<D:/项目/LearnDirectX/tmp/VXGIProbeQueryGPU-stage73-O2-report.json>)、
[Hybrid Debug 报告](<D:/项目/LearnDirectX/tmp/VXGIHybridGPU-Debug-report.json>)、
[Hybrid O2 报告](<D:/项目/LearnDirectX/tmp/VXGIHybridGPU-O2-report.json>) 保留。
既有表面材质/源光 Debug 的 25 次 GPU 查询继续通过，材质最大误差 `1.1920929e-7`，源光最大误差 `8.9407e-8`，InfoQueue 0 warning/error。
另保存 [48 项验收输入 SHA256](<D:/项目/LearnDirectX/tmp/VXGIStage73-input-hashes.json>)，对应上述最终代码、测试、共享 shader 和 Plain 资产。

没有启动可见主程序或操作用户窗口；以上合成几何、纹理和 D32 fixtures 不替代真实场景画面验收。
阶段 7.2 的 3.958 ms/p95 12.217 ms、valid 27,181 和质心支持率属于 alpha 接入前，阶段 7.3 尚无新的真实场景性能数字。
下一步在现有 256/64 预算下重测 post-alpha 支持率，分清候选缺失、禁用与可见性剔除，并定位全场批次长尾，再决定加速或预算调整。
探针仍只采集 `Lo0`，没有新增反馈反弹；透明、骨骼变形、局部脏区和更多光源按后续实测需求处理。

## 阶段 7.4：post-alpha 真实场景验收

本轮沿用 `Settings/Scene.json` 的汽车与地面、原有 0.5m 体素、1m SDF、每批 256 探针和每次 64 条采光射线。
性能测试与离屏渲染串行运行，没有显示窗口、调用 Present 或操作用户桌面。alpha 接入前的 7.2 报告和输入清单保持原样。

### 全场分类与有限性能样本

`Tests/RunVXGIStage7Benchmark.ps1` 现在输出独立的 [7.4 性能报告](<D:/项目/LearnDirectX/tmp/VXGIStage74Benchmark-report.json>)，
以及 [31 项 SHA256 输入清单](<D:/项目/LearnDirectX/tmp/VXGIStage74Benchmark-input-hashes.json>)，前后输入一致。
在 RTX 5070 Laptop 上运行实际 ResourceManager、ProbePass、FilterPass 和 RDG，两轮各 209 批，再提交 3 批退休时间戳，共 421 次提交。
每次提交独立等待 fence；性能 run 关闭 debug layer/GBV，不把既有正确性 fixture 的 InfoQueue 0 算作本次性能 run 的验证层结果。

129,386 个三角形产生 53,306 个固定探针：27,181 valid、26,125 disabled，其中背面 26,123、起点过近 2。
全场 unclassified、TraceFailure、SampleFailure 都为 0；分类统计和全部原始 SH 分量有限，负 SH 系数按合同保留。
26,692 个 valid 探针完成两次采光、489 个完成三次，禁用探针更新数为 0；这证明两轮调度覆盖，没有证明 32 次历史窗口已收敛。

| GPU 范围 | median | p95 | max | 含义 |
|---|---:|---:|---:|---|
| 首轮 Probe | 4.255 ms | 17.195 ms | 32.031 ms | 包含全历史 copy、reset、分类和选中批次采光 |
| 第二轮 Probe | 3.084 ms | 13.576 ms | 22.460 ms | 已分类的全场批次，仍包含全历史 copy 和采光 |
| 首轮 Filter | 0.008544 ms | 0.939744 ms | 5.031872 ms | 首轮逐批发现并缓存几何邻边，首批全局 reset |
| 第二轮 Filter | 0.006144 ms | 0.006848 ms | 0.008928 ms | 可复用已知邻边，仍保留最后不足 256 的批次 |

时间是当前后台运行的有限样本，未锁频，用户可同时工作，不是主程序整帧或多帧在途吞吐。
第二轮最慢批次从 probe 26,112 开始，长尾保留。纯 CPU 的同输入重建核对三角形数、探针数和 geometry fingerprint 完全一致，
该批中心 AABB 为 `(-0.75,-0.75,-40.25)` 到 `(-0.25,2.25,40.25)`，占据格候选引用中有 21,628 个车和 223 个地面引用。
不过第四慢批次只有地面、259 个引用，因此不能仅凭空间关联把长尾归因于车辆密度或某个 kernel。
详见 [CPU 热点补充](<D:/项目/LearnDirectX/tmp/VXGIStage74Benchmark-hotspots-cpu.json>)，该补充没有初始化 D3D12 或提交 GPU 工作。
平均值较低不能消除 p95/max 风险：13.576/22.460 ms 仅是 Probe 时间，尚需叠加 raster、SSGI 和其它阶段，不能宣布 60fps 达标。

CPU 首次 World/SDF/Topology 分别为 192.623/147.549/12.286 ms；稳定 World+SDF+Topology 的 16 次检查 median 0.0007 ms，
不包含材质刷新、SceneEdit 或主程序全部 CPU 工作。25 个公开 DEFAULT 资源的 committed allocation footprint 仍为 103.75 MiB，
不是物理驻留显存；资产、材质、query、主画面资源及私有 staging/readback 不在该 allocation 合计内。

### 表面支持率与缺失原因

新增 `Shaders/VXGIStage74SupportValidate.hlsl` 使用生产覆盖、追踪、材质和 RawQuery helper。
每实例请求 256 个面积分层的三角形质心，分别从几何法线两侧 0.25m 发出 primary ray。
先检查真实质心 UV 的 alpha，再要求首次接受命中是该精确三角形和端点，最后用真实 mapped normal 查询 raw 探针。
支持率分母只包含通过上述验证的目标；不同 primary、洞、invalid、miss 与失败独立计数。

| 样本组 | 请求 | 通过 primary 验证 | 有支持 | 支持比例 | 不同 primary |
|---|---:|---:|---:|---:|---:|
| 汽车正面 | 256 | 172 | 110 | 63.95% | 84 |
| 汽车背面 | 256 | 151 | 10 | 6.62% | 105 |
| 地面正面 | 256 | 256 | 256 | 100% | 0 |
| 地面背面 | 256 | 256 | 0 | 0% | 0 |

汽车正面 62 个无支持样本中，28 个的已分配候选全部禁用，34 个的有效候选全部被几何半球或严格 visibility 拒绝；
没有全部八角点缺失的样本。valid 黑色也算有支持，汽车正面 110 个有支持样本中 36 个重建为黑色，不能把亮度与支持混为一谈。
4 组的状态、corner 和 unsupported 分区逐组闭合，835 个 verified query 的生产结果与诊断 accepted/failure 计数一致；没有 query visibility failure。

这说明固定中心加禁用规则在复杂薄结构附近会留下可用性缺口；当前数据不能保证整个汽车表面都有缓存。
该统计只描述侧向首命中验证后的质心，不是屏幕像素或无偏表面积覆盖率，也不能与旧的 126/256 直接比较。
地面的 256 次请求只重复两个不同质心，100% 不能代表整块地面的空间覆盖。
本场景的 1,024 次请求没有命中 alpha 洞；真实 alpha 洞正确性仍由 7.3 专项 fixture 证明，不能用此支持率样本声称覆盖了洞。

53,306 / 256 需要 209 次提交才能遍历一轮；若主程序每帧提交一次、实际达到 60fps，一轮约 3.48 秒，32 次更新窗口约 111 秒。
这是调度推算，不是测得的主程序 fps；source/coverage reset 全清 SH 后逐批恢复，因此冷启动和编辑后的恢复速度仍需优化。

### 实际离屏画面

`Tests/RunVXGIStage74RenderAcceptance.ps1` 使用真实模型、贴图、方向光与已配置 HDR 的 IBL，执行 production
Shadow、GBuffer、HBAO、Deferred、SSGI 和 Skybox。内部 640×400、GI history 320×200，固定 LOD0、无 jitter、raster mipBias0。
RAW 在两轮 418 批后冻结，所有相机和模式使用同一缓存；四个相机、四种模式每组各自重置 SSGI history 后累计 8 帧，采样帧序列相同。
这包含真实 raster D32 和生产材质，不再是合成深度 fixture；但不包含主程序 DLSS、postprocess、透明或编辑器。
当前主配置 2560×1600/DLSS Quality 的 jitter、负 mipBias 与 15/40m raster LOD 切换没有被这组取代。

测试暴露并修复两处生产 warning：HBAO 的 R16 清屏值现在与 optimized clear `{1,0,0,0}` 一致；
IBL 的 SH DEFAULT buffer 以 COMMON 创建，在 compute 写入前明确转换到 UAV，随后转换到 CBV。
新增 fixture 的 query buffer 同样修正状态，HDR clear 匹配资源创建值，WIC 输出按实际 BGR 通道编码。
修复后主工程 Debug/Release 构建通过，真实离屏运行 debug layer InfoQueue 为 0 warning/error；本离屏测试未启用 GBV。

前三个相机为 `(8,4,-10)` 看向 `(0,1,0)`、横移到 `(8.6,4,-10)` 仍看向 `(0,1,0)`，以及原位置转向 `(-8,0,2)`。
第四个在原位置看向 `(-30,0,-14)`，汽车完全离开视野，134,159 个前景 first hit 全属于地面。
实际资产汽车 bounds 最低 Y=-1.519m，地面 Y=0，当前场景确有模型进入地面；这会影响外观、遮挡与探针禁用统计。
全图前景都是 ORM.a>=0.5 的 GI eligible 材质，仍保留单独的 eligibility 计数以避免将来混入 unlit。

| 相机 | 前景 | anchor verified | 对前景的支持比例 | 车 verified 中有支持 | 地面 verified 中有支持 |
|---|---:|---:|---:|---:|---:|
| 固定 | 175,890 | 81.82% | 78.98% | 23,875/28,525 | 115,051/115,395 |
| 横移 | 173,963 | 91.73% | 88.42% | 29,133/34,582 | 124,679/124,992 |
| 转向 | 165,340 | 74.00% | 71.80% | 16,886/20,133 | 101,829/102,224 |
| 汽车完全出屏 | 134,159 | 61.62% | 61.62% | 无汽车目标 | 82,668/82,668 |

instance 分组来自 SDF first hit；verified 与支持样本可解释为经确认的目标，失败样本的 SDF 身份不能当作 raster primitive ID。
总体 verified 中支持约 96%–97%，主要由大面积地面主导，不能用来宣称汽车覆盖接近完整。
前三个相机 mismatch 分别为 31,970/14,389/42,982，主要集中于 3–4 个 D32 ULP，固定相机另有 7,108 个 UV 容差超限像素，
两种原因可重叠。支持图的地面红色条带与这些拒绝一致，显示当前确认数值策略在真实 raster 上过于敏感。
单平地面的集中小 ULP 差很像舍入造成的假拒绝，但缺少 raster primitive ID，不能将每个 mismatch 都判成同一表面。

原始 query 的失败贡献射线分别为 1,896/2,181/1,267，涉及 1,674/1,879/1,097 个像素，均归属于汽车 first hit。
其中 353/479/356 个失败像素没有其它可用贡献，地面没有此类 query failure。
诊断重放每个失败贡献者的同一生产 trace，逐像素重放计数与生产结果一致：

| 相机 | miss 无法证明端点 | 同目标距离超容差 | 同目标位置超容差 | 硬 invalid / iteration-limit |
|---|---:|---:|---:|---:|
| 固定 | 899 | 994 | 3 | 0 / 0 |
| 横移 | 1,002 | 1,179 | 0 | 0 / 0 |
| 转向 | 572 | 692 | 3 | 0 / 0 |

没有 invalid 输入或零距离。`traceFailures` 在这里都是端点证明失败，不能统称硬 SDF 异常。
严格拒绝避免误接受其它表面，但会损失可用缓存，仍需定位数值来源；测试将 `numerical_validation_passed` 标记为 false。

真实 RDG 的 GPU timestamp 覆盖上述渲染阶段，排除 readback、验收 query 和 probe 更新。每模式/相机只有 8 个样本，
debug layer 开、GBV 关，未锁频，每帧独立等待，以下数字不能当作主程序整帧吞吐或精确优化收益。
最终运行的前三个相机 GI off median 约 0.46ms，screen-only 约 0.66ms，世界 `Lo0` 回退约 199–226ms，`Lo1` 缓存反弹约 286–400ms。
第四个出屏相机 `Lo0`/`Lo1` median 分别为 6.91/8.85ms，说明费用强烈依赖实际可见区域和查询路径，不能用地面视角代表近车预算。
尽管分辨率缩小，世界追踪已经远超实时预算；后续应优先优化实际追踪和候选检查，再增加功能。

图像 RGB/alpha 与实际 D32 均有限合法。相同相机 mode2-mode1 的前景 mean absolute RGB 差约 0.00029–0.00033，
说明缓存附加项已进入最终画面，但该场景的可见收益很小；没有 ground truth，不把差值当物理精度或质量达标证据。
图像使用固定 exposure/Reinhard/sRGB 诊断映射，与主程序 postprocess 不同。原始 float4、GI layer 与支持图一并保存于 `tmp/vxgi-stage74-render/`。

完整出屏视角的 screen-only GI history 为零，世界 `Lo0` 和 `Lo1` 分别约为 `5.69e-5`/`6.36e-5` 的全图 mean RGB `E/pi`。
它说明实际屏幕外世界源仍可被查询、探针附加项仍进入管线，能量很弱；没有按同一 world ROI 跟踪，不能拿不同地面视角证明亮度连续。
另有 mode2 保留 production history 的四个相机姿态序列，每个姿态连续 4 次提交，保存到达时的第一帧和第四帧。
RGB/GI 均有限，实际 reprojection/disocclusion 已后台执行；这是离散移动/转向的流程验证，没有平滑路径或 ground truth，不能宣称无闪烁/无拖影。

离屏报告：[实际渲染与数值状态](<D:/项目/LearnDirectX/tmp/VXGIStage74RenderAcceptance-report.json>)；
[106 项最终输入清单](<D:/项目/LearnDirectX/tmp/VXGIStage74RenderAcceptance-input-hashes.json>) 前后及最终校验一致；
CPU 解读与复现为 [图像/ROI/时间分析](<D:/项目/LearnDirectX/tmp/VXGIStage74RenderAcceptance-analysis.json>) 和 `tmp/VXGIStage74Analyze.py`；
[四相机四模式对照图](<D:/项目/LearnDirectX/tmp/vxgi-stage74-render/stage74-comparison.png>) 已实际检查。
图像示例：[固定相机 mode2](<D:/项目/LearnDirectX/tmp/vxgi-stage74-render/camera0_mode2.png>)、
[支持图](<D:/项目/LearnDirectX/tmp/vxgi-stage74-render/camera0_mode2_support.png>)。
支持图黑色为背景、红色为 anchor 拒绝、青色为 verified 但无 raw 支持、绿色为 verified 且有支持。

本轮验收结论：基础缓存、alpha 链路、失效事务和后台资源检查可用；实景数值可用性与实时性能尚未通过最终验收。
优先处理相机确认的数值误拒绝、探针端点证明失败及世界查询开销，再测主分辨率、DLSS/LOD 和更完整的运动质量。

## 阶段 7.5：世界查询加速与追踪分辨率预算

本轮针对实际 2–5 FPS 的不可用状态定位开销。距离场原先只帮助跳过空域，进入表面格后仍逐个检测 CSR 中的全部三角形；
真实汽车场景单格最多 11,219 个候选。新增逐 RDG pass 的 GPU timestamp 后，640×400 固定相机 mode2 的 `SSGI.Trace`
三帧 median 为 341.25ms，而其它 SSGI pass 合计约 0.12ms；1707×1067 的 Trace 三帧为 979.47/1067.81/1297.77ms。
追踪中还包含 camera anchor、世界首命中、太阳遮挡和 raw 探针可见性查询，因此不能将这些总时间都归为一次 primary ray。

### 密集格子 BVH

`Resources/VXGITraceAcceleration.h` 对候选数大于 32 的格子构建无栈 BVH，叶子最多 8 个三角形；小格子继续使用原始线性列表。
CPU 原始 CSR 与距离节点保留，GPU 在已有 reference SRV 中打包 marker、原始 ID 列表与 BVH，不增加 root SRV 或 DWORD。
节点采用前序布局，保存 bounds、绝对 escape word 和叶子数量，未相交子树直接跳至 escape；几何变化才重建，材质/覆盖编辑复用加速结构。
它仍是全局无符号保守距离场配合精确三角形复核，没有改成每模型 SDF，也没有改变覆盖、命中身份、SH 或光照契约。

bounds 同时包含原 `RayTriangle` 的重心容差边缘、坐标浮点误差与向外舍入；接近零的射线方向按显式平行 slab 处理。
BVH 使用整个当前格 entry/exit 区间，不按当前最近命中剪掉后续 coverage 检查。
如果任何可能命中的候选出现 invalid coverage，则清掉本格临时结果，按原始 CSR ID 顺序重放，保留旧的 fail-closed 行为。
alpha 洞仍只拒绝该候选，共面 opaque 和最小 triangle ID 的同距选择继续有效；DDA 与距离下界推进规则未改变。

真实场景有 235 个加速格子、51,861 个节点、26,048 个叶子。三个帧槽 offsets/reference 的公开 DEFAULT committed allocation 合计 9MiB，
每槽 payload 分别为 188,276/2,929,872 字节；这是加速查询两类缓冲的分配量，不是整个 GI 或实际驻留显存。
构建成本在不同后台 run 约 38–52ms，计时与渲染分开；稳定几何不重复构建。

只加 BVH、保持原每轴 1/2 追踪分辨率时，同一 RAW、相机、模式与三帧采样序列的 9 张最终 float4 HDR 全部逐字节相同，
其中 640×400 为 8 张，1707×1067 为 1 张。640×400 mode2 Trace median 从 341.25 降至 21.85ms；
较大分辨率三帧仍有 104.00/100.15/17.24ms 的波动，不能根据最快一帧宣布达标。
对应报告：[640×400 BVH A/B](<D:/项目/LearnDirectX/tmp/VXGIStage75-stage75-linear-vs-stage75-accelerated-640x400-comparison.json>)、
[1707×1067 BVH A/B](<D:/项目/LearnDirectX/tmp/VXGIStage75-stage75-linear-vs-stage75-accelerated-1707x1067-comparison.json>)。

### GI 追踪与历史预算

`ssgi.trace_downsample_factor` 接受 2/4/8，贯穿 Trace、Reconstruct、Temporal、A-trous 与 Composite 的 full-resolution guide 映射，
history 与工作纹理统一采用向上取整的尺寸。两套历史必须全部匹配本次 factor 与尺寸，否则不记录该 pass；factor 进入成功提交的 source key，
变化时清历史，失败记录仍保留待提交状态。每采样点的射线数、世界范围、源材质响应、屏幕/世界分支和每条射线的遮挡查询均沿用原算法。
factor 8 在 1707×1067 下实际为 214×134，factor 2/4 分别为 854×534 和 427×267。
这是减少 GI 采样点的质量/成本取舍，尚未实现 Lumen 式按时间预算改变追踪方法。

调整 shader 参数布局后，factor2 回归在 1707×1067 上最终 HDR 仍逐字节相同；640×400 的两张图存在少量 FP16 舍入差，
最大 RGB 差 `0.0001220703125`、全图 mean absolute RGB 差小于 `2.6e-9`，alpha/非有限值差为 0。
因此 BVH 独立对照的 byte-identical 结论不能扩大到全部后续 shader 参数变更。

为避免 Debug 主程序对最重 GI shader 使用 `-Od`，`ShaderCompiler::CompileFromFile` 新增默认关闭的 `optimizeInDebug` 参数。
SSGI 与探针更新/滤波的调用明确开启，在 Debug 保留符号并使用 `-O3`；其它调用保留原编译策略。
Release 的编译行为继续沿用原优化配置。下方预算数据来自 O2 native harness；Debug 符号路径的最终运行核对单独记录。

### 同输入的三档后台对照

`Tests/RunVXGIStage75Performance.ps1` 使用真实 LOD0 资产、贴图、当前 HDR 的 IBL，以及 production Shadow/GBuffer/HBAO/Deferred/SSGI/Skybox。
RAW 从两轮真实探针更新中采集后冻结，SHA256 为 `082BAC757C181929048B16F4FD75D96ED4F23A85625972FEA50995CB84696AEA`；
各 factor 重新清 SSGI history，使用相同的 12 帧采样序列，最后一帧比较实际线性 HDR。640×400 另测汽车完全出屏相机。
1707×1067 是 2560×1600/DLSS Quality 的名义内部尺寸，本轮没有查询或启动主程序验证其实际 viewport 尺寸。

| 1707×1067，固定相机 mode2 | Trace median | Trace p95/max | 离屏 RDG median | 离屏 RDG p95/max |
|---|---:|---:|---:|---:|
| factor 2 | 18.29ms | 35.40ms | 20.50ms | 45.44ms |
| factor 4 | 12.30ms | 26.07ms | 16.96ms | 40.73ms |
| factor 8，当前配置 | 4.85ms | 7.05ms | 6.67ms | 9.00ms |

每组只有 12 个样本，p95 采用 nearest-rank，因此此处等于 max。debug layer 开、GBV 关，未锁频，用户可以同时使用 GPU，每帧独立等待 fence。
时间范围排除 probe 更新、DLSS、postprocess、编辑器、透明、readback 与 Present；不是完整主程序整帧吞吐，不能取倒数当主程序 FPS。
此轮 factor2 比早期三帧 A/B 更快，反映后台运行条件和样本的变化，不能将全部差距都归因于分辨率预算。
640×400 的 factor2/4/8 Trace median 为 11.50/5.20/7.01ms，factor8 在小尺寸本轮并未更快，原因尚未确认；
当前大尺寸采用 8 的依据是上表整轮数据，小尺寸可手动选择 4。

factor8 相对 factor2 的最终前景 mean absolute RGB 差为 `4.20e-4`，RMSE `2.04e-3`，最大局部差 `0.18384`；
前景 mean luminance 从 0.479844 降至 0.479744，约 -0.021%。factor4 对应 mean RGB 差 `3.08e-4`、RMSE `1.34e-3`。
固定诊断映射的预览整体接近，差异图仍能看到地面反弹与边缘处更粗的局部变化；小平均差不意味着零质量损失。
这些是同一实现不同预算之间的差异，没有 ground truth，不证明物理精度、细节保留或平滑运动质量达标。

报告：[factor2](<D:/项目/LearnDirectX/tmp/VXGIStage75-stage75-budget2-1707x1067-report.json>)、
[factor4](<D:/项目/LearnDirectX/tmp/VXGIStage75-stage75-budget4-1707x1067-report.json>)、
[factor8](<D:/项目/LearnDirectX/tmp/VXGIStage75-stage75-budget8-1707x1067-report.json>)；
[factor2 对 factor8 的前景误差](<D:/项目/LearnDirectX/tmp/VXGIStage75-stage75-budget2-vs-stage75-budget8-1707x1067-comparison.json>)、
[factor8 图像](<D:/项目/LearnDirectX/tmp/vxgi-stage75-performance/stage75-budget8-1707x1067-camera0_mode2.png>)、
[8 倍绝对差预览](<D:/项目/LearnDirectX/tmp/vxgi-stage75-performance/stage75-budget2-vs-stage75-budget8-1707x1067-camera0_mode2_absolute_difference_x8.png>)。

最终配置另用 `/DDEBUG /O2 /MD` native harness 执行 12 帧，进入 production ShaderCompiler 的 Debug 分支，GI 保留符号并优化，
其它 shader 保留原 Debug 策略；这项验证 shader 策略，不代表 Debug CPU 吞吐。1707×1067 factor8 Trace median 为 5.036ms、
p95/max 11.114ms，离屏 RDG median 为 7.005ms、p95/max 21.339ms。仍沿用上述排除范围与未锁频条件。
同 RAW、相机和最终 sample11 对原 factor8：D32 内容相同，569 个 RGB 像素有差异，最大 `0.00048828125`、
全图 mean RGB 差 `2.306e-8`、前景 mean RGB 差 `3.356e-8`，alpha 差与非有限值均为 0；不宣称逐字节一致。
保存 [最终 Debug shader 策略报告](<D:/项目/LearnDirectX/tmp/VXGIStage75-stage75-final-debug-policy-1707x1067-report.json>)、
[数值对照](<D:/项目/LearnDirectX/tmp/VXGIStage75-stage75-budget8-vs-stage75-final-debug-policy-1707x1067-comparison.json>) 和
[最终摘要](<D:/项目/LearnDirectX/tmp/VXGIStage75-final-summary.json>)。

### 实际缓存更新预算

`Tests/RunVXGIStage7Benchmark.ps1 -Label stage75-accelerated` 单独重测当前全场 production ProbePass/FilterPass：
保持每批 256 探针、64 分类射线和 64 采光射线，两轮各 209 批加 3 次 timestamp drain，共 421 次更新提交。
53,306 探针中 valid 27,181、disabled 26,125、unclassified 0；硬 trace/sample failure 0、SH 全部有限，分类汇总与历史 7.4 相同。
geometry、classification 与完整 surface_support JSON 均与保存的 7.4 报告一致，包含已记录查询的值、状态和贡献者统计；比较范围为报告中的请求。
禁用探针仍固定在原中心，没有通过迁移、放宽遮挡或降低每点射线数改善时间。

| 当前全场批次，209 个样本/阶段 | median | p95 | max |
|---|---:|---:|---:|
| 首轮分类与采光 | 0.274ms | 1.564ms | 3.205ms |
| 首轮可见性与滤波 | 0.00845ms | 0.08803ms | 0.11197ms |
| 已分类轮采光 | 0.186ms | 1.122ms | 2.062ms |
| 已分类轮滤波 | 0.00602ms | 0.00646ms | 0.00726ms |

Probe 计时包括完整旧历史 copy、reset/分类/采光，排除 snapshot；filter 包括必要 reset、选中邻边可见性与滤波。
该测试不开 debug layer/GBV，逐提交等待 fence、未锁频，独立于上面的冻结 RAW 渲染测试。
历史 7.4 已分类轮 Probe median/p95 为 3.084/13.576ms；新旧不是受控的同次运行，不将差距全归为某项优化，
也不将两个独立测试的 median 相加或取倒数宣称主程序 FPS。
目前公开 DEFAULT committed allocation 为 115,474,432 字节（110.125MiB），比原基线增加 6,684,672 字节（6.375MiB）。
范围是 21 个共享场景缓冲、两套 raw、filtered 与 edges，排除资产、材质池、RDG 工作纹理、upload/readback 与实际驻留量。
保存 [当前全场缓存报告](<D:/项目/LearnDirectX/tmp/VXGIStage74Benchmark-stage75-accelerated-report.json>) 和
[67 项输入清单](<D:/项目/LearnDirectX/tmp/VXGIStage74Benchmark-stage75-accelerated-input-hashes.json>)，原 7.4 文件保留。

### 回归与剩余验收

- `Tests/RunVXGITraceAccelerationTests.ps1` 的 CPU Debug/O2 各 5 组通过，覆盖 2,400 条近远/掠射/重心容差边缘射线；
  候选仅保留 6,414/230,400，未出现错误剔除。既有 SDF Debug/O2 4 组与场景联动 Debug/O2 5 组继续通过。
- `Tests/RunVXGIDenseBVHGPUTests.ps1` 在 RTX 5070 Laptop 的 Debug/O2 各 10 组、11,820 条射线、40 个密集格子和 10 次提交通过；
  opaque、alpha 洞、共面 tie、远坐标、零方向分量、掠射与多类 invalid coverage 的旧 CSR/新 BVH 输出逐位一致。
  最大误差 0，debug layer/GBV 的 warning/error 0，隐藏窗口无 Present。
- `Tests/RunSSGIResolutionConfigTests.ps1` 的 Debug/O2 配置回归通过，覆盖默认与合法档位、11 类非法 JSON；
  `SSGIHybridState` 的 factor 改变和未提交重试回归通过，18 个 SSGI shader variants 的 Debug/O3 SM6.6 `-WX` 编译通过。
- 正常三档预算测试共 108 帧，D3D12 warning/error 0、最终 RGB/alpha 有限；最终 17×11 的 factor2/4/8 各 3 帧、
  1×1 factor8 两帧的 GBV 通过，Trace/history 实际尺寸分别为 9×6、5×3、3×2、1×1，warning/error 0。
- Debug shader 策略的 Probe GPU 14 组通过，production wrapper 为 32 次提交、3 槽和 978 条 CPU 对照记录，
  production SH/history 最大差 `5.364418e-7`；Query/Filter 10 组、881 条滤波记录与 19 次查询通过，
  实际 FilterPass wrapper 13 次提交，滤波误差 0、查询最大差 `1.192093e-7`。两项 debug layer/GBV warning/error 均为 0。
  当前 [Probe 报告](<D:/项目/LearnDirectX/tmp/VXGIProbeGPU-stage75-debug-policy-report.json>) 与
  [Query/Filter 报告](<D:/项目/LearnDirectX/tmp/VXGIProbeQueryGPU-stage75-debug-policy-report.json>) 独立保留。
- 主工程最终 Debug/Release 构建与 UI 架构/XML 检查通过，保留既有 C4099/C4267 等编译 warning。
  构建记录为 `tmp/vxgi-stage75-final-debug-build.log` 和 `tmp/vxgi-stage75-final-release-build.log`。
- 小尺寸 GBV 初次运行暴露了 IBL 烘焙的 HDR SRV 状态问题：cubemap 像素读取后，SH compute 仍沿用 pixel-only 状态。
  `IBLBaker` 现在在 SH dispatch 前显式转为 `NON_PIXEL_SHADER_RESOURCE`，由最终 GBV 回归核对。

[GPU O2 等价性报告](<D:/项目/LearnDirectX/tmp/VXGIDenseBVHGPU-stage75-O2-report.json>) 与
[GPU Debug 等价性报告](<D:/项目/LearnDirectX/tmp/VXGIDenseBVHGPU-stage75-Debug-report.json>) 独立保留。
性能 runner 与 `Tests/CompareVXGIStage75Performance.ps1` 保存实际输入 SHA256 清单与逐图数值差，报告对应当时文件快照。
历史 7.3/7.4 输入清单保持原样，不能因新版本通过其它测试就宣称它们仍对应当前全部文件。

在本次 7.5 验收记录时，最终渲染/小尺寸/Debug 策略回归的 7 份输入清单与源文件一致；缓存 benchmark 的 67 项清单同样匹配。
全部后台窗口均不可见、Present 为 0，没有操作用户桌面。

本轮已降低世界查询与实际探针批次的后台成本，并提供当前场景可选择的 GI 分辨率预算；7.4 的相机 anchor 数值误拒绝、严格 probe endpoint proof 缺失、
固定稀疏探针支持缺口未在本轮修复。分批调度仍需要约 209 帧覆盖全场一次；新的主程序整帧时间、DLSS/LOD 与平滑运动质量仍须分别验收。

### 默认关闭世界调试绘制

用户指出主程序仍在绘制大量格子 gizmo 后，检查确认项目 JSON 与 `VXGIConfig` 默认值均为 3（格子和探针）。
此模式对全部 53,306 个占据中心发出 24 顶点格子线框与 6 顶点探针十字的两个实例化 draw，约 160 万个顶点/帧；
没有相机剔除或深度遮挡，实际开销尚未独立计时，不能由上面的离屏 GI 数据判断它对主程序 FPS 的贡献。
两处默认值现已改为 0（Off）。主帧在调用三个 VXGI 调试 renderer 前按模式判断，0 会跳过全部调试上传与绘制。
已有 Details 的 `Debug view` 可按需开启；配置在启动时读取，当前运行中的实例可直接选择 Off。
上述 7.5 计时和输入清单对应关闭默认调试之前的文件快照，本次默认值调整没有重跑这些性能测试。

### 纯 GI 离屏检查

`Tests/RunVXGIStage75Performance.ps1` 新增 `-GIOnly`，将 SSGI 的正常 Composite 输出到独立清黑 RGBA16_FLOAT 目标，
保持真实 Deferred bounceSource、材质、GBuffer、Shadow 和采光路径，跳过 Skybox。输出是最终间接漫反射贡献，
即 `E/pi × 接收材质漫反射响应 × intensity`，不包含直接光/IBL 底色、自发光的直接显示或 gizmo。
它不是白材质辐照度视图，也不是单独显示探针项；发光仍可作为间接光源输入。

本次用当前汽车/地面和固定相机、1707×1067、mode2 导出 factor8 与 factor2；两者各累计 32 帧，统一预览曝光4、Reinhard/sRGB。
先以 `-WarmupRounds 32` 执行 6,688 个真实探针更新批次，再冻结 RAW，factor2 导入完全相同的缓存。
53,306 个探针中 valid27,181、disabled26,125；有效探针全部完成32次更新、2,048条采光射线，SH 有限。
RAW SHA256 为 `67E74DDD0C4CC07303EF0C5271F768EB97EBD4E07C2F31FEE8414264C40267E8`。
两张图的前景 D32 相同、天空 RGB 严格为0，D3D12 warning/error0，无可见窗口或 Present；原始 float4 同目录保存。

- [当前 factor8 纯 GI](<D:/项目/LearnDirectX/tmp/vxgi-stage75-performance/pure-gi-factor8-1707x1067-camera0_mode2.png>)：实际追踪214×134，地面橙/灰色反弹宽且平滑。
- [factor2 同缓存对照](<D:/项目/LearnDirectX/tmp/vxgi-stage75-performance/pure-gi-factor2-1707x1067-camera0_mode2.png>)：实际追踪854×534，局部变化更多，同时出现明显的点状/规则暗纹，尚不能作为干净细节的参照。
- [导出统计](<D:/项目/LearnDirectX/tmp/pure-gi-capture-stats.json>) 与各自 `tmp/VXGIStage75-pure-gi-factor{8,2}-1707x1067-report.json`、输入清单保留。

当前1m SDF单元用于加速，最终仍精确复核三角形与重心坐标；实际世界光照缓存的空间间隔是0.5m，角向为4系数L1 SH。
factor8只在每8像素采样后经过5×5重构、时域、两轮Atrous和双边放大，会压掉同平面的小变化。
这些对照支持先检查屏幕采样/滤波、相机/端点确认拒绝和探针支持缺口，再决定缩小探针或SDF；尚未从本次图像确定暗纹的单一原因。
主配置保持当前档位，本次只增加离屏导出功能；额外17×11普通捕获三帧的GBV回归通过。

```powershell
.\Tests\RunVXGIStage75Performance.ps1 -GIOnly -Width 1707 -Height 1067 -Camera 0 -Mode 2 -Samples 32 -WarmupRounds 32 -DownsampleFactor 8 -Label pure-gi-factor8
.\Tests\RunVXGIStage75Performance.ps1 -SkipBuild -GIOnly -Width 1707 -Height 1067 -Camera 0 -Mode 2 -Samples 32 -WarmupRounds 32 -DownsampleFactor 2 -Label pure-gi-factor2 -ReuseRaw tmp/vxgi-stage75-performance/pure-gi-factor8-1707x1067-raw.bin
```

## 阶段 7.6：恢复原 SSGI 射线，只用探针补充未命中的 GI

本次按用户明确纠正替换了阶段6的世界回退。旧实现擅自引入独立1000距离、SDF接收面重定位、屏幕命中附加探针项、
命中表面的实时材质/太阳遮挡/直接光和额外漫反射响应；这些步骤已从生产回退中删除。

当前每条采样射线只执行以下流程：

1. 原视空间 SSGI 起点仍为 `GBufferPosition + normal * normalBias`，方向仍是原余弦半球采样，先运行原屏幕追踪。
2. 屏幕有效命中直接读取原 bounceSource 并终止，黑色也是有效命中，不再重追相机 anchor 或追加探针项。
3. 未命中时，只用 inverseView 变换同一条射线。世界射线的起点和有限终点与原段一致，长度来自 `ssgi.radius` 和原有近裁剪缩短，
   视口边界只限制屏幕可见部分，不延长原三维线段。没有接收面重定位、附加偏移或额外几何半球筛选。
4. 世界 SDF/候选 BVH 在 `[0, 原世界线段长度]` 内求第一个实际三角形命中，保留 cutout 覆盖规则。
5. 命中后仅读取周围可见的 raw 探针，按原 A→B 射线方向重建四系数 SH 辐亮度。
   方向与探针采光的 `L(P,omega)` 基底相同，不取反，不按命中法线卷积，不乘命中表面的 albedo/漫反射响应，也不发太阳射线。
6. 无探针支持或黑色缓存仍作为黑色几何命中终止；不穿透到后面的亮物体，世界 miss 为零。
7. 接收端继续按全部射线平均为 `E/pi`，然后使用原有降噪/合成和接收材质响应。采样预算恢复每轴1/2、quality1的4条射线；调试叠加保持Off。

探针自己的更新仍独立采集光照，回退消费已有缓存，不触发缓存重采光。几何 alpha 覆盖与探针可见性是遮挡判断，不是现场光照计算。
`VXGIProbeQuerySurface` 的法线卷积接口仅保留供既有辐照度诊断使用，生产世界回退改用 `VXGIProbeQueryRadiance`。
两者共用严格的八邻点可见性与权重，拒绝禁用、未更新、缺失和无效贡献，最后才截断 SH 重建的负值。

`SSGIHybridParameters` 的旧灯光/世界距离槽已改成未使用的 reserved 字段，保持96B布局和608B trace cbuffer，
生产CPU不再上传这些光照或距离值。原世界距离不再进入 SSGI source key，probe epoch 则对两个启用值都触发清历史。
`EffectiveWorldMode` 对缺失 raw/topology 的情况直接回到 screen-only；不再降级为现场Lo0求光。
界面复用已有 UI center，只保留 screen-only 和 screen+probes 两项，旧配置值1兼容为同一缓存回退。

### 当前契约的后台验收

主工程 Debug/Release 构建通过，编辑器架构检查与项目 XML 检查通过；原有 C++/链接警告仍保留。
SM6.6 Debug/O3 的8个 shader 变体通过，反射确认96B/608B布局未变。CPU source/history state 的 Debug/O2 各3组通过。

- 混合 GPU Debug/O2 各10组、21条射线断言、5个实际 trace 像素断言；实际 SSGIPass/RDG 各12次提交、3个槽、2次尺寸变化。
  覆盖有限端点、原偏移、相机变换、近裁剪缩短、屏幕黑/非黑命中、alpha孔洞、无支持终止和缺失缓存回退。
  Debug 另以白色受光材质、巨大自发光和零缓存确认世界回退仍为黑色；黑色命中材质与正缓存则返回缓存。
- 探针查询 GPU Debug/O2 各15组、881条过滤记录、19次旧辐照度查询、23次方向查询。
  方向重建最大误差 `5.96046448e-8`，混合辐亮度最大误差 `4.76837158e-7`，实际历史/合成最大误差 `0.000439465046`。
  旧 `maximum_kernel_error` 计数器只记录首个参考像素，不作为5个像素的统一最大值。
- 两套 GPU 回归均开启 debug layer 和 GBV，D3D12警告/错误0；全部使用隐藏窗口，无Present。
  混合各74项输入、查询各26项输入前后稳定。O2混合测试夹具早于上述零缓存用例强化，精确快照已归档；两轮生产输入完全一致，
  详见 `tmp/VXGIHybridGPU-stage76-validation-notes.md`。当前源码及捕获输入的最终哈希核对收录于
  [7.6验收汇总](<D:/项目/LearnDirectX/tmp/VXGIStage76-final-summary.json>)。

当前场景重新导出纯SSGI(mode0)与探针回退(mode2)，均为1707×1067、每轴1/2追踪(854×534)、原radius0.5、32帧、预览曝光4。
两者复用上文同一已收敛RAW快照；本轮只改消费端，没有重新采光。两图深度逐位相同，HDR有限，天空RGB严格为0，捕获D3D12警告/错误0。

- [当前探针补充的纯GI](<D:/项目/LearnDirectX/tmp/vxgi-stage75-performance/stage76-finite-probe-gi-1707x1067-camera0_mode2.png>)
- [同条件纯SSGI对照](<D:/项目/LearnDirectX/tmp/vxgi-stage75-performance/stage76-screen-gi-1707x1067-camera0_mode0.png>)

| 后台Trace GPU时间，32帧 | median | nearest-rank p95 | max |
| --- | ---: | ---: | ---: |
| 纯SSGI | 0.684ms | 1.197ms | 1.211ms |
| 原有限射线＋探针回退 | 3.955ms | 4.788ms | 7.634ms |

计时仅为冻结RAW的Trace pass，排除探针更新、主窗口、Present、编辑器、DLSS与postprocess，不能换算主程序FPS。
实图可见局部缓存补光，细节仍受0.5m探针、4系数SH和现有降噪影响，噪声及最终质量尚未验收通过。
旧7.4/7.5性能与纯GI图对应此前算法，不能作为新契约的结果。

### Debug帧率反馈后的预算核对

用户反馈VS启动的Debug比修改前更慢。本轮先保持生产源码不变，在相同相机、RAW、1707×1067、quality1、32帧下，
分别测量当前factor2与factor8。开启production Debug shader策略及D3D12 debug layer、关闭GBV，隐藏窗口、无Present，警告/错误0。
native harness仍为O2/MD，仅复现Debug shader策略，未测VS Debug CPU或完整主程序帧率。

| 当前代码 | 实际追踪尺寸 | Trace median | Trace p95 | Trace max |
| --- | --- | ---: | ---: | ---: |
| factor2 | 854×534 | 4.352ms | 6.440ms | 6.711ms |
| factor8 | 214×134 | 1.820ms | 2.283ms | 2.629ms |

恢复factor2把候选追踪像素增加约15.9倍，本轮Trace median约为factor8的2.39倍；像素倍数不等于耗时倍数。
同一RAW的历史Release策略factor2为18.642ms，当前为3.955ms，因此既有记录未显示同档位回退Trace退化。
前后独立运行、未锁频，不据此推导精确优化收益或Debug整帧FPS。

当前一次世界命中除主射线外，还可能对8个raw探针各执行一次完整几何可见性查询；缺失、禁用、零权重或背面会提前退出。
距离场仅辅助安全空域跳步，表面格仍经过DDA/候选BVH/精确三角形与alpha检查，短射线仍有格内固定成本。
主程序还独立执行探针更新及诊断滤波，冻结RAW的渲染计时未覆盖这些阶段；未取得整帧分项证据前，不能把用户观察全部归因于某项。
当前GI shader在Debug保持O3，主程序仅显式启用D3D12 debug layer，没有显式启用GBV。
报告：`tmp/VXGIStage75-stage76-debug-factor{2,8}-1707x1067-report.json`，配套输入哈希清单同目录。

## 阶段7.7：世界命中后直接消费探针缓存

按用户确认的简化流程执行：

```text
原SSGI屏幕追踪
  命中：返回原bounceSource
  失败：同一有限射线变换到世界坐标并求交
          命中：读取邻近探针缓存 → 三线性插值 → 方向SH求值 → 返回GI
          未命中：返回零
```

`VXGIProbeQueryRadiance` 现在只接收世界位置、原射线方向、探针间距/数量、格子索引与raw SH缓存。
它没有几何、材质、法线、探针中心或追踪资源参数，也不调用任何可见性或求交函数。
最多读取八角点，跳过缺失、禁用、未更新及非有限SH，按有效三线性权重归一化；四个RGB系数插值后按原方向求值，最后截断负值。
有效黑色缓存仍保留支持，无支持的世界几何命中仍以黑色终止。

原world主射线、alpha覆盖、有限长度、屏幕命中优先和接收材质响应只乘一次的规则保持原值。
本轮不修改factor2、quality1、radius0.5、探针更新预算和debug view。
缓存采光/禁用分类与邻间遮挡仍在其原阶段处理；六邻滤波继续作为既有诊断，生产消费的仍是raw缓存。
旧 `VXGIProbeQuerySurface` 的严格可见性只保留给辐照度诊断，不属于生产回退。
移除消费时的几何验证后，不再承诺旧逐贡献者查询的薄墙隔离规则；新方向查询测试验证直接读取语义。

### 阶段7.7后台验证

- Debug/Release主工程构建及编辑器架构检查通过；项目XML和diff空白检查通过。SM6.6 Debug/O3共8个shader变体通过，96B/608B布局保持。
- 查询GPU Debug/O2各16组：881条旧过滤记录、19次旧辐照度查询、28次直接方向查询。
  方向最大误差 `1.1920929e-7`；替换整套几何/SDF、命中身份、法线和材质不改变缓存读取输出，解析稀疏权重与状态跳过均通过。
  DXIL资源绑定确认方向查询只含缓存/查询输入输出与常量，不含geometry/material/centers/sampler。
- 混合GPU Debug/O2各11组：22条射线比较、5个实际CS像素、12次production RDG提交、3槽和2次尺寸变化。
  新回归用小遮挡片挡住探针到表面的路径、保留原主射线畅通，仍读取8个缓存贡献者并返回，证明没有旧可见性或半球筛选。
  辐亮度最大误差 `4.76837158e-7`，全部5个kernel像素最大误差 `2.98023224e-8`，实际历史/合成最大误差 `0.000439465046`。
- 两套GPU回归均开启debug layer/GBV，警告/错误0，隐藏窗口、无Present。查询各26项、混合各74项输入哈希稳定，Debug/O2混合来源完全一致。

保持1707×1067、factor2(854×534)、quality1、radius0.5、相同相机与已收敛RAW，使用Debug shader策略离屏导出32帧。
[当前纯GI图，预览曝光4](<D:/项目/LearnDirectX/tmp/vxgi-stage75-performance/stage77-direct-cache-gi-1707x1067-camera0_mode2.png>)
已实际检查，输出HDR有限、天空RGB为0、深度与7.6对照逐位相同，D3D12警告/错误0。

性能尚未验收通过：首轮Trace median6.816ms、p95 10.825ms、max10.893ms；额外连续3轮相同射线序列的median为7.141/10.886/13.331ms，
背景计时明显波动，没有据此宣称精简后提速。重复捕获最终HDR逐位相同，测试间没有调整采样、质量、几何或缓存。
这些计时只含冻结RAW的离屏Trace，native harness为O2/MD加Debug shader策略，不含VS Debug CPU、探针更新、DLSS或编辑器整帧。
详细报告、重复采样与当前输入哈希见 [7.7验收汇总](<D:/项目/LearnDirectX/tmp/VXGIStage77-final-summary.json>)；功能契约通过，不代表无漏光或完整性能验收。

## 静止进图后低帧率的冷启动诊断

用户反馈约前10秒100FPS，随后160FPS，模型保持静止。本次只增加后台测试工具选项，没有改生产渲染或配置。
`RunVXGIStage75Performance.ps1 -ColdStart` 跳过探针预热/RAW导入，每帧执行正式场景版本检查、探针更新、邻边滤波与真实光栅/SSGI，
记录CPU分项、几何revision/重建计数、reset/epoch、批次和对齐的GPU时间。`-NativeDebug -DebugShaderPolicy` 使用原生 `/Od /Zi /MDd /D_DEBUG` 及Debug依赖；默认仍为O2/MD。

分别采样O2 host与原生Debug host各1280个静态帧，均无可见窗口、无Present，debug layer开启、GBV关闭，D3D12警告/错误0。

| 初次CPU准备 | O2 host | 原生Debug host |
| --- | ---: | ---: |
| World/表面格子构建 | 165.22ms | 1725.33ms |
| SDF/候选加速结构/距离烘焙 | 161.74ms | 1722.35ms |
| Probe topology | 11.41ms | 52.62ms |

两轮各执行1281次场景准备检查，World/SDF都只在初次准备重建一次；随后1280帧无额外重建，SDF几何revision保持1。
原生Debug稳态SDF检查median0.0006ms、max0.0012ms；epoch保持1，只有第一帧发出分类reset。
原生Debug前两项合计约3.45秒，加topology/surface后约3.50秒，只是这几项同步CPU准备，不含更早的资产、IBL、管线初始化，不能当作全部启动耗时。

53,306个探针按256/frame遍历，第一轮需要209提交帧，最后批次58个；首次全部分类的读回快照为frame224。
按用户报告的100FPS估算，209帧约2.09秒，不能独自解释10秒低帧阶段。分类与首次邻边查询后会早退，SH采光仍持续循环更新；32次历史窗口不是停止条件。
本轮原生Debug renderer GPU分段中位约3.65ms，没有复现对应十秒的阶段跳变；不能把这个GPU范围取倒数当主程序FPS。

初次CPU准备在本测试的frame0之前计时，主程序在第一次Update同步执行。测试帧逐帧等待fence，不含主程序的DLSS、编辑器、后处理、透明、Present，且未挂VS调试器。
原生Debug1280帧CPU wall合计约9.39秒，末帧导出图像约0.89秒属于诊断；不能把它当运行卡顿，也不能宣称覆盖了实际主程序完整10秒转折。
当前证据支持一次性构建停顿，不支持静止时持续烘SDF；用户报告的完整100→160FPS变化仍未定位，未作生产性能修复。

原始运行清单因Windows PowerShell解码含中文路径的MSVC依赖JSON失败，仅记录8项基础输入；现已显式使用UTF8修复后续构建。
原生Debug运行所用runner快照已归档；另从实际编译依赖和shader引用补充106项当前输入，其中104项生产输入与7.7记录一致。
这份补充清单不能追补原运行缺失的完整前后哈希覆盖，具体限制见 [冷启动诊断汇总](<D:/项目/LearnDirectX/tmp/VXGIStartup-stage78-summary.json>)。

```powershell
.\Tests\RunVXGIStage75Performance.ps1 -NativeDebug -DebugShaderPolicy -ColdStart -Width 1707 -Height 1067 -Camera 0 -Mode 2 -Samples 1280 -DownsampleFactor 2 -Label startup-debug
```

## Mesh SDF 迁移第 1 步：静态局部资产与内存共享

新增 `Resources/VXGIMeshSDF.h`，提供 `VXGIMeshSDFCache::GetOrCreate(model, settings)`，返回 `shared_ptr<const Asset>`。
缓存键是资源 owner 生命周期内稳定的模型地址与距离场 cell size/分辨率上限；不扫描几何内容变化，不提供动态几何更新、失效或重建接口。
目前只做进程内缓存，没有磁盘缓存。cache 应早于模型资源 owner 析构；返回的资产深拷贝几何和距离数据，可由共享引用独立持有。

每份资产收集全部 submesh 的 LOD0 完整三角形，保留模型原有局部位置、UV、TBN、meshIndex/sourceTriangleIndex/sourceVertexIndices。
导入节点变换已经包含在模型顶点中，不重复应用实例 TRS；实例 ID、实例 Cutout 和材质覆盖不进入共享几何，materialId 留为 UINT32_MAX，后续实例层按 meshIndex 解析。
坏索引、非有限位置和退化三角形沿用现有几何筛选语义；局部网格沿用一格 padding、保守无符号节点距离、完整候选 CSR 与密集格 BVH。

`VXGISDF::BuildGeometry` 复用旧 Global 构建顺序，资产的一次性初始化只对 friend cache 开放。全局 Update 的静止复用、材质刷新和失败提交规则保留。
`Core/D3D12App.h` 仅增加空缓存 owner；生产没有调用 GetOrCreate，没有新增加载烘焙、GPU 上传或追踪切换。屏幕失败后的消费流程及所有 shader/配置未改。
实例包围盒、射线空间换算和世界最近命中属于下一步；探针继续使用现有世界空间布局。

后台验证结果：

- `RunVXGIMeshSDFTests.ps1` 的 Debug/O2 两档均通过 6 组 CPU 测试，使用 `/W4 /WX`；覆盖解析局部距离及保守节点、多 submesh/LOD0、UV/TBN/来源、共享与实例隔离、设置键/失败事务、空/坏几何、只读资产深拷贝寿命。
- 原 `VXGISDFTests.cpp` Debug/O2 各 4 组通过，每轮含 700 次随机 BVH/grid 对照；原 `RunVXGITraceAccelerationTests.ps1` O2 5 组通过，含 2400 条射线的保守候选验证。
- 主工程 Debug/Release 构建和 Editor UI architecture 检查通过；新模块未产生编译警告，主工程仍有原有类型转换、编码和第三方 PDB 等警告。
- 工程 XML、runner 语法及 diff 空白检查通过。本轮没有启动主程序、操作桌面或运行 GPU/图像测试，不作帧率与最终画质结论。

```powershell
.\Tests\RunVXGIMeshSDFTests.ps1
.\Tests\RunVXGIMeshSDFTests.ps1 -DebugBuild
```

## Mesh SDF 迁移第 2 步：实例表、局部射线与精确几何参考

新增 `Resources/VXGIMeshSDFScene.h`。`UpdateInstances` 按稳定实例 ID 组织记录，保存只读资产引用、TRS/正逆矩阵、世界包围盒、
来源索引、Cutout 标记和按 submesh 解析的实例材质映射。透明实例不参与，屏幕可见性和渲染 LOD 不改变 GI 几何。
重复更新不改变 revision，材质/排序变化复用矩阵，TRS 变化只重算该实例矩阵和包围盒；资产缓存不重烘。不增加动态几何编辑接口。

`MakeLocalRay` 保留原世界射线的原点和有限区间，只做逆变换；局部方向归一化后，以 `k=length(worldUnitDirection * inverseWorld)`
换算局部 `[worldMin*k, worldMax*k]`，同时返回 k。非均匀与负缩放按同一规则处理，不重新锚定起点或增加射线长度。

`TraceGeometry` 是供第 3 步 GPU 迁移对照的独立 CPU 几何参考，**不执行局部 DDA/距离场跳空，也不评估 Alpha 贴图**。
它先筛实例世界包围盒，再遍历资产已经保留的局部 BVH；每个访问节点临时转换成保守世界 AABB，叶子按旧 World 相同的 XM 顶点变换做 double 三角形求交。
只扩大候选包围盒的浮点舍入界，不扩大原世界射线区间；所有候选以 double 世界距离裁决，最终输出才转 float。
镜像后的几何法线由世界顶点绕序计算，着色 TBN 的 inverse-transpose 矩阵单独保留。只在查询时转换候选数据，不构建第二套持久世界三角形。

开发时实际复现并修正了局部 float 首命中方案的三个问题：缩放端点回算越界、rayMin 推进舍入越过包含表面、首候选被世界区间拒绝后漏掉同资产后续表面；
另保留远起点下两个世界距离转 float 相同仍须选择真正最近实例的回归。未来 GPU 实现必须与这些有限区间案例对照，不能直接把局部首命中当最终世界命中。

第 2 步后台验证：Debug/O2 各 6 组 CPU 测试通过，含 1072 次通用世界 BVH 对照、局部原点/区间端点往返、AABB 空域筛选、缩放/镜像/法线/身份、
跨实例最近命中、上述定向数值回归、TRS 资产复用及失败事务。测试采用 `/W4 /WX`，未放宽既有对照容差。
最终主工程 Debug/Release 构建和 Editor UI architecture 检查通过；工程 XML、runner 语法和 diff 空白检查通过。主工程仍有原有类型转换、编码和第三方 PDB 等警告。
本步主程序只新增空 scene 成员，未调用 UpdateInstances 或进行新 GPU 上传；生产仍是 Global 路径，尚不作 GPU 耗时、Alpha coverage 与最终画质结论。

```powershell
.\Tests\RunVXGIMeshSDFSceneTests.ps1
.\Tests\RunVXGIMeshSDFSceneTests.ps1 -DebugBuild
```

## Mesh SDF 迁移第 3 步：GPU 共享资产与生产追踪切换

主程序现在调用 `VXGIMeshSDFScene::UpdateInstances`，不再调用 `VXGISDF::Update(world)` 烘焙全局距离场。
固定世界探针、稀疏分配和历史调度保持原有流程；移动模型仍可能刷新这些世界空间数据，但不会重烘模型局部 SDF。
`VXGIMeshSDFGPUData.h` 将唯一资产的局部距离节点、候选 offsets、原始索引的 packed references 和 208B 局部三角形拼接上传，
每资产用 64B 记录保存网格和拼接基址；每实例用 240B 记录保存三个变换矩阵、世界 AABB、材质映射范围、Cutout 和稳定实例 ID。
资产相对 CSR/BVH 索引不重写；多个实例共享资产切片。`VXGIGPUScene` 每个 frame slot 在 fence 完成后才替换资源或释放 staging。
同一资产集合下 TRS 修改仅上传实例记录，材质映射单独比较/上传；静止帧复用资源。初次使用仍需为各 frame slot 准备 GPU 副本。

生产 Probe/Filter/SSGI 和 SDF 诊断用 `VXGI_MESH_SDF=1` 编译，共享三个 space1 SRV：资产、实例、实例材质 ID。
根 StructuredBuffer SRV 没有 NumElements 描述信息，实例数量明确存入记录的 `sceneInstanceCount`，不调用 GetDimensions 猜测。
Probe/Filter 的新增绑定均在原有参数之后；SDF 诊断将根常量改为逐帧 CBV，从而给新增 SRV 留出根签名空间。
旧 Global shader/Prepare 和 Probe/Filter 默认初始化方式仅保留给历史独立测试；主程序明确选择 Mesh backend，没有按耗时切换后端的新机制。

求交先直接遍历实例世界 AABB，再逆变换原射线进入该资产局部网格，执行 DDA、保守无符号距离跳空、CSR/密集格无栈 BVH。
对实际候选临时变换三角形，在原始世界有限区间内求交，先判断 Alpha 覆盖，再按世界距离选择最近命中；实例 Cutout、材质覆盖和镜像法线均在实例层恢复。
局部遍历的浮点舍入界仅用于保守收集候选，不放宽世界射线的 rayMin/rayMax。最终世界三角形复核/距离裁决使用 FP64，要求 DoublePrecisionFloatShaderOps；生产 SSGI 初始化显式检查支持。

SSGI 屏幕失败后继续保持原契约：原有限射线转换世界坐标 → Mesh SDF 求交 → 在世界命中点读取现有 probe radiance → 返回。
没有追加屏幕追踪、延长射线、现场直接光或 probe 查询可见性射线。探针更新阶段原有的材质采样/太阳源光和邻居遮挡也切换到 Mesh 后端。
诊断射线共用同一后端，距离切片遍历实例采局部场；编辑器现有统计行改读唯一局部资产之和，Node dimensions 显示各轴最大值，不代表另有全局网格。
未改变编辑器控件、布局或样式，未操作桌面或做 UI 外观验收。

定向后台验证：

- `RunVXGIMeshSDFGPUDataTests.ps1` Debug/O2 通过：唯一资产、原索引拼接、共享/中性属性、实例材质、TRS 后资产字节不变、范围与空场景；采用 `/W4 /WX`。
- `RunVXGIMeshSDFGPUValidation.ps1` Debug/O2 各 11 组、1457 条实际 GPU 射线通过，含平移/旋转/非均匀/负缩放共享实例、严格端点、零长度包含区间、拒绝过近候选后继续、距离输出同 float bin 的真正最近实例、Alpha 洞/阈值/非 Cutout、几何重载、密集格 BVH 与原 CSR。
- 该 GPU fixture 使用生产打包器与 shader，独立 CPU `TraceGeometry` 作几何参考；最大误差 `2.38418579e-6`。RTX 5070 Laptop、D3D12 debug layer/GBV、无 HWND/Present，warning/error 0。
- GPU fixture 自己创建缓冲区，不单独证明生产 `VXGIGPUScene` 的跨帧 fence 与上传计数；实际 Pass 集成检查另记。这里不作全场稳态 FPS 或最终画质结论。

实际 Pass 集成检查也已通过，报告分别为 `tmp/VXGIProbeQueryGPU-mesh-step3-renderer-smoke-O2-report.json` 和
`tmp/VXGIStage75-mesh-sdf-step3-smoke-64x40-report.json`，各附稳定源码哈希：

- 原 Query O2 runner 16 组通过，实际创建了新的 SDF 诊断 CBV 根签名及 Mesh VS/PS PSO，D3D12 debug/GBV warning/error 0。窗口隐藏，Present 0。
- Stage75 harness 已迁移到当前 Mesh backend。真实场景 2 资产、2 实例、129386 三角形、53306 探针，执行 6 批生产 Probe/Filter，再以 64×40、camera0、各 1 帧运行 GI 关闭和模式 0/1/2，4 张 HDR 全为有限值，debug/GBV warning/error 0。
- 三个 frame slot 均预热后，对一个实例临时平移 X=0.125 并还原；各 slot 的实例上传增加 2 次，资产烘焙、资产上传和材质映射上传均增加 0 次。各 slot 最终为 1 次资产上传、3 次实例上传、1 次材质映射上传。
- 这次仅执行 1536/53306 的探针批次工作，缓存未完整初始化，用于功能和上传复用检查；不是全场质量或性能验收。所有运行没有可见窗口或 Present，没有使用桌面操作。
- 最终主工程 Debug/Release 构建、Editor UI architecture、工程 XML、runner 语法和 diff 空白检查通过。主工程仍有原有类型转换、编码和第三方 PDB 警告。

复现小规模实际 Pass 检查：

```powershell
.\Tests\RunVXGIStage75Performance.ps1 -Width 64 -Height 40 -Camera 0 -Samples 1 -WarmupBatches 6 -VerifyInstanceUpdates -GpuValidation -Label mesh-sdf-step3-smoke
```

```powershell
.\Tests\RunVXGIMeshSDFGPUDataTests.ps1
.\Tests\RunVXGIMeshSDFGPUDataTests.ps1 -DebugBuild
.\Tests\RunVXGIMeshSDFGPUValidation.ps1
.\Tests\RunVXGIMeshSDFGPUValidation.ps1 -DebugBuild
```

## Mesh SDF 迁移第 4 步：完整离屏验收与剔除优化

本轮结论是**功能通过，静态追踪性能仍有回退**。全程后台运行，无可见窗口、桌面操作或 Present。
实际配置保持 `quality=1`、`radius=0.5`、`trace_downsample_factor=2`、探针间距 0.5、局部 SDF cell size 1/max axis 128。
测试显卡为 RTX 5070 Laptop GPU。生产代码没有新增按 GPU 时间切换算法、延长射线、现场直接光或动态几何重建接口。

验收发现初版 Mesh 对大量无关候选做世界空间 FP64 求交，且实例包围盒包含整个 SDF padding。保留精确命中裁决，增加三层廉价剔除：

1. 资产一次计算保守几何包围盒，实例使用它而非 padded lattice 做世界 AABB。边界包含与候选 BVH 相同的重心容差、浮点与 cell margin；局部 SDF 节点/网格不变。
2. 密集 BVH 叶子内先测试每个三角形的保守局部 AABB，再做三顶点世界变换和 FP64 三角形复核。
3. 实例求交前先检查原世界有限线段 AABB 的必要重叠，按运算规模扩展浮点舍入界。通过者继续原 FP64 slab/三角形求交；不改变 rayMin/rayMax 或最近命中规则。

CPU 新回归证明地面朝上的短射线直接被实例盒排除，局部追踪/三角形测试次数为 0，向下射线仍命中。
资产、实例 Debug/O2 各 7 组通过，后者包含 1072 次世界参考对照；GPU 合成 Debug/O2 各 11 组、1457 条严格端点/Alpha/最近交点射线通过。
真实 GLB 场景独立世界 BVH 参考覆盖原场景 16384、变换/镜像 8192、1e4/1e5 远原点 4096 条，共 28672 条 Mesh 射线，命中/距离/身份/状态差异均为 0。
远坐标参考使用 GPU 实际归一化后的浮点世界方向，避免 double 再归一化产生另一条空间直线；输出距离的 float ULP 误差单独记录。
该真实几何测试保留 Cutout 三角形，不评估材质 Alpha；覆盖逻辑由合成 Alpha 测试及实际材质渲染检查补充。所有 correctness 运行 debug/GBV warning/error 为 0。

### 完整缓存与画面

最终生产 Probe/Filter 执行 32 轮共 6688 批，另 3 批用于排空异步时间戳，总计 6691 批。
53306 个记录全部完成分类：27181 valid、26125 disabled（26123 backface、2 unsafe origin），unclassified/trace failure/sample failure/invalid SH 均为 0。
全部 valid 至少更新 32 次（26692 个为 32、489 个为 33），26331 个记录有非零 SH；有效黑色缓存不视为追踪失败。
32 是滚动均值窗口上限，之后仍继续射线采样，不是达到 32 次就停止工作。

冻结同一 RAW，输出四个静态相机的 640×400 纯 GI 与合成图，各模式每视角 12 帧；GI 关闭只在合成检查中使用。
优化前后 16 张合成 HDR、16 张深度全部逐字节一致；主分辨率前后 HDR/深度和 32 轮新旧 RAW 也完全一致。
RAW SHA256 为 `A0C9D93B81609EB6826997B0FD19E06CDC26DB87C2EE8BBBE8F4CEE229686722`。
纯 GI 预览使用固定显示曝光 ×4、Reinhard+sRGB，线性 HDR 不变；文件见 `tmp/vxgi-stage75-performance/mesh-sdf-step4-gi-final-640x400-camera{0..3}_mode{0..2}.png`。

实际图像中，回退在车门框/后部暗区提供了补充；光照仍然宽而平滑，没有因换 Mesh SDF 获得高频细节。
前三个视角的前景约 17.5%–19.6% 像素相对 screen-only 有变化；汽车出屏的相机 3 仅约 0.25%。这是差异比例，不是正确率或探针支持率。
有限半径 0.5、4 个 SH 系数、探针间距和现有降噪共同限制了细节与远距离补充。没有 ground truth，也没有平滑相机移动的时域画质验收。
当前模式 1/2 都走 screen-first + unresolved world fallback，同一算法的分时测量差异不能解释成两种后端性能差异。

### 时间与内存

以下是同 RAW、camera0、1707×1067、factor2、生产 Debug shader 优化策略/O2 native host 的 GPU 时间。
初版运行 12 帧；后两版各重复 3 轮，每轮 12 帧、丢弃前三帧，保留 27 个稳态样本。CPU/GPU 其他负载和 GPU 频率未锁定。

| 当前 Mesh 实现 | Trace median | 整段 SSGI median |
| --- | ---: | ---: |
| 第 3 步初版 | 44.013ms | 44.556ms |
| 紧致实例盒 + 单三角形盒 | 14.569ms | 15.427ms |
| 再加世界线段必要重叠 | 7.233ms | 7.830ms |

最终整段 SSGI p95 为 15.711ms，仍有尖峰。另一轮同进程多模式测量 hybrid median/p95 为 8.725/13.907ms，screen-only median 为 1.100ms。
这说明初版回退已大幅减轻，不能据此声明整帧达到某个 FPS；计时排除 probe 更新、readback、DLSS、postprocess、UI 和 Present。
实际 Probe 256 个/批，已分类的第二轮 median/p95 为 0.212/1.772ms，最终第 32 轮约 0.215/1.764ms；Filter 已知邻边约 0.009ms。

另用**同样的 16384 条有限几何射线**对比 Global/Mesh kernel：关闭 GBV、Release O3、DEFAULT 输入只上传一次，2 次预热后交替顺序各测量 12 次。

| 几何追踪 kernel | median | p95 |
| --- | ---: | ---: |
| Global | 0.680ms | 0.999ms |
| Mesh | 1.671ms | 1.775ms |

这组命中较多的静态工作负载上 Mesh 耗时为 Global 的约 2.46 倍，两者结果都通过独立几何参考。
该 kernel 不含 Alpha、probe 查询、SSGI 或 raster，不能用它直接推算整帧差值，但明确不能宣称 Mesh 静态追踪更快。
转换和精确世界复核仍有开销；本轮保留严格命中语义，没有用放宽有限区间换时间。

相同场景、同设置的一次独立 O2 CPU 测量：Mesh 初始化约 180ms、Global 184ms，1000 次 Mesh TRS 更新约 0.46ms、资产重烘次数 0。
静态 GPU payload 为 Mesh 29,841,352B、Global 30,250,884B，当前只少约 1.4%；CPU SDF retained 估算 Mesh 46,104,320B、Global 45,391,804B。
当前只有两个模型/两个实例，尚未体现大量重复实例的资产共享收益。
最终三帧槽的全部 10 个 shared scene buffers 合计 DEFAULT payload 97,839,792B、committed allocation 99,287,040B；另有 Probe/readback 25,586,928B、Filter 5,330,648B 的逻辑缓冲统计。
这些值不是整个渲染器显存/物理驻留量，且资产仍各有三帧槽副本。单次 CPU 时间不是多轮稳定基准。

原生 Debug `/Od /Zi /MDd/_DEBUG` 冷启另测 256 帧：World 初建 1774.17ms、Mesh 两资产 1790.90ms、拓扑 56.74ms，共约 3.62s CPU 阶段。
之后全部 256 帧仅检查静止源，没有 World 或资产重建；首 209 帧完成第一轮分类。该次缓存仅 1–2 次更新，不用于上述收敛画质对比。
首 209/后 47 帧 Probe outer GPU median 分别为 0.340/0.191ms（批次位置不同，非匹配稳态 A/B），renderer median 9.776/9.600ms。
最后一帧包含 HDR readback/扫描导致约 1.1s CPU wall 异常，不在 GPU timer 内，不能换算主程序 FPS。未连接 VS 调试器，未包含 UI/DLSS/Present。

### 移动刷新与最终状态

新小场景使用实际 World/MeshScene/Topology/Surface/ProbeState、三帧槽 GPU Prepare、Probe/Filter wrapper：Debug/O2 各 6 组通过。
移动发光盒后，旧 SH/分类/邻边先全部清理，再按部分预算刷新；8 个共有固定探针改变分类，110 个改变源 SH，指定探针 disabled→valid、+X 邻边 blocked→visible。
旧 pending plan/epoch 被拒绝，实际 Poll 丢弃旧 GPU snapshot；三个帧槽复用资产与材质映射，只更新实例。还原后分类/邻边完全恢复，SH 最大差 0。
这证明刷新机制，没有把离散测试称为真实汽车连续拖动的平滑画质验收。
最终 Debug/Release 主构建、Editor UI architecture、工程 XML、runner 语法和 diff 空白检查通过；保留原有转换/编码/第三方 PDB 警告。

主要证据：

- `tmp/VXGI-MeshSDF-step4-acceptance-metrics.json`：实际渲染、RAW、时间、内存和图像一致性汇总。
- `tmp/VXGIMeshSDFAcceptance-final-guard-report.json`：28672 条真实几何参考对照。
- `tmp/VXGIMeshSDFAcceptance-GPUBenchmark-report.json`：同射线 Global/Mesh 静态计时。
- `tmp/VXGIMeshSDFGPUValidation-{O2,Debug}-report.json`：1457 条合成几何/Alpha 回归。
- `tmp/VXGIMeshSDFEditGPU-{O2,Debug}-report.json`：移动、历史和邻边刷新。
- `tmp/VXGIMeshSDFAcceptance-final-summary.json`、`tmp/VXGIMeshSDFAcceptance-modes-summary.json`：离线 HDR/逐 pass 统计。

复现命令（使用 runner 输出的 RAW，避免使用已覆盖的历史快照）：

```powershell
.\Tests\RunVXGIMeshSDFAcceptance.ps1 -Transforms
.\Tests\RunVXGIMeshSDFAcceptance.ps1 -Benchmark
.\Tests\RunVXGIMeshSDFEditGPUTests.ps1
.\Tests\RunVXGIMeshSDFEditGPUTests.ps1 -DebugBuild
.\Tests\RunVXGIStage75Performance.ps1 -DebugShaderPolicy -WarmupRounds 32 -AllCameras -GIOnly -Samples 12 -Label mesh-sdf-step4-gi-final
.\Tests\RunVXGIStage75Performance.ps1 -NativeDebug -DebugShaderPolicy -ColdStart -Width 1707 -Height 1067 -Camera 0 -Mode 2 -Samples 256 -Label mesh-sdf-step4-native-debug-cold
```

## 距离阈值方案第 1 步：模型实例 BVH 与最近两个候选

后续 SSGI 消费路径改为：原有限世界射线 → 模型 AABB BVH → 最近两个模型 → 局部 SDF 达到距离阈值 → 世界命中位置采探针 → 返回。
本步先建立候选阶段，不改变当前 GPU 求交或渲染结果。探针的独立更新流程继续保留，未修改配置或编辑器控件。

新增 `Resources/VXGIInstanceBVH.h`，对现有保守模型几何 AABB 建树；不持有三角形、距离场或模型资产。
节点固定 32B：`float3 minimum + uint instanceIndex + float3 maximum + uint escapeIndex`，内节点的 instanceIndex 为 UINT32_MAX。
前序布局、每叶一个紧凑实例索引，以 escapeIndex 跳过整棵子树；节点数量为非空实例数的 2N-1，可供后续 GPU 上传。
使用包围盒中心最长轴做稳定中位数划分。模型移动只自底向上 refit 边界；成员/索引身份变化重建树；不变和仅材质修改无分配、不改变 BVH revision。
模型 SDF 仍是只读共享资产，新增的是实例层的数据，不提供运行时几何修改/重烘接口。

`VXGIMeshSDFScene::UpdateInstances` 在新实例表完整准备好后更新 BVH，成功后移动提交实例表；失败保留旧场景和旧 BVH。
`GetInstanceBVH` 提供节点、统计和 revision；`QueryClosestTwoInstances(origin,direction,rayMin,rayMax)` 提供独立候选查询入口。
它按归一化原世界射线的闭区间查询，候选 entry/exit 均裁剪到原区间，不延长射线或重锚定原点。
按 AABB 进入距离排序，同距依次比较实例 ID/紧凑索引；原点已在盒内时 entry 为 rayMin，支持零长度区间、平行方向和退化平面盒。
最多保留两个候选；有两个结果后仅剪掉 entry 严格大于第二名的节点，保留同距排序机会。查询不分配内存，不访问 SDF、三角形或材质。

这里只判断包围盒候选，不能把返回的 entryDistance 当作表面命中。只取两个包围盒可能漏掉第三个模型中的有效表面，是下一阶段明确采用的近似预算。
当前沿用模型几何的保守边界；本步曾将阈值与扩盒的关系留到第 2 步定义。第 2 步确定不额外扩盒，仍以紧模型 AABB 为查询支持域。
CPU 几何参考用 double 计算区间，以验证候选语义；本步没有新增任何 GPU FP64 操作，后续 GPU 候选/距离步进按轻量 FP32 设计。

后台验证：

- `RunVXGIInstanceBVHTests.ps1` Debug/O2 均 `/W4 /WX` 通过 7 组：1924 次独立有限射线/AABB 暴力对照、4096 次零分配查询；
  包括进入距离与中心距离不同、最近两者/稳定同距、盒内起点、严格端点、平行/退化盒、静止/材质复用、refit/rebuild、失败回滚及 1024 盒剪枝。
- `RunVXGIMeshSDFSceneTests.ps1` Debug/O2 各 8 组，1072 次原世界几何对照仍通过；新增 Scene 集成检查排序/材质不更新 BVH、移动 refit 且不重烘、实例删除/透明过滤后索引正确、失败保留旧查询。
- 主工程 Debug/Release 构建、Editor UI architecture、工程 XML、runner 语法及 diff/新增文件空白检查均通过；主工程保留原有转换、编码和第三方 PDB 警告。
- 尚未上传 BVH 节点、切换 GPU 候选或实现 SDF 阈值命中；本步没有运行主程序、GPU/图像基准或操作桌面，不宣称当前帧率改善。

```powershell
.\Tests\RunVXGIInstanceBVHTests.ps1
.\Tests\RunVXGIInstanceBVHTests.ps1 -DebugBuild
.\Tests\RunVXGIMeshSDFSceneTests.ps1
.\Tests\RunVXGIMeshSDFSceneTests.ps1 -DebugBuild
```

## 距离阈值方案第 2 步：直接采样与近似命中

新增 `Resources/VXGISDFThreshold.h` 与独立的 `Shaders/VXGISDFThresholdCommon.hlsli`，复用已有资产 unsigned 距离节点，不改变烘焙、资产大小或 Pipeline 参数。
这是供下一步 SSGI 使用的独立查询；本步没有更换生产 SSGI、探针更新或滤波路径，也没有上传实例 BVH。

### 采样和步进约定

- 每次采样读取当前 cell 的八个节点并做三线性插值。插值距离 `d <= 0.9h` 即接受近似命中，`h` 为资产实际局部格距。
  默认步进比例 0.9，最多 512 次采样；参数结构 16B，网格描述 48B，CPU/HLSL 一致，GPU 节点数量显式传入。
- 现有节点是无符号场：薄面落在节点之间时，表面上的插值距离仍可能大于零，插值不确定性最大约 `sqrt(3)/2 × h`。
  所以默认阈值按格距设置，不能把它宣称为精确 SDF 求交；当前配置 `cell_size=1` 的近似也较粗，实例缩放会放大世界误差。
  烘焙节点已经留有保守舍入余量，接受距离并非真实三角形距离的严格上界。
- 保守下界只决定安全前进：`u = h * sqrt(sum(f * (1-f)))`，其中 `f` 是 cell 内分数坐标；
  步长为 `0.9 * max(0, d-u-roundoff)`。不把下界为零当作命中，也没有随意加最小步长跨过薄面。
- 原点不重新锚定，方向用 FP32 稳健归一化，查询区间有限且闭合；零长度区间采一次，起点已在阈值内立即命中。
  先对局部场做 slab 裁剪；公开点采样拒绝域外点，步进仅对已裁剪点的微小边界舍入作位置钳制，返回位置仍在原射线上。
- 候选采用第 1 步的紧模型几何 AABB，不随阈值扩盒。射线没有碰到模型盒就不尝试该模型；这是明确的支持域取舍，
  避免 padded 网格及阈值外壳把离开地面的射线也当作接收面命中。局部步进同时受原区间、候选 entry/exit 和 SDF 域限制。
- 状态区分 Hit、Miss、Invalid、Exhausted、Stalled。达到采样预算或浮点无法前进不伪装成已确认 miss。
  参数要求阈值大于约 0.8660254 格；非常接近这个下限时，舍入余量仍可能使步长停滞，默认 0.9 留有间隔。

### 跨实例 CPU 参考

`Resources/VXGIMeshSDFThreshold.h` 组合现有最近两个 AABB 查询与上述节点查询，每候选只做一次局部变换。
令逆变换后的世界单位方向长度为 `k`，则局部查询区间为世界 entry/exit 乘 `k`，返回局部单位距离除以 `k` 后比较世界距离。
世界命中位置用原世界射线计算并保持原有限区间，覆盖旋转、非均匀缩放与镜像。
若较近候选耗尽或停滞，不能把另一个较远命中称为已确认最近命中；该参考显式返回未完成状态。

新查询只返回近似位置、距离、实例身份和采样状态，不提供三角形身份、UV、真实法线、正反面或 Alpha 覆盖。
没有三角形候选表、局部三角形 BVH、逐三角形世界变换、FP64 精确求交或现场光照；探针源采集仍使用原独立路径。
最近两个 AABB 的预算保持不变，两个空框可挡住第三模型的查询资格，这是可复现的预期近似漏查，不补做全场精确搜索。

后台验证结果：

- CPU 距离查询 Debug/O2 均 `/W4 /WX` 通过 6 组，包含 512 次独立 double 插值参考对照、524 次有限命中；
  覆盖非节点薄面、保守下界为零但插值不命中、点查询域外拒绝、零区间、端点/阈值等号、无效数据、预算耗尽和停滞，以及真实烘焙开面/闭盒。
- Scene Debug/O2 均通过 10 组，保留 1072 次原世界几何对照，新增空框/第三候选预算、有限区间不扩展、零宽表面盒、AABB miss 零采样、
  旋转/非均匀/镜像缩放、跨模型世界最近距离、较早盒子内较远表面、未完成近候选状态传播。
- 独立 D3D12 GPU fixture 在 RTX 5070 Laptop 上 Debug/O2 各通过 1807 条 CPU/GPU 查询，状态计数均为 Miss 158、Hit 1642、Invalid 5、Exhausted 1、Stalled 1；
  最大数值误差 `7.1525574e-7`。使用 debug layer 与 GBV，warning/error 0；没有 HWND、Present 或桌面操作。
  DXIL 检查无实际 double/`.f64` 指令；反射资源严格为 TestConstants、nodes、rays、results，无旧几何或材质资源。
  反汇编文件为 `tmp/VXGISDFThresholdValidate-O2.ll` 和 `tmp/VXGISDFThresholdValidate-Debug.ll`。
- 主工程 Debug/Release、Editor UI architecture、工程 XML、runner 语法及差异/新增源文件空白检查通过。
  本步没有切换生产消费端、输出新画面或计时，不据此推断帧率提升。下一步上传实例 BVH 并接入 SSGI。

复现命令：

```powershell
.\Tests\RunVXGISDFThresholdTests.ps1
.\Tests\RunVXGISDFThresholdTests.ps1 -DebugBuild
.\Tests\RunVXGIMeshSDFSceneTests.ps1
.\Tests\RunVXGIMeshSDFSceneTests.ps1 -DebugBuild
.\Tests\RunVXGISDFThresholdGPUValidation.ps1
.\Tests\RunVXGISDFThresholdGPUValidation.ps1 -DebugBuild
```

## 距离阈值方案第 3 步：生产 SSGI 接入

生产 SSGI 现在使用新路径：屏幕查询成功时返回原屏幕源；失败时把原有限射线变换到世界空间，遍历实例 AABB BVH，
仅查询入口最近的两个模型的局部距离节点，达到阈值即接受近似表面，按世界距离取最近者，读取现有 raw 探针 SH 后返回。
建树/refit 位于实例更新阶段，不在每像素或每射线中重建 BVH。未更改 SSGI 原有法线偏移、半径、近裁剪限制或屏幕成功分支。

### GPU 数据与资源

- `VXGIGPUScene` 新增 slot 10 的 32B 实例 BVH 节点；原 0–9 槽、64B Asset 和 240B Instance 布局保留。
  BVH 按独立 revision 上传，静止或材质改变不重复上传，TRS 变更上传 refit 结果，成员改变上传重建结果。
  每个帧槽等待原有 fence 后准备，copy/staging 生命周期沿用现有规则；空场景有安全 dummy 节点，显式计数为 0。
- 节点/资产/实例/BVH 数量显式传入，避免 root SRV 的 GetDimensions 问题。
  `SSGIHybridParameters` 新增 16B 数量行为 112B，完整 trace b0 为 624B，前面矩阵和字段偏移保留。
- `VXGIMeshSDFThresholdCommon.hlsli` 以无栈前序 BVH/escapeIndex 遍历，按盒入口距离稳定保留两者；只在原有限区间内做 FP32 局部查询。
  每候选只做一次逆变换，局部归一化距离按方向变换长度换回世界距离；近处候选未完成时，不把较远命中当作已确认最近者。
- 生产 Trace 编译宏为 `SSGI_PASS_TRACE=1` 和 `VXGI_SSGI_THRESHOLD=1`；根签名精简为 b0 加六个固定 SRV，去掉旧材质 sampler。
  顺序为距离节点 t0/space0、探针 lookup t6/space0、raw SH t7/space0、资产 t0/space1、实例 t1/space1、BVH t3/space1。
  RDG 仅登记这六个 buffer 的依赖；原 SSGI 屏幕纹理和输出继续通过 bindless heap 使用。
- 从旧公共文件提取 `VXGIMeshSDFData.hlsli`、`VXGIProbeData.hlsli` 和 `VXGIProbeRadianceCommon.hlsli`；
  共享 Record、球谐基函数及 raw 八角点插值公式保持一致，新消费路径不 include 三角形、材质或完整探针采光代码。
  旧精确求交和配套缓冲仍供探针分类/采光/滤波及历史参考使用；没有将缓存更新改成这个近似位置查询。

### 命中与返回

默认仍为 `d <= 0.9 × localCellSize`，步进比例 0.9、每候选最多 512 次采样，候选支持域不额外扩张。
起点已在阈值内立即接受，没有新加逃离自身表面的搜索或重锚定；这沿用第 2 步的近似约定，格距和阈值导致的实际画质偏差留给第 4 步测量。
结果明确区分 miss/hit/invalid/exhausted/stalled；新命中没有真实 triangleIndex，诊断保留 UINT_MAX。

命中后只调用纯 `VXGIProbeQueryRadiance`，在世界命中位置对八角点已有 SH 插值，以原 GI 射线方向恢复辐亮度。
无三角形复核、候选 CSR、逐三角形世界变换、FP64、材质/Alpha 复核、直接光、阳光射线或探针可见性射线。
缓存纯黑是有效返回；缓存无支持也在该近似表面终止，不继续找更亮的后方模型。
当前固定探针分配、资产格距和 Pipeline JSON 参数未改变；没有加入自动帧时间降级策略。

### 后台验证

- `RunVXGIMeshSDFGPUDataTests.ps1` Debug/O2 `/W4 /WX`：BVH 节点逐字节打包、独立 revision、静止/材质复用、TRS refit、删除重建及空表 padding 通过。
- 新 `RunVXGIMeshSDFThresholdGPUValidation.ps1` Debug/O2 `/W4 /WX` 各通过 1549 条 CPU/GPU 世界查询：
  Miss 314、Hit 1231、Invalid 3、Exhausted 1；覆盖两个空框省略第三模型、端点、零长度、AABB miss 零采样、空场景，
  1536 条旋转/非均匀/镜像变换射线、跨模型世界最近距离和近处候选未完成。最大位置/距离/字段误差 `1.3828278e-5`。
  另各 6 个实际 `SSGIWorldRaySource` 案例验证缓存 RGB、有效黑色、禁用/缺失支持均终止、原射线短区间 miss 及非法方向。
  fixture 只暴露所需资源，D3D12 debug/GBV warning/error 0，没有 HWND 或 Present。
- 按生产实际宏重新执行 CPU-only DXC 审计，明确核对 624B b0 及六个固定 SRV 的名称、寄存器和 space，DXIL 无 FP64 或三角形/CSR/材质 buffer。
  初次 GPU fixture 带有未被新路径使用的旧 Mesh 宏，报告保留原测试来源哈希，并单独保存去掉该宏后的精确生产审计记录；不混淆两次证据来源。
- 生产 Probe/Filter 的三帧槽隐藏 fixture Debug/O2 各 11 组、31 次提交通过，含 GPU BVH 回读逐字节匹配、静止/材质不更新 BVH、TRS/成员/空场景更新及还原。
  原有分类、邻边、源 SH、旧 epoch/snapshot 拒绝检查保持通过，还原最大 SH 差 0，资产未重烘，D3D12/GBV warning/error 0，Present 0。
- SSGI 源历史状态 CPU Debug/O2 各 3 组通过；主 Debug/Release 构建通过，保留原有类型/转换/第三方 PDB 警告。
- 实际当前汽车/地面资产使用生产 Shadow/GBuffer/HBAO/Deferred/SSGIPass，64×40、相机 0、四模式（-1/0/1/2）各 2 帧通过；
  使用生产 Debug shader 策略、O2 native host 及 GBV，四张 HDR 均有限，D3D12 warning/error 0、窗口隐藏、Present 0。
  三帧槽均验证当前 BVH revision/count：2 实例、3 节点，每槽仅上传一次 96B BVH、一次不可变资产；显式距离节点数 6699。
  报告统计包含新增 BVH 的全部 11 个 shared buffer。仅预热 6 批探针，本次属于绑定/状态/执行检查，不是全场探针收敛、画质或性能验收。

主要证据：`tmp/VXGIMeshSDFThresholdGPU-{Debug,O2}-report.json`、对应日志，
`tmp/SSGIThreshold-production-{Debug,O2}-audit.log` 与 `.ll`，以及 `tmp/VXGIMeshSDFEditGPU-{Debug,O2}-report.json` 和输入哈希。
实际生产流程记录为 `tmp/VXGIStage75-threshold-step3-rdg-final-64x40-report.json`、对应输入哈希和 `tmp/VXGI-Threshold-step3-RDG-final.log`。
本步只验证接入和有限样本行为；最终画质、阈值自命中比例和匹配当前配置的性能对比属于第 4 步。

```powershell
.\Tests\RunVXGIMeshSDFGPUDataTests.ps1
.\Tests\RunVXGIMeshSDFGPUDataTests.ps1 -DebugBuild
.\Tests\RunVXGIMeshSDFThresholdGPUValidation.ps1
.\Tests\RunVXGIMeshSDFThresholdGPUValidation.ps1 -DebugBuild
.\Tests\RunVXGIMeshSDFEditGPUTests.ps1
.\Tests\RunVXGIMeshSDFEditGPUTests.ps1 -DebugBuild
.\Tests\RunVXGIStage75Performance.ps1 -DebugShaderPolicy -Width 64 -Height 40 -Camera 0 -Samples 2 -WarmupBatches 6 -GpuValidation -Label threshold-step3-rdg-final
```

## 距离阈值方案第 4 步：实际场景、纯 GI 与性能验收

测量完成。新查询的实现不变量和性能改善已验证，但当前 `cell_size=1` / `0.9h` 的质量风险明确：
车体表面向外的几何诊断中，4078/4096 条射线在起点接受同一车体；新纯 GI 图的大片车身补光也明显更强、更均匀。
因此不能把本轮称为“与精确求交等价的性能优化”，也不能把更亮的画面直接判为质量提升。
本步没有调整 SDF 分辨率、阈值、射线偏移、半径、探针或默认后端；起点阈值内如何处理留作后续设计决定。

### 对照条件与缓存

- RTX 5070 Laptop；两个当前配置 GLB 实例、129386 个世界三角形，当前 SSGI quality 1、4 rays、radius 0.5、trace factor 2，探针 voxel 0.5、资产局部 SDF cell 1。
- 两版均使用真实 NativeDebug `/Od /Zi /MDd /D_DEBUG` 及匹配 Debug 原生依赖，GI shader 按现有生产 Debug 策略保留符号并优化。
  没有连接 VS 调试器。精确参考只由验收 runner 的 `-ExactFallback` / `VXGI_ACCEPTANCE_EXACT_FALLBACK` 编译启用，
  恢复旧 SSGI 根绑定与旧 Mesh CSR/三角形 BVH/FP64 求交，再查询同一 raw SH。默认主程序和普通 runner 仍使用新阈值路径。
  两种验收二进制、依赖/策略记录及 SHA256 分开保存；SkipBuild 会核对原生配置、宏、源码和二进制哈希。
- 复用已充分预热的 `tmp/vxgi-stage75-performance/mesh-sdf-step4-gi-final-640x400-raw.bin`，SHA256 为
  `A0C9D93B81609EB6826997B0FD19E06CDC26DB87C2EE8BBBE8F4CEE229686722`。
  场景、Pipeline/Lighting JSON、GLB、HDR 与生成该 RAW 时一致；本轮不重做探针采光。
  53306 个记录全分类：27181 valid、26125 disabled；有效记录最低 32 次更新，26692 个为 32 次、489 个为 33 次；26331 个含非零 SH，最大绝对系数 3.73074。
  导入后检查分类/有限数值，GPU RAW 在所有 captures 前后逐字节不变。旧精确结果是工程对照，并非路径追踪 ground truth。

### GPU 时间

相机 0、1707×1067、mode 2，在新/旧/旧/新顺序下分别运行两个 batch。
每 batch 为两条各 12 帧的序列，每序列丢弃前 3 帧；每后端共 48 帧、保留 36 帧计入下表。
两批的 Trace 中位数分别为旧 7.112/7.043ms、新 0.617/0.617ms，结论不依赖单批结果。

| GPU 范围 | 旧精确 median / p95 | 新阈值 median / p95 | 中位耗时变化 |
| --- | ---: | ---: | ---: |
| SSGI.Trace | 7.055 / 13.465ms | 0.617 / 0.622ms | 降低 91.3%，约快 11.4 倍 |
| 全部 SSGI passes | 7.586 / 14.048ms | 1.130 / 1.179ms | 降低 85.1% |
| 离屏 renderer | 9.184 / 16.307ms | 2.719 / 2.802ms | 降低 70.4% |

时间由生产 RDG pass 的 GPU timestamp 得到，包含其前置资源屏障；renderer 包含实际 Shadow/GBuffer/HBAO/Deferred/SSGI/Skybox。
计时启用 D3D12 debug layer，关闭 GBV；输入上传和只读缓存已准备，图像 readback 位于计时区域之外。
不包含探针更新、CPU 初始化/主线程、编辑器/gizmo、DLSS、后处理、透明或 Present，各帧单独等 fence；不得换算成主程序 FPS。
加速既来自去掉精确求交，也包含近似阈值提前终止带来的工作量下降，不能全部归因于 BVH 或 FP32。

### 画面与近似代价

纯 GI 与正常合成都在 640×400、四个静态相机、每模式 12 帧下比较最后一帧；两版使用同一 RAW、光照、采样序列、D32 和接收端合成。
纯 GI 从独立黑 HDR target 输出，不包含 Deferred 直接/环境底色或 Skybox，预览固定为 4 倍曝光；所有误差用未经预览映射的线性 HDR 计算。
实际生成并检查了纯 GI、正常合成及差异图。前三个视角的车身/车顶/后斗出现更广泛的橙色低频补光，窗口仍呈暗区；
这与近起点接受已有探针相符，不能凭这些图证明新的遮挡/间接光更正确。汽车基本出屏的第四视角变化较小。

| 纯 GI / mode 2 | 前景改变比例 | RGB MAE / RMSE | 前景平均亮度：旧 → 新 |
| --- | ---: | ---: | ---: |
| 相机 0 | 29.88% | 0.001487 / 0.006162 | 0.000462 → 0.001788 |
| 相机 1 | 28.06% | 0.001383 / 0.005916 | 0.000429 → 0.001665 |
| 相机 2 | 33.18% | 0.001731 / 0.006657 | 0.000559 → 0.002102 |
| 相机 3 | 0.92% | 0.000017 / 0.000382 | 0.000000153 → 0.000016958 |

前景包含地面，比例不是车体专属统计。改变阈值为 RGB 任一通道绝对差大于 1e-7；前三个视角最大 RGB 差约 0.131–0.134。
正常合成的同组前景 MAE 约 0.001383–0.001731，背景/直接光底色使相对变化显得更小；不能以合成图变化小掩盖纯 GI 差异。
所有 HDR/深度均有限，纯 GI 背景保持黑色；mode 1/2 当前均读取同一 raw 缓存，并非两个不同反弹层。

`RunVXGIMeshSDFThresholdAcceptance.ps1` 对真实配置资产使用独立世界三角形 BVH 参考，Debug/O2 GPU 各跑 32768 条有限射线。
四组固定随机种子覆盖面积加权向外/向内、实例均衡向外和世界范围射线；表面使用几何法线、原 0.02 偏移和 0.5 世界半径。
它不是屏幕可见/SSGI 失败射线统计，不含法线贴图，双方几何参考都不计算 Alpha，不能把下面数字直接当屏幕漏光像素比例。

- CPU/GPU 阈值查询状态、实例及距离超 1e-3 的不一致均为 0；非法/耗尽/停滞、非有限结果、有限区间和两候选预算失败均为 0。
  O2 最大距离差 2.0862e-7、Debug 8.9407e-8；最大位置分量差均为 3.8147e-6，最大局部字段差均为 1.1921e-7。
- 实例均衡向外组中，车体 4096 条：真实三角形命中 1736，近似命中 4079，其中 4078 条距离 ≤1e-5 且仍为原车体实例，约 99.56%。
  相对三角形有 2343 条近似额外命中；这是当前阈值壳的接受结果，不是 GPU 与 CPU 不一致。
  地面 4096 条：真实命中 13、近似命中 38，原地面起点接受为 0；紧 AABB 没有将向上离开平面的射线普遍接受为地面命中。
- 该均衡组双方都命中的 1749 条，绝对命中距离误差 median 0.1366、p95 0.4418、max 0.4976 世界单位；近似均更早，半径仅为 0.5。
  它说明当前尺度下位置偏差可接近整条射线长度，解释了画面出现大范围缓存补光的风险。
- 当前场景只有两个实例，相对逐实例阈值查询的 top-two 漏查为 0；这不验证更多模型时的保真度。
  先前两个空框挡住第三表面的预期漏查测试仍成立，最近两个盒的预算没有变成全场精确搜索。

### 屏幕模式控制与数值差异

四视角 D32 逐位相同；正常合成 GI-off 也均逐位相同。
屏幕模式跨后端并非完全逐位一致：相机 0/3 相同，纯 GI 相机 1 有 100 像素差、最大 RGB 1.5259e-5；相机 2 有 134 像素差、最大 0.000610352，
全图 RGB MAE 分别约 1.16e-9 与 3.74e-8。它们远小于新旧世界回退的画面差异，但报告明确保留，不能宣称屏幕模式全图逐位不变。
CPU-only DXC 审计发现相同 `-Zi -O3` 下，共享接收位置重建表达式在旧模块生成 `fdiv float`，新模块生成 `fdiv fast float`；
两版完整 Trace 的浮点代码生成已不同。没有证据把每个差异像素唯一归因于此；HBAO 不含历史且不进入 bounce source，可排除这一方向。
同一后端/二进制/RAW 的相机 2、mode 0、12 帧序列各重复一次，旧/新各自 HDR 和 D32 均逐位一致，差异像素 0，支持跨模块数值漂移而非运行不稳定的解释。

默认生产 Debug/Release 构建、工程 XML、runner 语法、Python 源码解析、Editor UI architecture 和差异空白检查通过；
主工程保留原有转换/类型/第三方 PDB 警告。A/B 四批完整源码/输入哈希清单一致，未改动生产默认参数或操作桌面。

### 证据与复现

- 汇总：`tmp/VXGI-Threshold-step4-acceptance.json`，含 ABBA 源报告 SHA256、两批/合并统计、重复抓图控制和实际几何来源。
- 纯 GI：`tmp/VXGI-Threshold-step4-quality-summary.json/.png`；单视角对照 `tmp/VXGI-Threshold-step4-camera0.png`。
- 合成：`tmp/VXGI-Threshold-step4-composite-summary.json/.png`。
- 计时：`tmp/VXGIStage75-{threshold,exact}-step4-debug-perf-{a,b}-1707x1067-report.json` 及输入哈希。
- 几何：`tmp/VXGIMeshSDFThresholdAcceptance-GPU-{Debug,O2}-report.json`。
- 数值控制：`tmp/VXGIStage75-{threshold,exact}-step4-screen-repeat-640x400-report.json`，`tmp/screen-drift-{exact,threshold}.ll/.dxil`。

```powershell
# 两个独立 NativeDebug 验收二进制；默认主程序仍是阈值路径。
.\Tests\RunVXGIStage75Performance.ps1 -BuildOnly -NativeDebug -DebugShaderPolicy
.\Tests\RunVXGIStage75Performance.ps1 -BuildOnly -NativeDebug -DebugShaderPolicy -ExactFallback
# 四相机纯 GI；另一次加 -ExactFallback、更换 Label，即得到精确参考。
.\Tests\RunVXGIStage75Performance.ps1 -SkipBuild -NativeDebug -DebugShaderPolicy -GIOnly -AllCameras -Samples 12 -ReuseRaw tmp/vxgi-stage75-performance/mesh-sdf-step4-gi-final-640x400-raw.bin -Label threshold-step4-quality
# 正常合成去掉 -GIOnly；计时改为 1707x1067/Camera0/Mode2/Repeat2，按新旧旧新顺序。
.\Tests\RunVXGIStage75Performance.ps1 -SkipBuild -NativeDebug -DebugShaderPolicy -Width 1707 -Height 1067 -Camera 0 -Mode 2 -Samples 12 -Repeat 2 -ReuseRaw tmp/vxgi-stage75-performance/mesh-sdf-step4-gi-final-640x400-raw.bin -Label threshold-step4-debug-perf-a
.\Tests\RunVXGIMeshSDFThresholdAcceptance.ps1
.\Tests\RunVXGIMeshSDFThresholdAcceptance.ps1 -DebugBuild
D:\Python\python.exe Tests/SummarizeVXGIThresholdAcceptance.py --reference tmp/VXGIStage75-exact-step4-quality-640x400-report.json --candidate tmp/VXGIStage75-threshold-step4-quality-640x400-report.json --output tmp/VXGI-Threshold-step4-quality-summary.json
D:\Python\python.exe Tests/SummarizeVXGIThresholdStep4.py
```

## 探针事件驱动缓存（2026-10-17）

生产主流程通过 `VXGIProbeCache` 连接场景快照、`VXGIProbeInvalidation` 与 `VXGIProbeState`。
SSGI 消费端保持原有限射线、最近两个模型 SDF 阈值命中及 raw SH 查询的契约。
没有调整生产 rays、距离、格距、光照、追踪分辨率或采样目标。

- 首次分配的新探针进入分类/采光队列；valid 达到 `probeMaxHistoryUpdates` 次成功更新后 clean，disabled 在分类完成后停止。
  当前生产目标仍为 32，不是进图立即停更；首次填充和编辑后的重新采样仍按每帧预算完成。
- 静止 clean 时保持原 history buffer 和内容，不发出 probe/filter dispatch，不复制全历史，不创建新的 GPU 反馈读回。
  已经提交的异步反馈仍需收尾；CPU 保留轻量场景版本检查。调度器的帧号、epoch、游标和提交次数也不因 idle 推进。
- 模型增删/TRS/alpha coverage 以旧、新包围盒查询依赖；受影响探针重新分类并清 SH。源材质 RGB/emission 改动只清依赖探针 SH。
  太阳光变化全场清 SH，但保留可靠分类；相机运动不使探针失效。
- 探针仍只分配在模型表面占据格。分配表变动按固定世界整数坐标生成 new→old remap，即使总数相同也执行 GPU remap。
  幸存且未失效的 raw/filtered 历史继续复用。邻接关系相同的已知可见性边也复用。
- 每个探针反馈 96B record + 32B 依赖 mask；追踪分类、采光和命中点太阳阴影的路径，包含 miss 路径。
  邻边可见性的六条中心线段也加入依赖。mask 使用稳定的 8×4×8 粗分区，只用于失效查询，不是新的 SDF 或光照体素。
  该分区允许误更新；真实长路径和粗分区可能使局部编辑刷新较大范围。未返回路径的在途探针按未知依赖保守刷新。
  模型超出原覆盖范围时扩大依赖域并全量失效；域不会随普通移动每帧重建或自动缩小。
- 去重队列和每探针 generation 拒绝过时反馈；只有真实提交成功才消费工作。每帧预算只限制重采样数量，所有新失效 SH 先立即清理，
  避免预算外仍显示旧光照。滤波更新当前批次及受影响邻居，几何事件清对应缓存边。

边界：模型移动仍会重建现有 CPU World 分配表；本次通过 remap 保留缓存，并未把 World 三角形提取改成增量构建。
Mesh SDF 资产仍复用；活动更新批次暂保留整份 GPU SH history copy，idle 没有该 copy。
分配变化仍有缓冲区重映射/重分配成本：raw 数量变化和诊断滤波拓扑重建会等待在途帧，频繁拖动尚可能产生停顿。
旧数值参考工具如果没有调用 `ConfigureCache`，仍使用原循环调度；主程序明确启用新缓存模式。
此缓存仅采集直接光/自发光源形成的间接补光，没有缓存到缓存的多跳传播，因此没有传播队列。

复现命令（全部后台，无可见窗口或 Present）：

```powershell
Tests/RunVXGIProbeInvalidationTests.ps1 -DebugBuild
Tests/RunVXGIProbeInvalidationTests.ps1
Tests/RunVXGILazyProbeGPUTests.ps1 -DebugBuild
Tests/RunVXGILazyProbeGPUTests.ps1
```

后台验收结果：

- CPU scheduler Debug/O2 各 8 组、失效控制器 Debug/O2 各 8 组通过；含 4096 次静止检查、10000 个粗网格覆盖案例、
  材质/coverage、域扩展、缺失依赖保守更新、邻边路径、稳定坐标 remap 与异步 generation 拒绝。
- 新生产 lazy GPU fixture Debug/O2 各 8 组通过，源输入 SHA256 前后稳定。488 个探针首次达到 fixture 目标 2 次成功更新，
  344 valid、144 disabled；随后 64 idle 帧 raw/filtered/edges 逐字节一致，probe 的 copy/dispatch/新增反馈和 filter dispatch 均为零。
- 模型移动后 244 个远处完整记录逐字节保留，资产未重烘；同数量 remap 保留完整 GPU record；材质修改保留分类。
  全场太阳变化采用 17 探针预算，第一帧即清空全部预算外失效 SH，之后队列有限收敛；488 个过期工作 generation 被拒绝。
- 旧 Mesh edit GPU Debug/O2 各 11 组通过；新旧 GPU 运行均为 D3D12 debug layer + GBV，warning/error 0、可见窗口 0、Present 0。
- 主工程 Debug/Release、Editor UI architecture、工程 XML、runner PowerShell 语法与差异/新增源文件空白检查通过。

报告：`tmp/VXGILazyProbeGPU-Debug-report.json`、`tmp/VXGILazyProbeGPU-O2-report.json` 及对应 `-input-hashes.json`；
旧回归为 `tmp/VXGIMeshSDFEditGPU-{Debug,O2}-report.json`，构建日志为 `tmp/VXGI-Lazy-Main-{Debug,Release}-build.log`。
新 GPU fixture 使用两组相隔的地面/方块、4 单位采光距离和 2 次目标以验证条件局部性及队列行为；
生产仍使用原 1000 距离和 32 次目标，不据该小场景声称真实场景的局部更新比例、首次填充时长或主程序 FPS 已验收。

## 懒缓存性能复测：GPU 空闲，Native Debug（2026-10-17）

本轮测试前/后 GPU 占用均为 0%。使用同一生产 EXE/参数/实际汽车地面、固定 camera0、内部 1707×1067、SSGI factor2、Quality1/radius0.5，
完成 3,500 个活动批次直到所有 27,181 valid 探针精确完成 32 次采样。随后 96 帧无新增探针/滤波更新、缓存复制或反馈读回，RAW 逐字节相同。
稳态丢弃前 24 帧后保留 72 帧；DLSS/ToneMap 用本轮 HDR/depth 单独串行复放，预热 24 帧后保留 72 帧。

| GPU 阶段 | 稳态中位数 ms | P95 ms |
|---|---:|---:|
| DLSS Quality 1707×1067→2560×1600，独立复放 | 1.660 | 1.882 |
| 完整 SSGI | 1.425 | 1.775 |
| 其中 SSGI.Trace | 0.921 | 1.266 |
| HBAO，含时域和几何历史复制 | 0.864 | 0.872 |
| DeferredLighting | 0.410 | 0.415 |
| ToneMap，独立复放 | 0.059 | 0.060 |
| Probe/Filter 实际 GPU 工作 | 0 | 0 |

在测得的生产阶段中，稳态最大单项为 DLSS；GI 内最大单 Pass 为 SSGI.Trace。核心 renderer（不含 DLSS/后处理）中位数 3.124 ms，P95 3.514 ms。
不同测量的中位数不能相加后换算主程序 FPS；编辑器、透明、完整主线程、Present 未覆盖，DLSS 输入为静止零 MV/零抖动复放。

填充期分类完成后的 238 帧：Probe GPU 中位数 0.321 ms/P95 1.966 ms、Filter GPU 0.011 ms；
探针 CPU 侧最大单项为依赖结果登记 0.429 ms，随后 Filter 录制 0.305 ms、Poll/统计 0.290 ms、Probe 录制 0.173 ms。
静止 Cache Prepare 降至 0.0067 ms。六邻居 BoxMask 在每次结果登记中重复计算，以及 Filter 临时列表/排序，是可继续研究的 CPU 成本。
一次性 World 几何/分配准备 1.907 s、Mesh SDF 资产构建 1.901 s、Topology 82 ms；随后静止无 World 重建或资产重烘。

保留当前生产 WARNING 820（AO history 的预设 RGBA clear 与时域 clear 不同）共 544 条，错误和其他意外警告为 0；没有过滤或修改渲染行为。
尾部复放 D3D12 warning/error 为 0。新旧 lazy 报告 EXE 与最终 RAW SHA 相同；旧有并发 GPU 负载时 Trace 中位数 6.220 ms，
GPU 空闲后为 0.921 ms，之前 5–15 ms 的异常长尾基本消失，旧结果只用于记录环境干扰。

完整分析/复现命令：`tmp/VXGI-Lazy-idle-gpu-performance-report.md`；
结构化汇总：`tmp/VXGI-Lazy-idle-gpu-performance-summary.json`；
原始主流程：`tmp/VXGIStage75-lazy-idle-gpu-native-debug-1707x1067-report.json` 及 `-input-hashes.json`；
原始尾部：`tmp/VXGIRenderTail-lazy-idle-gpu-report.json`。
所有输入 SHA256 通过；无可见窗口或 Present，没有桌面操作。最后一帧导图 CPU wall 已从汇总 wall 指标剔除。

## 首次缓存改为 ResourceManager 初始化（2026-10-17）

当前 `ResourceManager::InitVXGI()` 与 `InitIBL()` 一样属于加载阶段，在模型、材质池和 IBL 就绪后执行。
`ResourceManager` 持有 `VXGIResources`，统一保留 World、Mesh SDF、Topology、Surface、探针调度器和 GPU 历史；主程序只引用这些资源。
初始化复用运行时生产 Pass，等待全部有效探针完成配置的 32 次采样、禁用探针完成分类，并准备所有 GPU frame slot 后才返回。
此前性能记录中的“边显示边填充”属于旧启动时序；现在这段工作移到首次绘制之前，加载等待仍然存在。

进入主循环后，静止场景从第一帧开始就没有探针生产任务。模型移动、材质或光照变化仍通过同一套局部失效和有限更新处理。
当前 Debug → Pass 只切换查看及世界回退，不重新初始化资源；当时的 T 快捷键已移除。关闭 VXGI 配置时跳过首次烘焙。

后台验收调用实际 `ResourceManager::InitVXGI()`，检查完整初始化后连续 `2 → 0 → 2` 的首帧及后续帧均无缓存 GPU 更新，并逐字节核对 RAW。
复现命令：

```powershell
Tests/RunVXGIStage75Performance.ps1 -NativeDebug -DebugShaderPolicy -ColdStart -LazyProbes -StartupToggle -InitializeProbeCache -Samples 32 -Width 1707 -Height 1067 -Camera 0 -Mode 2 -Label resource-init
```

实际汽车/地面、Native Debug 后台验收通过：初始化约 12.399 秒（仅 VXGI 初始化，含管线、场景/SDF和缓存生成），
53,306 探针全部分类，27,181 valid 精确完成 32 次采样，26,125 disabled；首帧计划工作数为 0。
连续 32 帧包含 `2 → 0 → 2`，全部 Probe/Filter 活动计数不增加，RAW 逐字节不变，且与迁移前完整缓存 SHA256 相同。
此等待发生在加载阶段，未声称降低首次生成的总成本或实测完整主程序 FPS。
原始报告：`tmp/VXGIStage75-resource-init-1707x1067-report.json`；源输入和构建哈希均校验通过。
D3D12 错误、意外警告、消息丢弃均为 0；原有 AO clear-value 警告 820 保留 32 条。全程隐藏窗口、Present 0。
现有局部更新 GPU 回归 8 组通过（D3D12 + GBV 零警告/错误），覆盖移动局部失效、远端记录保留、材质/光照变化及过期反馈拒绝。
主工程 Debug/Release 构建、SSGI 历史 CPU 测试、Editor UI 架构检查和工程 XML 检查通过。

## 稳态消费增量：SSGI 直接结束与 VXGI 世界回退 A/B（2026-10-17）

用同一生产 Trace PSO、同一份充分填充的 RAW、相同相机/质量/分辨率/采样帧直接比较 mode0（屏幕失败返回零）和 mode2（世界 BVH/SDF/SH 回退）。
Native Debug、内部 1707×1067、GI factor2/quality1/radius0.5。单进程连续 `0/2/2/0`×2，每块预热96、保留128，每种模式512帧。
计时块内和块间没有图像读回，捕获移至计时结束后的两个验证序列；所有验证均后台，无可见窗口/Present。

| GPU 步骤 | 纯 SSGI ms | 开启世界回退 ms | 增量 ms |
|---|---:|---:|---:|
| SSGI.Trace | 0.388496 | 0.622928 | +0.234432 |
| 完整 SSGI | 0.893152 | 1.127264 | +0.234112 |
| 核心 renderer，不含 DLSS/UI/Present | 2.604480 | 2.839472 | +0.234992 |

完整 SSGI 增加约26.2%；Trace 增加约60.3%。四对 Trace 差值中位数稳定在 +0.233904..+0.234480 ms。
非 GI 基线仅差0.000304ms，非Trace的SSGI仅差−0.000624ms，增量确实集中于世界回退所在 Trace。
上次0.921ms是开启回退后的Trace总时间，不能当作VXGI边际成本；应使用本次约0.234ms的直接差值。

这是静止、探针已填充完成后的消费开销；首次场景/SDF构建、缓存生产、移动重采样、CPU维护、DLSS等仍单独计算，不能据此推算主程序FPS。
RAW前后逐字节不变、两模式深度相同、源哈希通过；D3D12错误/意外警告/消息丢弃均0，已知820性能警告保留2240条。
完整约定、分块数据及复现：`tmp/VXGI-world-fallback-ab-report.md`；原始报告：`tmp/VXGIStage75-world-fallback-ab-continuous-native-debug-1707x1067-report.json`；
汇总：`tmp/VXGI-world-fallback-ab-continuous-summary.json`。本轮只增加测量工具，没有修改生产 Shader 或质量参数。

## 初始化加载画面（2026-10-17）

启动时先初始化 ImGui 和独立的加载命令资源，再显示 `EditorUICenter::LoadingScreen`。
中央进度条下方显示当前阶段或正在加载的模型文件名；覆盖资源、IBL/SH9、Mesh SDF、VXGI 探针采集和编辑器初始化。
各阶段使用固定的总体占比；探针阶段按实际完成采集数推进，禁用探针分类后视为完成。百分比表示工作进度，不表示剩余秒数。
快速探针计数最多每 50 ms 重绘一次，阶段和文件名切换立即更新。首次缓存就绪并显示 100% 后才进入正常主循环。

加载 UI 通过独立 allocator/list/fence 提交，不重置资源加载正在录制的命令列表。
围栏位于 Present 之后，退出加载或关闭程序时包含最后一次呈现的 GPU 工作。关闭窗口可以取消加载。
控件由 UI Center 和主题统一提供，F10 Loading 节有相同组件及全屏预览。

后台验证（先构建对应配置）：

```powershell
Tests/RunEditorLoadingRenderTests.ps1
Tests/RunStartupLoadingTests.ps1 -Configuration Debug
```

组件测试使用真实 ImGui DX12 后端渲染离屏纹理，覆盖 100%/150% DPI、普通/窄窗口、首帧和长中文状态；没有原生窗口或 Present。
完整启动测试使用 `--verify-startup`，窗口保持隐藏，实际执行加载画面的 Present，初始化后立即退出。
同时检查进度单调到 100%、非空状态、探针就绪、D3D12 消息以及进程正常退出，避免仅凭初始化报告忽略退出清理错误。
报告与截图分别位于 `tmp/Loading-startup-*.json*` 和 `tmp/editor-loading-render/`。

## 启动慢态修复与 mode-SRV 验收（2026-10-18）

完整主程序的后台记录复现了：探针已经提前生成、静止时 `probe_work=0`，
SSGI.Trace 仍会从启动初期约 0.7 ms 进入约 3.5 ms 的慢态；T 执行 `2 → 0 → 2` 后恢复较快状态。
仅把着色器中的非零模式值 `2 → 1 → 2`，保持世界路径和工作量相同，也能改变该状态。
这与探针尚未填充、历史重置或增加了射线工作量的解释不符。

`no-background` 和 `no-pgo` 诊断分别通过 `ID3D12Device6::SetBackgroundProcessingMode`
禁用后台工作或系统 profiling，均使当次慢态不再出现。这是当前设备/驱动环境下后台优化策略影响异常的因果证据；
具体驱动特化变体及其最终机器码尚未确认。禁用 PGO 仅用于诊断，生产修复保留系统默认后台策略。

mode-SRV 修复的读取契约：

- Trace 常量结构仍为 624 字节，CPU `worldMode` 偏移由 `static_assert` 固定为 604。
  生产 Trace 定义 `SSGI_MODE_FROM_SRV=1`，以 `ByteAddressBuffer.Load(604)` 读取原模式值。
- 新增第 7 个 root SRV，即根参数 `slot 7 = t4, space1`；`slot 0` 仍为 CBV。
  新 SRV 与 CBV 绑定同一份 Trace allocation、同一 GPU 虚拟地址，不另建或复制模式缓冲。
- 原 6 个世界 SRV、CPU 默认 mode2、T 开关、有限射线及预算、BVH/SDF 命中与探针缓存逻辑均不变。
  mode0、WorldInput 为空或缓存缺失时，Trace 也会绑定新 SRV，并读取对应的有效模式。
- 其他 SSGI 阶段不读取新 SRV；`EXACT_FALLBACK` 及未定义该宏的独立 shader 验证保留原 CBV 读取路径。
  当前设备上的最终验收通过；此调整没有禁止驱动进行其他优化。

已有静态输出证据：`mode-srv-fixed-a` 的帧 856、1432 均处于联合采样相位 `frame % 576 = 280`。
两张 RawGI 各有 456,036 个 RGBA16F 像素、3,648,288 字节，互相逐字节相同，
并与旧 `raw-toggle-a` 同相位输出逐字节相同；RGBA 最大误差、MAE、RMSE 均为 0。
该证据限于已采静态场景和相位，不替代动态场景、其他视角或全部相位的画质验证。

用户关闭其他 GPU 程序后，使用完整 Debug 主程序的隐藏窗口完成独立启动及真实 ImGui T 往返。
相同 EXE、DLSS DLL、根签名及资源，只把 helper 的返回值改回 `hybrid.worldMode`，即可恢复慢态；
两组渲染源码哈希的差异仅在该 HLSL 文件。所有计时均为 GPU Dispatch 中位数，非 CPU 提交耗时。

| 验收 | 启动后 / 切换前 | T 关闭 | T 恢复 / 后续 |
| --- | ---: | ---: | ---: |
| 旧 CBV 读取控制 | 3.483 ms（2–8s） | — | 3.488 ms（10–18s） |
| SRV 独立启动（32s） | 0.749 ms（2–8s） | — | 0.753 / 0.756 ms（10–18s / 20–32s） |
| SRV T 往返（32s） | 0.751 ms（2–8s） | 0.347 ms | 0.708 / 0.705 ms（10.25–18s / 20–32s） |
| SRV 后台优化压力（24s） | 0.751 ms（2–8s） | — | 0.751 / 0.754 ms（10–18s / 20–24s） |

SRV 两轮稳态没有超过 2ms 的 Trace 帧，也没有旧版持续或隔帧约 3.5ms 的慢平台。
T 往返仍有约 0.04ms 的 Trace 小差异，不能写成所有计时完全相同；完整 Frame 中位数为
4.286 → 4.311ms，按 CPU 帧间隔统计约 231 → 228 FPS，没有原先切回后整帧突然加速的现象。
隐藏 Present、诊断开销与 Visual Studio 会话不同，以上 FPS 不能直接当作用户可见帧率承诺。

后台优化压力轮使用诊断专用 `ALLOW_INTRUSIVE_MEASUREMENTS`，92帧报告仍需测量，
最后一次请求位于1.062s；之后完成优化仍保持快速状态。5242帧查询均成功，所有稳态窗口均无>2ms Trace，
请求测量期间最大1.079ms。生产默认路径没有该强制策略或禁用优化设置。

独立启动的两份 RawGI（相位280）逐字节等于旧输出；T 往返在切前6.000s和恢复后13.386s
取得相位201的两份 RawGI，也逐字节相同。每份456,036像素、3,648,288字节，全部通道有限。
此证据限于静态场景及所选相位；射线、采样数量、分辨率、Mesh SDF/BVH 与探针数据均未降级。

清理后的 Debug / Release 构建均成功。小规模 GPU 正确性检查通过12组（22条参考射线、8个kernel像素，
以及生产 SSGIPass/RDG 的3帧槽、2次resize、12次提交），启用 GPU validation，零警告、零错误。
失败的 `frame-lists`、`split-dlss` 和未使用的 GPU-local 常量实验已撤除。
两轮存在启动重叠或切前采样缺失的中间记录未用于最终性能结论；runner 现会拒绝缺失切前/切后稳态或首图位于切后等情况。

- 因果对照、分布、后台优化压力结果及限制：[启动调查汇总](tmp/VXGI-startup-investigation-summary.md)。
- 空闲独立启动：[运行与哈希](tmp/VXGI-full-mode-srv-idle-control-acceptance.json)、[RawGI](tmp/VXGI-full-mode-srv-idle-control-rawgi-report.json)。
- 同 EXE 的 CBV 控制：[运行与哈希](tmp/VXGI-full-cbv-read-idle-control-acceptance.json)。
- 串行 T 往返：[运行与哈希](tmp/VXGI-full-mode-srv-idle-toggle-serial-acceptance.json)、[RawGI](tmp/VXGI-full-mode-srv-idle-toggle-serial-rawgi-report.json)。
- 串行后台优化压力：[运行与哈希](tmp/VXGI-full-mode-srv-idle-pgo-serial-acceptance.json)。
- 清理后的构建：[Debug](tmp/VXGI-mode-srv-final-Debug-build.log)、[Release](tmp/VXGI-mode-srv-final-Release-build.log)。
- GPU 正确性：[报告](tmp/VXGIHybridGPU-stage77-O2-report.json)、[执行日志](tmp/VXGI-mode-srv-correctness-mesh.log)。

## 完整主程序 Debug Layer / GBV 验收（2026-10-18）

新增显式诊断参数 `--profile-gpu-validation`，在任何设备创建前启用 GPU-Based Validation，
并在设备返回后、创建应用队列和资源前接入 InfoQueue。正常启动不会主动开启 GBV。
`--profile-mode-read=cbv|srv` 选择对照读取方式，默认仍为 SRV；不需要修改着色器源码。
完整命令和验收门槛见 [诊断工具说明](Tests/STARTUP_FRAME_DIAGNOSTICS.md)。

使用同一 Debug EXE、相同源码及 DLSS DLL，串行验证真实汽车/地面、完整探针构建、
RDG、DLSS、编辑器绘制、Present 和 GPU 空闲后的 DLSS/ImGui 清理：

| 路径 | 实际提交 | 模式序列 | 错误 | 警告 | Attach 后过滤 / 容量丢弃 |
| --- | ---: | --- | ---: | ---: | --- |
| 旧 CBV | 36 帧 | 2 → 0 → 2，每段12帧 | 0 | 0 | 0 / 0 |
| 当前 SRV | 36 帧 | 2 → 0 → 2，每段12帧 | 0 | 0 | 0 / 0 |

每段三个物理帧槽各提交4帧，两轮各保存1660条原始消息，包含运行时 GBV 已启用的确认。
设备刚返回时，InfoQueue 已有64条内部消息被默认过滤；此基线与后续零增量分别保留，
不能将不可恢复消息的严重性推断为已知，也不声称设备内部从零开始的全部日志都被捕获。
最终成员 COM 释放发生在最后一次收集之后，本轮不是资源泄漏检查。

首次验证发现 HBAO 单通道历史纹理的清屏值与优化清屏值不匹配（warning820）：
资源优化值 `{1,0,0,0}`，原实际值 `{1,1,1,1}`。已将 scalar clear 未使用的分量归零，
保持 R 通道结果不变；正式复验没有该警告。原失败记录保留，未过滤已知警告或覆写历史报告。

关闭 GBV 后，再用同 EXE 各短跑8秒，2–8秒 Trace 中位数仍为 CBV **3.502ms**、SRV **0.752ms**。
因此清屏修正没有消除原 Trace 慢态。GBV 验收只说明所执行路径内未检测到 API 使用错误，
不能据此确定驱动机器码的具体原因或排除所有竞态，也不能用 GBV 的 GPU 时间比较性能。

- [完整原始证据、独立审核与采集边界](tmp/VXGI-full-gbv-validation-summary.md)。
- [CBV 验收](tmp/VXGI-full-gbv-cbv-full-c-acceptance.json)、[SRV 验收](tmp/VXGI-full-gbv-srv-full-c-acceptance.json)。
- 本轮构建均成功：[Debug](tmp/VXGI-full-gbv-Debug-acceptance-build.log)、[Release](tmp/VXGI-full-gbv-Release-build.log)。

## PIX 后台诊断尝试（2026-10-18）

成功捕获旧 CBV 慢态：未加载 PIX 的 Trace 基线约 3.468ms；加载 PIX 后，捕获前后仍约
3.479/3.493ms。捕获文件约 285MB，但命令行分析两次失败；英文路径重试明确报告
NVIDIA Perfworks 当前硬件配置不受支持，以及 `E_PIX_MISSING_PERFORMANCE_LOGGING_PERMISSIONS`。
按用户要求停止 PIX，不再修改权限或重复尝试；未取得可解释原慢态的原生指令或硬件计数器。

临时 PIX 接入代码已撤除，79 个渲染输入源码哈希与尝试前一致；原 SRV 修复保留。
Debug 重新构建成功，普通 SRV 回归 2–8 秒 Trace 中位数 0.741ms，无 >2ms 帧。
[捕获、日志、采样边界及清理记录](tmp/VXGI-pix-investigation-summary.md)。

## 完整流程纯 GI 显示（2026-10-18 历史记录）

2026-10-19 起改用顶部 Debug → Pass：启动正常合成，菜单还可选择关闭 VXGI、仅 SSGI 或仅 GI；旧 JSON 显示键和 T 已删除。
以下保留首次纯 GI 实现的行为及验收记录。当时 `Settings/Pipeline.json` 的 `ssgi.gi_only` 设为 `true`，启动显示完整滤波后的间接照明
`E/pi * intensity`，不乘接收模型的 Albedo、Fresnel 或金属度响应。
当时 T 只切换 VXGI 世界空间补充，`gi_only=false` 用于恢复正常场景合成；这两个旧入口现已移除。

SSGI 所需的 DeferredLighting/BounceSource 保留，屏幕追踪、Mesh SDF/探针补充、重建、时域累积、
HistoryClamp 和 Atrous 均走原流程。最终 Composite 的纯照明 PS 变体在接收材质计算之前返回，
以覆盖写 PSO 排除原 SceneColor 的直射光/IBL，并跳过后绘天空、透明物体和 VXGI 调试覆盖。
光源、BounceSource 和探针中源表面产生的彩色反弹保留，它们属于 GI 光照本身。
随后仍执行原 DLSS/AA 和色调映射。正常加法合成 PSO、worldMode 的 SRV 规避及 T 的历史失效机制保持原样。

Debug 构建及后台完整主程序验证通过：4215 帧完成 `2 → 0 → 2`，每帧纯 GI 标记为真，
完整 GI 阶段和 DLSS 均有执行，天空/透明阶段未出现。
[完整流程及图像核对](tmp/GI-irradiance-display-validation.json)、[运行与源码哈希](tmp/VXGI-full-gi-irradiance-toggle-a-acceptance.json)。

同视角、同冻结探针缓存的生产 GI/Composite 图已实际检查；两种模式均有限、背景 RGB 为零，
D3D12 警告/错误为零。对照图使用诊断固定曝光 4 和 Reinhard/sRGB 预览，不包含主程序 DLSS/后处理，
不作为实际窗口最终显示的逐像素复刻：

- [仅 SSGI 纯照明](tmp/vxgi-stage75-performance/gi-irradiance-ssgi-640x400-camera0_mode0.png)
- [SSGI + VXGI 纯照明](tmp/vxgi-stage75-performance/gi-irradiance-vxgi-640x400-camera0_mode2.png)

此前 `gi-only-view-*` 图错误地显示了 GI 乘接收材质，保留为历史记录，不作为当前纯照明结果。
正常合成控制与修正前 HDR 输出逐字节相同，仍显示天空及原场景照明；测试 Harness 的 `-GIOnly` 显式覆盖交互配置，
避免项目默认纯 GI 时误把普通合成回归当成纯 GI。

## 纯 GI 直边黑块定位（2026-10-18）

对汽车侧面的矩形黑块，冻结同一已完成 RAW、相机、640×400 分辨率、12 帧序列及生产
Trace/重建/时域/Atrous/Composite，分别只改变导入的测试缓存：

| 控制 | 改变 | 车身黑块内部线性亮度均值 |
| --- | --- | ---: |
| 原始 RAW | 无 | 0.000110 |
| valid_constant | 保留禁用状态，有效探针全部设为常量 radiance 1 | 0.000262 |
| all_constant | 所有已分配探针临时设为有效、常量 radiance 1；不增加格子 | 0.996041 |
| l0_only | 保留禁用状态与 L0，只清方向项 | 0.000097 |

ROI 为同相机输出像素 `[342,348) × [233,241)`，共48像素；四组深度逐字节相同，
使用同一测试 EXE，D3D12 警告/错误均为零。三张控制图均已实际检查。
这些是隔离原因的人工缓存，不是可用于正式渲染的补光或防漏光修复。

该黑块主要由禁用探针造成的查询支持缺口触发：`VXGIProbeQueryRadiance` 只使用八邻格中
有效且更新过的记录；全无效时返回零。当前还会按有效权重总和重新归一，因此最后一个
有效角点的权重趋近零时亮度不会随之衰减，跨过支持边界后直接归零，能沿格子平面形成直边。
原始快照53,306个探针中27,181有效、26,125禁用；有效探针全部完成32次更新，无未分类记录。
这不是初始化尚未完成。`all_constant` 没有增加查询格子，因而单纯缺失 lookup 不能解释此块。

低阶 SH 方向重建的负值钳制仍会影响其他区域，但清除方向项未消除该块，不能将它当成此块主因。
生产查询读 raw SH，已有邻域 filter 的输出不参与这个查询，因此它不会补齐支持缺口。

另有已记录的阈值自命中风险：当前 Mesh SDF 阈值0.9格、格距1，起点偏移仅0.02，
从 t=0 接受命中。原真实几何测试4096条车体向外射线有4078条在起点接受同一实例；
当前核心阈值实现、场景和资产哈希未改变。该统计不是本次屏幕失败射线或截图像素比例，
只能说明错误近似命中位置会进一步暴露接收面附近的缓存支持分布。

本次未修改生产渲染行为。修复应保留内部探针禁用，解决表面外侧的有效支持覆盖和阈值起点自命中，
而不能把测试中临时启用全部探针当成正式方案。

- [可复现 CPU 控制生成/比较脚本](Tests/AnalyzeVXGIBlackBlocks.py)
- [原始缓存、控制缓存 SHA 和全前景分析](tmp/vxgi-black-blocks/cpu-controls-report.json)；全前景含地面，不是车身支持率。
- [车身黑块 ROI 对照及深度一致性](tmp/vxgi-black-blocks/car-black-block-roi-analysis.json)
- [有效探针恒亮，黑块仍在](tmp/vxgi-stage75-performance/black-blocks-valid-constant-640x400-camera0_mode2.png)
- [已分配探针全部临时有效，黑块消失](tmp/vxgi-stage75-performance/black-blocks-all-constant-640x400-camera0_mode2.png)

## 同尺寸探针格外铺一层（2026-10-18）

`vxgi.probes.padding_layers` 当前默认 `1`，支持 `0/1/2`。格距仍为 `0.5`，以真实三角形
占用格为种子，按完整26邻域增加同尺寸格子，包含面、棱和角，重叠坐标去重并确定性排序。
原有世界坐标的探针中心不移动；新增格子也执行原分类、禁用和有限捕获规则。

World 将几何 `GetCells()/FindCell()/GetGrid()` 与探针 `GetProbeCells()/FindProbeCell()/GetProbeGrid()`
分开。外层探针格没有三角 CSR 引用，不构成遮挡，也不扩展 Mesh SDF 资产。
Topology、GPU 上传、初始化预算、局部失效和按坐标历史映射使用完整探针分配。
只改层数会改变 allocation revision，保留 geometry/material/coverage identity 和距离节点。
SSGI/世界阈值求交/八邻 raw SH 采样着色器均未修改，内部探针保持禁用。

实际配置场景的零层/一层离屏对照：

| 项目 | 零层 | 一层 |
| --- | ---: | ---: |
| 几何占用格 | 53,306 | 53,306 |
| 探针总数 | 53,306 | 108,966 |
| 有效探针 | 27,181 | 57,211 |
| 禁用探针 | 26,125 | 51,755 |
| 一份 RAW 数据字节 | 5,117,376 | 10,460,736 |
| 原车身黑块 ROI 平均线性亮度 | 0.000114 | 0.250773 |

ROI 为 `[342,348) × [233,241)` 的48像素。两张640×400纯 GI 图已实际检查，该黑方块被补齐；
不代表所有视角的所有黑块均解决。两组24帧、深度逐字节相同，几何 fingerprint、三角引用数、
Mesh SDF 资产数、距离节点数和上传字节数相同，D3D12 警告/错误均为零。
新增布局重新烘焙，不沿用旧 RAW 索引；零层用旧32次快照，一层32轮加计时排空使768个探针达到33次，
其余有效探针均32次。新增索引也会改变随机方向种子，这不是严格匹配随机样本的噪声对照。
离屏未改的 ShadowMap 耗时也明显变化，故不据两张图的计时宣称增加一层会提速或给出可靠性能差值。

Debug 主程序构建、UI 架构检查及隐藏完整程序验证通过。实际初始化完成7,354批捕获后开始渲染，
首帧 world probeCount=108,966；4,157帧 `probe_work=0`，`2→0→2`、完整纯 GI/DLSS 流程正常。
切换前/后 GPU Frame 中位数4.203/4.210 ms，Trace 0.764/0.732 ms；这是本轮当前实现的实测，
不是跨版本性能差值。CPU World/Query/Scene 回归及9组局部失效测试均通过，覆盖0/1/2层、
角邻居、重叠去重、保留距离场、静态不更新和模型移动/还原时的局部历史保留。

- [同场景图像、ROI、内存与采样边界](tmp/VXGI-probe-halo1-image-analysis.json)
- [完整主程序验收](tmp/VXGI-full-probe-halo1-toggle-a-acceptance.json)、[当前帧时间](tmp/VXGI-probe-halo1-main-analysis.json)
- [零层纯 GI](tmp/vxgi-stage75-performance/probe-halo0-reference-640x400-camera0_mode2.png)
- [一层纯 GI](tmp/vxgi-stage75-performance/probe-halo1-baked-640x400-camera0_mode2.png)

后台复现：`Tests/RunVXGIStage75Performance.ps1 -NativeDebug -DebugShaderPolicy -GIOnly
-ProbePaddingLayers 1 -WarmupRounds 32 -Width 640 -Height 400 -Samples 24 -Camera 0 -Mode 2 -Label <新标签>`。
`-ProbePaddingLayers` 是测试进程配置覆盖，不改交互配置。正式程序继续使用 Pipeline.json 的一层默认值。
