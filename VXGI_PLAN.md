# 固定稀疏探针 VXGI 缓存实施计划

阶段 1—6 的代码接入与后台验收已完成，包含世界登记、SDF 表面查询、GPU 固定探针分类/SH 历史、邻边可见性、raw 表面查询和混合 SSGI。
最终 Debug/Release 主工程构建、独立 CPU/GPU 数值与后台集成检查通过，主场景分类/更新及部分实际 UI 已检查。
新内核已有有限 batch 时间采样；具体覆盖范围与限制见 `VXGI.md`。
第五步 CPU/GPU Debug/O2、production wrapper、shader 与主工程构建均通过；新增视图尚未实际检查，按用户要求只做后台工作。
第六步已将探针查询接入屏幕/SDF 混合 SSGI，后台验收记录见下文；实际主场景画面和新增 pass 性能尚未验收。
阶段 7.1 编辑失效修复与 7.2 alpha 接入前的真实场景基线已完成；7.3 alpha-cutout 全链路和后台回归已完成。
阶段 1—7.2 的完整 cutout 遮挡、mip 只清 SH 和性能数字保留为历史记录；当前覆盖契约以 7.3 为准。
阶段 7.4 已完成 post-alpha 全场分类/支持率与真实离屏验收，当时数值可用性与实时性能均未通过。
阶段 7.5 加入密集候选格子 BVH、2/4/8 档 GI 追踪分辨率和 Debug GI shader 优化，当时 JSON 选择每轴 1/8。
世界格子与探针调试绘制现默认 Off，按需通过既有 Debug view 开启；离屏性能数据未包含这些叠加绘制。
当时同输入的 1707×1067 后台 12 帧采样中，1/8 档 Trace median 4.85ms、离屏 RDG median 6.67ms。
该范围不含探针更新、DLSS、postprocess 和编辑器；数值拒绝与主程序整帧预算仍须验收，详见 `VXGI.md` 的阶段 7.5。
阶段 7.6 按用户纠正：原 SSGI 射线的起点、方向和有限终点只作坐标变换，世界求交后读取方向 SH 缓存。
已删除现场直接光/太阳阴影/命中材质着色、接收面重定位和屏幕命中后的附加缓存计算；恢复原1/2采样。
以下阶段6的 Lo0/Lo1 模式与独立世界距离为历史实现，已被该契约取代；新验收另记于 `VXGI.md`。
阶段7.6主工程Debug/Release、CPU Debug/O2、shader及两套GPU Debug/O2回归已通过；GBV无警告。
当前场景已在隐藏窗口重新导出纯SSGI与探针回退纯GI对照；验收记录见 `VXGI.md` 阶段7.6，数值与输入清单见 `tmp/VXGIStage76-final-summary.json`。
当前生产探针更新已改为事件驱动：首次有限填充后静止停更；模型、材质与光照事件触发依赖失效和预算队列，详见文末新增记录。

## 已确定的设计

- 世界空间规则格子，按模型表面稀疏分配；每个已分配格子一个固定位置探针。
- 每个探针保存 4 个 RGB SH 系数，采用同一世界空间基底，覆盖 `l=0,1` 两个 band。
- 内部或无法可靠采样的探针禁用；运行时不移动探针。
- 六邻居滤波检查邻探针之间的遮挡，仅作为诊断；旧辐照度诊断保留逐贡献者验证，当前生产回退只读取八角点raw缓存并插值，不追加可见性追踪。
- 首版使用固定世界范围、静态不透明几何、方向光和漫反射 GI。

阶段7.7进一步精简命中后的消费路径：读取邻近探针缓存、三线性插值、方向SH求值后返回，删除逐贡献者可见性追踪。
保留缓存构建/分类/更新/邻间遮挡与旧表面诊断；不改变当前采样分辨率和射线预算。
7.7主工程Debug/Release、shader及两套GPU Debug/O2功能回归均通过，GPU验证无警告；实景纯GI已后台检查。
背景性能计时波动，未确认提速，性能验收仍未通过；报告见 `tmp/VXGIStage77-final-summary.json` 和 `VXGI.md` 阶段7.7。

## 当前回退的光照契约

原SSGI射线保持起点、方向和有限终点，屏幕命中沿用原bounceSource；失败才将同一有限线段变换至世界求交。
世界命中仅沿原A→B方向重建邻近raw探针的入射辐亮度，采样阶段不发可见性射线，不按命中面法线卷积、不做命中材质着色或现场直接光。
全部射线平均仍输出接收端 `E/pi`，接收材质响应只乘一次。探针采光更新独立执行，下面的Lo0定义仅用于其采集源。

## 阶段6原光照契约（历史）

探针 SH 存未做余弦卷积的入射辐亮度。查询时按表面法线做卷积，输出 `I = E/pi`。
`rho_d` 表示源面的漫反射反射率；源面着色要明确其材质能量响应，并在屏幕源和世界源间保持一致。

```text
Lo0(x) = directDiffuse(x) + cappedEmissive(x)
Iprobe(x,n) = Eprobe(x,n) / pi
Lo1(x) = Lo0(x) + rho_d(x) * Iprobe(x,n)
```

- `Lo0` 是启动探针的已知光源；排除环境 SH、镜面、AO 和间接光反馈。
- 自发光沿用现有 GI 源峰值 RGB 上限 2，保持可见自发光与 GI 源的区分。
- 探针第一版只采集 `Lo0`；无需等待探针自身产生的光照，消除循环依赖。
- 屏幕接收端沿用余弦加权半球采样 `p=cos(theta)/pi`，对每条射线的命中 `Lo` 求平均，继续输出现有 `E/pi`，接收材质响应只乘一次。若改采样 PDF，估计量须改为 `Lo*cos(theta)/(pi*p)`。
- 第一阶段命中返回 `Lo0`，得到一次间接反弹；加入探针项后返回 `Lo1`，增加第二次间接反弹。
- 首版世界追踪未命中返回零局部辐亮度，现有接收端 SH 环境照明继续按原路径计算。

## 当前项目可以复用的部分

| 当前代码 | 规划用途 |
| --- | --- |
| `Resources/PBR_Model.h` 的 CPU vertices、LOD0 indices、UV/TBN、materialID | 世界几何登记、体素化、命中表面身份及材质恢复 |
| `ResourceManager::GetSceneInstances()`、`ModelInstance` | 全场景实例、稳定 ID、变换和材质覆盖 |
| GPU 全局材质池及 bindless 纹理 | 屏幕外表面材质采样 |
| `Shaders_For_GBuffer.hlsl` 的材质语义 | sRGB、材质因子、ORM、法线图、alpha 和 unlit 的一致性参考 |
| `SSGIPass.h`、三个 SSGI shader | 半球采样、降噪、历史和 HDR 合成 |

世界几何已通过 `VXGIWorld` 独立登记全部参与 GI 的场景实例，固定使用 LOD0。
主场景实例上传仍依赖相机可见列表；GI 不用该列表或被视锥剔除更新的 `isVisible` 决定登记状态。
当前 CSM 依赖相机级联覆盖；世界表面直射阴影首版采用 SDF 朝光源的可见性射线。

## 阶段 1：世界几何登记与稀疏分配

1. 整理全部参与 GI 的模型、实例变换、LOD0 三角形、材质 ID 和材质覆盖。
2. 建立锚定世界原点的规则格子与格子尺寸，标记真正与表面相交的格子；bounds 从占据范围推导。
3. 输出 compact cell/probe 列表和位置到索引的查询映射；空格子不分配 SH。
4. 探针固定在格子中心；分类后只改变有效标记。
5. 先测严格表面格子的有效覆盖率。若禁用后缺失严重，再允许模型表面外侧一层格子参与，仍保持模型附近的稀疏分配。

交付：占据格子和固定探针的实际渲染可视化、分配数和内存统计。

验收：转动相机、改变相机选用 LOD，不改变世界几何登记和探针分配；原有实例变换、材质覆盖正确。

## 阶段 2：距离场与 SDF 软件追踪

1. 首版为固定小场景建立距离体；距离场分辨率与探针密度分别控制。
2. 建议先用 CPU BVH 加速最近三角形查询，烘焙静态距离体并作为小场景正确性参考；实时追踪在 GPU 上进行。GPU 行进使用保守步长与局部命中复核，验证烘焙/插值误差不会使射线越过薄墙。
3. 距离体配套保存或关联附近三角形/实例候选。接近命中时局部复核，恢复三角形身份、重心坐标和 UV。
4. 明确封闭网格与开放平面的距离语义；开放平面不能依赖封闭实体内外符号来判断有效性。
5. 追踪返回命中状态、位置、距离、几何法线、材质身份和可用的 UV/TBN。几何法线保留真实正反面，着色法线单独计算。

交付：SDF 距离切片、命中位置/法线、太阳可见性视图。

验收：盒子、地面、薄墙及非均匀缩放实例，与三角形参考追踪核对命中距离和遮挡；黑色表面仍然终止射线。

## 阶段 3：世界表面材质与直接光

已实现：真实命中 UV 的 bindless 材质查询、固定显式 mip、共享屏幕/世界 GI 源响应、SDF 二值太阳遮挡与独立更新 revisions。
源光按命中点即时计算，供后续探针缓存；没有新增均匀的逐三角形光照存储或表面图集。

1. 从真实表面身份恢复 albedo、metallic、emissive 和法线；沿用现有线性颜色与材质因子约定。
2. compute 材质采样使用显式 `SampleLevel` 与约定 footprint；先验证纯色材质，再验证纹理材质。
3. 定义适合漫反射缓存、与相机无关的源面响应。共享屏幕/世界 GI 源着色公式，保留接收端现有合成语义。
4. 向太阳发射 SDF 遮挡射线，形成 `Lo0`。世界表面光照缓存覆盖参与 GI 的完整几何。
5. 表面光照更新与几何构建分开：相机移动不触发世界光照改变，太阳/材质编辑使对应结果失效。

交付：完整世界表面的直接漫反射与自发光查询，足以启动探针。

验收：物体离开屏幕后仍保留正确材质、直射阴影和自发光；转动相机不改变缓存的直射光照。

## 阶段 4：探针分类、SH 采集与时域更新

已完成，主场景运行记录及独立验收覆盖范围见下文：

1. 稳定 antipodal 球面分类从固定中心以 `tMin=0` 发射，默认 64 条，支持偶数 8—256。
   背面 hits/全部分类 rays 严格大于默认 0.25 时禁用；真实命中距离 `<= surface.ray_min_distance` 才判为不安全起点。
   靠近但仍大于偏置的探针保留；invalid/exhausted 禁用；探针始终不移动。
2. 有效探针从原中心以 surface rayMin 采集真实 hit-UV `Lo0`，正常 miss 为零；source/trace 失败禁用并清 SH。
   采光同样使用 antipodal 配对，按 probe index/成功 update count 旋转，不受相机或帧调度影响。
3. 4 个 RGB SH 保存未卷积的入射辐亮度，顺序 `[constant,X,Y,Z]`，基底常数 `0.2820947918`/`0.4886025119`。
   均匀球面投影为 `4*pi/N`；消费 `E/pi` 时 L1 乘 `2/3`，仅结果钳制，系数保留负值。
4. GPU record 96B、compute constants 128B；实际 GPU 状态不在 `World::Probe` CPU 分配模板中。
   RDG 完整 copy prev→next，按需 full reset→UAV barrier→选中批次更新，未选中记录保留。
   production `CSUpdateParallel` 每 probe 一个 64 线程 group，分类/采光均组内分工与 shared tree reduction，lane0写历史。
   dispatch 为 selected probe count，超 65,535 groups 分段设置 start/count；serial `CSUpdate` 保留为数值参考。
   默认 256 probes/frame、64 capture rays/update、history window 32；计数累计成功更新/射线数，历史权重在窗口上限后固定。
5. reset1 清分类和 SH，reset2 保留可靠分类、清 SH/计数；reason3/4 的失败记录重新分类。
   geometry/allocation/SDF、coverage、分类参数及射线区间变化触发 reset1，source-only/采光参数变化触发 reset2；预算/相机不清历史。
   仅 frame Signal 成功后推进批次、帧和历史索引，成功 reset 才增加 epoch，旧 epoch readback 被拒绝。

交付已写入：Details 的 Probe cache、视图 13 状态十字、14 八面体 `E/pi` 重建、15 年龄，均为固定中心 xray 诊断。
每 32 个探针提交帧及 reset 帧异步读回统计；timestamp 覆盖 full history copy 至批次更新结束，
含可选 reset/分类/采光，不含 shared scene 上传、统计 snapshot 或诊断绘制。

独立验收：CPU Debug/O2 5 组通过；真实 RTX 5070 Laptop GPU Debug/O2 13 组通过，D3D12 debug/GPU validation 0 warning/error。
最新报告数值 SH/`E/pi` 最大误差 `1.5479e-4`、真实 hit-UV 源 SH 误差 `9.3579e-6`、历史误差 `2.3842e-7`；
覆盖常量、有符号三轴、薄墙/盒内/贴面/安全间距、实际源光、批次/历史/reset/失败重试。
测试 primary entry 为 `CSUpdateParallel`，覆盖 8/64/128/256 rays，8/256 同时检查分类与采光分工边界。
40 条同输入 parallel/serial reference 记录的身份/计数一致、最近距离差为零，SH 归约差最大 `1.4305e-6`。
后台 CPU 场景集成另有 Debug/O2 `/W4 /WX` 3 组通过：真实 World/SDF 的非对齐 engine X+0.17/返回原变换
使探针数 4→5→4、epoch 1→2→3，中心/cells/节点/CSR 精确恢复，参考射线 hit→miss→原 hit，旧 epoch 被拒绝。
该 fixture 直接修改场景输入；真实 `ModelInstance`/`EditorTransform`/`EditorHistory` 的单事务 Undo/Redo 另外独立验证。
不把后台检查描述为实际 UI 按钮操作或完整 GPU 资源扩容验证。
第 13 组为 production `RenderDevice`/`VXGIGPUScene`/`VXGIProbePass`/RDG wrapper 集成，
最终 Debug/O2 均以 `/utf-8 /W4 /WX` 通过：3 slots、32 submissions，真实分配数 32→9→32；
978 条记录与独立 CPU 参考核对，source/history SH 最大误差 Debug `5.51343e-7`、O2 `5.36442e-7`，
旧 epoch snapshot 丢弃及完成汇总匹配，两个独立配置报告保留在 `tmp`。
不可见 fixture 窗口无 Present/交互，两个测试设备 0 warning/error；实际未观测 in-flight resize，不称为压力测试。
旧 serial 的 256-probe 批次只有 4 groups，密集车身实测约 230—240 ms，因此改为每探针一个组并行分类和采光。
最终 parallel Release 在 256 probes/frame、64 capture rays/update 设置下的几个 batch 采样为约 1.03—4.61 ms；
不是正式基准的 min/max 或分位数，不降低参数预算，也不推断性能倍数。
阶段 4 alpha 接入前的 53,306 个探针实际分类 valid 27,181、disabled 26,125（backface 26,123、unsafe 2、trace/sample failure 0），
约 51% 的已分配探针有效；阶段 7.2 后来补充旧版本的离散质心支持抽样。阶段 4 的 GPU cache 约 24.401596 MiB，不含 shared scene/资产。
旧版本 mip 0→1→0 实际使 epoch 1→2→3 并保留可靠分类；当前 mip 改变须重分类并清邻边。编辑器 Location.X 0→1 使 world revision 1→2、epoch 4，
探针数不变，重分类 valid 27,178、disabled 26,128。容量变化/返回原变换的后台 CPU 与独立 EditorHistory 检查见上文，未声称实际 UI undo。
Debug 100% 已检查旧 kernel 状态/缓存；最终 parallel Release 150% 已检查 SH、年龄、宽约 258 的 Details 和 F10。
最终 parallel 100% 仅检查启动默认视图/部分缓存；相机 epoch 稳定记录来自旧 kernel。
Shared scene 抽取后旧 SDF/surface 全屏诊断尚未重查实际画面，shared 路径由 headless 数值测试覆盖。
最终 Debug/Release 主工程与 UI 架构检查通过，后台集成检查完成；覆盖限制保留，细节见 `VXGI.md`。

## 阶段 5：可见性滤波与表面查询

已完成，实现及后台验收范围如下：

1. CPU 拓扑保存世界原点锚定的 16B lexicographic lookup、24B 六邻居记录，缺失为 UINT_MAX，
   轴顺序 `[-X,+X,-Y,+Y,-Z,+Z]`。同一 World 且尺寸/数量不变时只按 allocation revision 失效，替换 World 或改变尺寸/数量也刷新。
   拓扑 link 数不代表无遮挡边；材质/太阳/相机不重建拓扑或 SDF。
2. 几何边按 min-index→max-index 中心追踪闭区间 `[0,d]`，任何命中（含端点）、invalid/exhausted 都遮挡。
   4B mask 保存六个 visible bits、六个 failure bits 和 known bit；source-only reset 保留边，几何或 coverage reset 清边。
3. 滤波只读 raw self＋六个可见有效且已更新的 raw 邻居，自身权重 1、默认邻居权重 0.25；
   四个 signed SH 同权归一化，有效自身 metadata 保留，无效自身输出整条零记录，合法黑色记录仍有效。
   filtered SH 只供诊断，不作为真实表面查询源，避免 P→A、A↔B 可见但 P→B 被挡时从 A 借用 B 的颜色。
4. 真实表面查询用 `floor(P/voxel_size-0.5)` 的八个中心角点和普通三线性权重，逐个读取 raw、验证状态与可见性，
   剔除后重新归一化；没有 26 邻域回退。反向 probe→surface 从精确中心 `tMin=0` 起步，第一命中须 exact triangleIndex，
   距离和位置均确认目标端点。容差仅用于 endpoint verification/rayMax，不忽略目标整个三角形/物体，不做 normal offset。
   backFace 翻转原始几何法线后严格检查探针在正半球；mapped normal 只参与 SH `E/pi` 重建。
5. 48B 结果保留 unclamped/clamped `E/pi`、negative mask、原始 accepted weight、occupied candidate/visible accepted 数和失败数。
   黑色不等于 missing；无支持返回零和 hasSupport=0。已分配比例、有效比例与实际表面支持率分别统计。
6. RDG 可选 full reset→UAV barrier→选中六边协作计算→mask SRV→同批 raw SH filter，最后回到 pixel/non-pixel SRV。
   80B/20DWORD constants；阶段 5 交付时 compute root 为 42DWORD，7.3 加 material SRV/sampler 后为 44DWORD；表面诊断重用 `t4`，仍为 64DWORD。heap 在 root 之前。
   容量变化等待并核对全部 fence；成功 Signal 后推进状态；shared scene 身份与 revision 一起核对，staging 按槽 fence 释放。

交付已接入：视图 16 filtered SH、17 邻接可见性、18 表面查询 `E/pi`、19 支持权重、20 缺失/负值；
尚未接入场景间接光或 SSGI。第五步没有执行实际 UI/像素检查，不能从构建和数值测试推断画面验收。

CPU Debug/O2 `/W4 /WX` 3 组通过，覆盖稀疏/负坐标拓扑、权重/状态/metadata、正负 SH 先插值后钳制、合法黑色支持、
闭区间端点、薄墙红蓝隔离及三点可见性反例。三个 production CS 和两套诊断 VS/PS 的 Debug/O3 SM6.6 `-WX` 编译通过，
主工程 Debug/Release 与 UI 架构检查通过。
真实 RTX 5070 Laptop GPU Debug/O2 `/utf-8 /W4 /WX` 均通过 10 组，直接调用三条 production CS 与 common 查询，
881 条 signed SH filter records 最大误差零，19 个查询的 `E/pi`/支持权重误差最大 `1.1920929e-7`。
真实 production wrapper 使用 RenderDevice/GPUScene/Topology/FilterPass/RDG，3 slots、10 submissions，World/SDF 数量 72→25→72。
同容量短墙 height 0.2→0.8→0.2 在 lookup/centers/count 不变时正确清边并翻转 clear/blocked；weight 与 SH/source reset 保留边。
其 raw 输入为已知 signed SH fixture，独立验证第五步，没有重复阶段 4 的材质采光。
两套实际诊断 root/PSO 初始化通过；两台设备 InfoQueue 均为 0 warning/error，窗口始终不可见、Present 0。
[最新报告](<D:/项目/LearnDirectX/tmp/VXGIProbeQueryGPU-report.json>) 为 O2，另保留独立
[Debug 报告](<D:/项目/LearnDirectX/tmp/VXGIProbeQueryGPU-Debug-report.json>) 和 [O2 报告](<D:/项目/LearnDirectX/tmp/VXGIProbeQueryGPU-O2-report.json>)。
未实际观测 in-flight resize，不宣称在途扩容压力覆盖；这也不是主场景逐像素误差或实际 UI 验收。
阶段 5 交付时邻边首次计算/复用成本与汽车场景表面支持率未测量，阶段 4 时间采样不含这些新增 pass；7.2 后来提供 alpha 接入前的有限基线。
新增 DEFAULT payload 为每 probe 100B filter＋三帧各 40B topology；以 53,306 probes 估算约 11.184044 MiB，
不含 staging、对齐、纹理或其它共享场景数据，不是实际显存测量。完整端点容差与结果语义见 `VXGI.md`。

## 阶段 6：接入现有 SSGI

已实现，后台数值与 production wrapper 验收如下：

1. `vxgi.hybrid.mode` 严格接受整数 0—2，默认 2；模式分别为原屏幕 `Lo0`、屏幕加世界 `Lo0`、屏幕加世界 `Lo1`。
   世界数据缺失降为 0，只有 raw/topology 缺失时 2 降为 1；无可见探针支持的表面保留 `Lo0`。
2. 屏幕结果保存 Hit/Miss/OutOfScreen/RadiusLimit/IterationLimit/Invalid 和距离、命中像素、置信度。
   有效黑色、unlit 或源光查询失败的几何命中仍终止；不能根据 RGB 决定穿透到后方世界光源。
3. 混合接收点由 camera-near 沿同一像素射线的真实 SDF 首次命中确认，核对当前 jittered view/proj 的 D32 深度（至多 2 ULP）
   和 UV（每轴至多 `max(8*FLT_EPSILON, 0.01/sceneSize)`）。确认后用真实表面作为起点，屏幕/世界共用方向且不偏移；
   确认失败禁用世界回退。没有 raster primitive ID 时，极近且落入深度核对范围的两个表面身份仍无法严格区分。
4. mode 2 的屏幕命中也经 camera 首次命中确认，再对真实 triangle/endpoint 做第五步 raw 八角点查询。
   camera 深度核对与 probe 可见性的严格几何端点条件分别处理，后者不放宽、不忽略目标三角形。
5. screen/world 都按 `Lo1 = Lo0 + GIComputeSourceDiffuseWeight(albedo, metallic) * albedo * Iprobe` 添加间接源响应，
   `Iprobe` 已为 `E/pi`，不再除 pi、不乘 AO，不增加环境/镜面或探针反馈。真实材质采样 footprint、LOD 和阴影路径仍有差异。
6. 每条射线只采用一个分支，所有射线（包括 miss）平均为现有 `E/pi`，复用重建、时域、A-trous 和 composite。
   composite 对接收材质和 intensity 应用一次。屏幕 radius 0.5 与世界最大距离 1000 独立配置。
7. 实际 geometry/material/raw 注册进 RDG；trace root 为 18 DWORD，heap 在 root/args 前绑定。
   `SSGIHybridState` 按模式、源变化、SDF/source revision 与 probe reset epoch 清历史，普通 probe 分批更新保留历史；
   仅成功 Signal 后提交 source key、翻转历史并推进采样帧，失败 recording 不消费状态。

交付：现有 `SSGIPass` 已消费本帧 raw 探针查询，保留三种源模式用于对比。Details 控件复用 UI center，未实际打开检查。

CPU Debug/O2 `/W4 /WX` 各 3 组通过；production trace 及验证 shader 的 Debug/O3 SM6.6 `-WX` 编译、
主工程 Debug/Release 与 UI 架构检查通过。RTX 5070 Laptop GPU 的 Debug/O2 各 11 组、27 条射线、5 个 production CS 像素通过；
radiance 最大误差 0/`7.4505806e-9`，kernel 误差均为 0。
production wrapper 各 12 次提交、3 帧槽、2 次 resize，FP16 history 最大误差 `0.000945329666`/`0.000960946083`。
两个测试设备 InfoQueue 均为 0 warning/error，窗口始终不可见、Present 0，逐提交 wait/readback；
完整报告、非在途扩容与合成 GBuffer/SH 的覆盖限制见 `VXGI.md` 的第六步验收记录。
后台 fixture 检查屏幕外彩色源、黑色几何终止、source/receiver 响应与 pi 因子、相机旋转/抖动以及远处表面确认；
主场景转动相机、跨越屏幕边缘的实际亮度和性能仍待实测，不能用数值 fixture 代替画面验收。

## 阶段 7：编辑失效、性能预算与材质扩展

1. 已完成编辑失效核对：材质刷新独立于 VXGI 开关且先于本帧 source stamp；`SceneEditState` 检测注册、删除、TRS、override 和 flags。
   CPU Debug/O2 各 5 组联动测试、实际材质 GPU Debug/O2 各 10 组通过；9 次 readback 的 48B 记录逐字节一致，InfoQueue 0 warning/error。
   复用 revision、完整重建与成功提交机制，不增加新资产运行时加载或 raster 实例缓冲扩容。
2. 已完成 alpha 接入前汽车＋地面的有限基线：129,386 tris、53,306 probes，两轮各 209 批，valid 27,181。
   全场稳态 probe median 3.958/p95 12.217 ms，filter median 0.008576/p95 0.009728 ms；公开 DEFAULT allocation footprint 103.75 MiB。
   几何质心抽样汽车正面支持 126/256，不能作为屏幕像素覆盖率；主场景 SSGI 和整帧时间未测。
   保留 256/64 参数及 float32 距离节点；这些数字不是当前 7.3 性能，也不证明 60 fps 预算达标。完整数据、范围和复现 runner 见 `VXGI.md`。
3. 7.3 已将 alpha 接受放在 CSR 精确候选命中后、最近命中选择前，以真实重心 UV、固定显式 mip 和 `sampledAlpha*baseColorAlpha>=0.5` 判断。
   洞只拒绝候选，不推进 `tMin`，保留共面 opaque；opaque 忽略 alpha，invalid coverage 关闭穿透。
   serial/parallel 分类、capture、source/sun shadow、center edges、raw query、SSGI camera anchor/world fallback 与诊断共用覆盖追踪。
   full-triangle 节点/CSR 仍保守有效，不重烘焙；World/Surface coverage revision 使分类和全部 known edges 失效，RGB/emission/sun 只清 SH。
   成功 Signal 后才提交 pending key/history/coverage；filter root 加 `t9` 材料和 `s0`，为 44DWORD，诊断重用 `t4` 保持 64DWORD。
   CPU Debug/O2 各 5 组、15 shader entries Debug/O3 `-WX`、主 Debug/Release 已通过；alpha GPU Debug/O2 各 14 组、38 提交、37 外层请求、4 个 cached-edit 阶段通过，InfoQueue 0。
   Probe Debug/O2 各 14 组包含 alpha serial/parallel 分类与 capture；Query/Filter Debug/O2 各 10 组和实际 FilterPass 自动 coverage/部分批次缓存重算通过。
   Hybrid Debug/O2 各 12 组、30 helper rays、7 个实际 trace 像素通过，所有 GPU InfoQueue 0 warning/error；完整实测范围、报告与限制见 `VXGI.md`，不把直接 kernel fixture 说成完整 wrapper。
4. 7.4 在现有预算下完成 post-alpha 全场验收：53,306 probes，27,181 valid，未分类/硬 trace/sample failure 0；
   两轮各 209 批加 3 次 drain，稳态 Probe median 3.084/p95 13.576/max 22.460ms，公开 DEFAULT allocation 103.75MiB。
   真 alpha/primary 首命中验证后的汽车正面质心支持为 110/172，62 个缺失分为 28 个全禁用和 34 个半球/visibility 剔除，不能当屏幕支持率。
   新增真实资产 Shadow/GBuffer/HBAO/Deferred/SSGI/Skybox 的 640×400 离屏对照，原始 D32、同 RAW、四组相机和各模式 8 帧，含汽车完全出屏。
   mode2 保留实际 history 的四姿态、每姿态 4 次提交也已执行，属于离散相机移动流程检查，未证明平滑运动无闪烁。
   修复生产 HBAO clear 与 IBL SH buffer 初态 warning，主 Debug/Release 和离屏 debug InfoQueue 0 通过。
   实景四姿态 anchor 仍有 8%–38% 前景 mismatch，probe endpoint proof 有 miss/超容差拒绝但硬 invalid/iteration-limit 0，数值可用性门槛未通过。
   世界模式有限样本的离屏 RDG GPU 时间为数百 ms，实时性能未通过；下一步先修数值证明与查询开销，再扩展功能。
5. 7.5 已针对世界追踪性能加入密集格子 BVH：候选数大于 32 才加速、叶子最多 8 个，保守 bounds 覆盖重心容差与浮点误差。
   原始 CSR 与距离节点保留，GPU 复用已有 references SRV；invalid coverage 按原顺序重放，不改变 alpha、薄墙、严格端点或最近命中身份。
   BVH 独立 A/B 的 9 张最终 HDR 逐字节一致；Debug/O2 GPU 各 10 组、11,820 条射线输出逐位一致，debug layer/GBV warning/error 0。
   新增 `ssgi.trace_downsample_factor`，严格接受整数 2/4/8，全部 GI 工作/历史尺寸向上取整，full-resolution guide 映射贯穿重建和降噪。
   factor 改变清 SSGI history，失败记录不消费 source key；原深度金字塔继续逐级缩小 2 倍。
   当前项目选 8，C++ 默认仍为 2；这是配置预算，没有按 GPU 时间自动切换方法。Debug GI 调用保留符号并使用 O3，其它 shader 保留原策略。
   同 RAW 与 12 帧序列的 1707×1067 固定相机 mode2：factor2/4/8 的 Trace median 为 18.29/12.30/4.85ms，离屏 RDG median 为 20.50/16.96/6.67ms。
   当前档位 Trace p95/max 7.05ms、离屏 RDG p95/max 9.00ms；测试排除 probe 更新、DLSS、postprocess、UI、readback 与 Present，不推算主程序 FPS。
   factor8 最后一帧前景 mean RGB 差 `4.20e-4`、RMSE `2.04e-3`、最大局部差 `0.18384`，整体亮度差小但局部细节有变化；没有 ground truth。
   CPU 加速/配置/提交状态回归及 18 个 shader variants 编译通过；17×11 三档和 1×1 的 GBV 通过，warning/error 0。
   最终 Debug shader 策略 12 帧 Trace median/p95 为 5.036/11.114ms，离屏 RDG 为 7.005/21.339ms；这项使用 O2 native host，不代表 Debug CPU 吞吐。
   Debug 策略实际 Probe/Filter wrapper 回归通过，warning/error 0；全场两轮 209+209 批与 3 次 drain 当前分类仍为 valid27,181、disabled26,125、unclassified0。
   已分类轮 Probe median/p95/max 为 0.186/1.122/2.062ms，filter 为 0.00602/0.00646/0.00726ms，独立于冻结 RAW 渲染计时，不能推算整帧 FPS。
   当前公开 DEFAULT allocation 为 110.125MiB（增加6.375MiB）；完整测试范围、当前输入清单与报告见 `VXGI.md`。
   阶段 7.4 的相机确认误拒绝、probe endpoint proof 缺失和固定探针支持缺口尚未修复；分批调度仍需约209帧覆盖全场一次。
6. 尚未新增反馈。以后增加更多反弹时，探针只读取上一轮稳定缓存，独立验证能量收敛；局部脏区、更多光源、透明、骨骼变形或更大范围管理按实测决定优先级。

验收：模型编辑后旧遮挡和旧光照被更新；白色封闭测试场景的能量不持续增长；报告质量、时间和内存及其测试场景。

## 文件与工程组织

- `Resources/VXGIWorld.h` 管理完整世界三角形和稀疏格子/固定中心；CPU Probe 是分配模板。
- `Resources/VXGISDF.h` 管理 CPU 参考 BVH、无符号节点距离与候选 CSR；`Resources/VXGITraceAcceleration.h` 将密集格候选打包为 GPU 无栈 BVH。
  `Resources/VXGISurface.h` 管理材质/光照/采样失效状态。
- `Resources/VXGIProbeTopology.h` 管理 allocation-only lookup/六邻居与八角点权重；
  `Resources/VXGIGPUScene.h` 统一每帧 SDF/CSR/208B 三角形/12B 中心/拓扑的 DEFAULT 上传与 fence 生命周期，供 compute 和诊断共享。
- `Resources/VXGIProbeState.h` 管理事务批次、成功提交、source/history epoch 与异步 GPU 汇总；
  `Passes/VXGIProbePass.h` 管理 RDG 双 GPU 历史、copy/reset/采光、timestamp 和 snapshot；
  `Passes/VXGIProbeFilterPass.h` 管理几何 mask、独立 filtered 诊断记录与对应 RDG/reset/timestamp。
- `Shaders/VXGIMaterialCommon.hlsli` 共享 48B 材质布局，`VXGISDFTraceBody.hlsli` 保持几何/覆盖重载使用同一 traversal；
  `VXGISDFCommon.hlsli`、`VXGISurfaceCommon.hlsli`、`GISourceCommon.hlsli`、`VXGIProbeCommon.hlsli`
  分别共享追踪/表面恢复、命中材质与源光、screen/world 源响应、SH/分类/采光/reset；各诊断和更新 shader 独立。
- `Shaders/VXGIProbeQueryCommon.hlsli` 共享闭区间边、真实 endpoint visibility、六邻居滤波及 raw 八角点表面查询；
  `Shaders_For_VXGI_Probe_Filter.hlsl` 提供 reset/visibility/filter 三个 production kernel。
- `Resources/SSGIHybridState.h` 管理 SSGI 源身份与提交，`Resources/SceneEditState.h` 记录已注册实例编辑和 raster 矩阵 dirty；
  `Shaders/SSGIHybridCommon.hlsli` 共享首次表面确认、world `Lo0/Lo1` 与源响应。
- 现有 `SSGIPass.h` 与 SSGI trace/BounceSource 路径已接入混合追踪和消费本帧 raw 查询，重建/降噪/合成映射与可配置 GI 尺寸一致。
- `Tests/RunVXGITraceAccelerationTests.ps1` 和 `Tests/RunVXGIDenseBVHGPUTests.ps1` 验证 CPU 候选保守性和 GPU 旧/新追踪等价性；
  `Tests/RunSSGIResolutionConfigTests.ps1` 验证配置，`Tests/RunVXGIStage75Performance.ps1` 与 `Tests/CompareVXGIStage75Performance.ps1` 记录真实场景逐 pass 时间和同输入 HDR 差。
- 配置统一放在 `Settings/Pipeline.json` 的已有对象中：几何/探针参数放 `vxgi`，追踪预算放 `ssgi.trace_downsample_factor`，由 `Settings_Manager` 读取。
- 诊断控制通过 `EditorUICenter` 和既有共享组件创建；新增组件先进入 F10 style guide。诊断视图需要检查实际渲染。

当前探针只采集已知 `Lo0`，cutout 使用 7.3 的候选覆盖复核，miss 零局部辐亮度，无环境/镜面/AO/反馈。
阶段 1—6 已实现，阶段 7.1 与 alpha 接入前的 7.2 已交付，7.3 后台回归和 7.4 实景离屏验收工作已完成。
7.5 已降低最重世界查询的后台成本并接入分辨率预算；历史 7.4 数值问题未在本轮解决，不能据此宣布整个 GI 最终验收通过。
主程序含 probe 更新的整帧预算、主分辨率 DLSS/LOD 画面、严格端点数值可用性及平滑运动质量仍属后续。

## Mesh SDF 迁移（按模型包围盒进入局部求交）

本轮约定使用静态模型资产：射线先与实例包围盒求交，进入模型局部空间后复用当前 DDA、保守距离跳空、候选 CSR 和密集格 BVH/三角形复核。
模型移动、旋转和缩放只更新求交实例信息；不增加运行时几何修改检测、Invalidate/Rebuild/Reload 等接口。
多个模型的真实命中统一按世界射线距离比较；保留原有限 SSGI 射线，最近交点查询现有世界空间探针后返回。

1. **已完成：局部资产基础。** `Resources/VXGIMeshSDF.h` 从唯一模型的局部 LOD0 几何构建只读资产，按模型地址与烘焙设置提供进程内共享。
   不经过 `VXGIWorld::Update`，不创建世界探针分配，也不烘入实例 TRS、全局材质 ID、Cutout 或透明状态。
   `VXGISDF` 抽取共同烘焙流程；新增一次性资产初始化为 private，仅供 cache 构建。第 1 步结束时主程序只持有空 cache，仍使用原 Global 路径。
2. **已完成：实例表、射线换算与 CPU 几何参考。** `Resources/VXGIMeshSDFScene.h` 管理 TRS、保守世界包围盒、实例材质映射和稳定来源身份；
   `MakeLocalRay` 保留原点，独立提供归一化局部方向、距离比例与原有限区间。CPU `TraceGeometry` 只读遍历共享资产 BVH，临时变换候选包围盒/叶子，
   直接在原世界区间做 double 三角形求交和最近命中裁决；不持久构建世界几何，不依赖局部 float 首命中作为候选。
   当前两实例仍直接测试外层包围盒，没有新建实例 BVH；局部 DDA/距离跳空的 GPU 接入留在第 3 步。
3. **已接入：切换生产消费者。** 打包/上传共享局部资产和实例数据，接入 SSGI 回退、探针分类/采光、太阳遮挡、邻居可见性与诊断；保持 Alpha/材质映射和探针读取契约。
   主程序不再调用 Global SDF 的 Update；资产数据按唯一模型共享，实例变换只刷新实例缓冲。旧 Global 实现保留给独立历史对照测试。
4. **已完成后台验收：功能通过，性能结论保留。** 独立真实几何、有限射线、缩放/镜像、Alpha、跨实例最近命中及移动后的探针/邻边刷新通过。
   发现并修正第 3 步初版 Mesh 的剔除开销：紧致几何包围盒、局部单三角形包围盒和世界线段包围盒必要重叠测试，保持原世界区间和精确复核。
   1707×1067、同 RAW 的 SSGI median 从 44.56 降至 7.83ms，图像/深度/RAW 逐字节一致；完整统计及尖峰见 `VXGI.md`。
   同输入静态几何 kernel：Mesh 1.671ms、Global 0.680ms，Mesh 仍慢约 2.46 倍；当前两模型场景静态 GPU 数据仅少约 1.4%，不宣称静态性能全面提升。

第 1 步验证：新局部缓存 Debug/O2 各 6 组、原 SDF Debug/O2 各 4 组（每轮 700 次随机参考对照）、原候选加速 O2 5 组通过；
主工程 Debug/Release、编辑器架构检查、工程 XML、runner 语法与 diff 空白检查通过。未启动主程序、未操作桌面，也未执行 GPU 性能验收。
本步仅完成内存缓存，不包含磁盘缓存；不能据此宣称主程序已改用 Mesh SDF 或已提速。

第 2 步验证：新实例几何 Debug/O2 各 6 组通过，含 1072 次通用独立世界 BVH 对照，及局部射线端点往返、缩放端点、
零长度包含表面、排除过近候选后继续查询和 float 输出距离相同仍选最近实例的定向回归。CPU 参考不读取 Alpha 贴图，不能据此宣布 GPU coverage 等价。
最终主工程 Debug/Release 构建、Editor UI architecture 检查、工程 XML、runner 语法和 diff 空白检查均通过。
第 2 步结束时主程序只持有空实例 scene，未执行更新或上传；当时显示、SSGI、探针与 Global SDF 生产路径仍未切换。

第 3 步定向验证：资产打包 Debug/O2 均通过；Mesh GPU Debug/O2 各 11 组、1457 条射线通过，包含 Alpha、缩放/镜像、
严格原世界区间、跨实例最近交点和局部密集格 BVH/CSR。最大位置/距离/属性误差 2.38419e-6，Debug layer/GBV warning/error 0。
这些小场景求交测试不能代替第 4 步全场质量、稳态性能和初始化成本验收。
实际生产 Pass 后台小规模检查也通过：6 批 Probe/Filter、三帧槽移动/还原、64×40 四种 GI 模式输出有限 HDR，D3D12/GBV 0 warning/error。
移动及还原的资产烘焙/上传增加 0，仅各槽实例上传增加 2；主 Debug/Release 与工程检查通过。具体记录和未完整预热范围见 `VXGI.md`。

第 4 步最终记录：真实场景 28672 条射线及合成 Debug/O2 各 1457 条均通过；移动 Debug/O2 各 6 组，旧 epoch/snapshot、部分清理、分类/SH/邻边刷新与还原通过。
32 轮全场采样加 3 次排空提交后，53306 探针全部分类，27181 valid 均至少 32 次更新，trace/sample failure 和非有限 SH 为 0。
四个静态视角的纯 GI/合成图已离屏输出并检查，所有优化前后合成 HDR/深度完全相同；仍是低频补充，没有高频画质或平滑相机运动合格的证明。
原生 Debug 冷启 256 帧中 World/资产各只构建一次（资产共 2 份），初次 CPU 阶段约 3.62s；随后无重烘。零 D3D12 验证警告，无可见窗口/Present。

## SSGI 回退简化：实例 BVH、最近两个模型与 SDF 阈值命中

按新的约定，SSGI 失败后只查询原有限世界射线；先用模型实例 AABB BVH 选进入距离最近的两个候选，再在这两个局部 SDF 内步进，
距离达到阈值即接受近似命中，取最近世界命中位置采现有探针并返回。SSGI 消费路径不再进入三角形候选/BVH、逐三角形世界变换或 FP64 精确求交。
探针缓存独立更新。最近两个 AABB 是有意的近似预算；未入选模型不会在该次查询中补查。

1. **已实现：世界实例 BVH 与最近两个候选。** `Resources/VXGIInstanceBVH.h` 维护模型几何 AABB 的树；`VXGIMeshSDFScene::UpdateInstances` 自动维护，
   通过 `QueryClosestTwoInstances` 提供 CPU 候选参考和 32B 紧凑节点。静止/材质/源数组排序复用；TRS 变化 refit，实例成员变化 rebuild；资产不重烘。
   第一步结束时只建立 CPU 数据与参考查询，GPU 仍沿用原有求交；不把建树说成已切换 SSGI。
2. **已实现：可直接命中的距离场采样与步进。** CPU `VXGISDFThreshold` 与独立 HLSL helper 直接复用资产的 unsigned 节点，
   三线性距离不大于 `0.9 × localCellSize` 即接受；插值误差下界只用于安全步长，不拿下界为零判命中。
   保留紧模型 AABB，不为阈值扩盒，也不把 padded 网格当作模型盒；每次只在原射线、候选盒及局部场的交集内查询。
   新局部查询使用 FP32、最多 512 次采样，并区分 miss、hit、invalid、exhausted、stalled；CPU top-two wrapper 验证跨实例世界距离选择。
   没有三角形/材质读取，没有资产重烘；阈值是格距尺度的近似，当前格距 1 的细节限制仍在。第二步结束时提供 GPU helper，尚未切换 SSGI。
3. **已实现：SSGI 消费端切换。** 三帧槽独立上传实例 BVH（slot 10），GPU FP32 查询最近两个紧 AABB，再查局部 unsigned 距离场。
   原有限射线不延长，阈值命中后只读取现有 raw 探针 SH 并返回；黑色或无支持不继续搜后方。
   生产 Trace 使用 `VXGI_SSGI_THRESHOLD=1`，只绑定节点、lookup、raw、资产、实例及 BVH 六个 SRV；移除这条回退的三角形/CSR/材质资源及 FP64。
   旧精确求交仅供探针独立更新及历史验证；CPU/HLSL 新布局、上传版本、根绑定和生产回退均已后台验证。
4. **已完成测量与验收记录：后台质量与性能。** 同一充分预热 RAW、原生 Debug、当前 quality/radius/factor 下完成新旧 A/B/B/A、纯 GI/合成四相机对照及实际几何诊断。
   1707×1067 Trace GPU 中位数 7.055→0.617ms，完整 SSGI 7.586→1.130ms，离屏 renderer 9.184→2.719ms；每版保留 36 帧。
   实现不变量及性能改善通过；当前阈值的质量风险未消除：车体向外表面诊断中 4078/4096 条射线在起点接受自身，纯 GI 车身补光明显更强、更均匀。
   两种方法并非相同求交语义，不能把更亮或更快当作物理画质正确；后续需单独确定起点阈值内的处理方式。本步未修改生产参数或默认方法。

第一步定向验证：实例 BVH Debug/O2 各 7 组、1924 次独立 AABB 对照、4096 次零分配查询通过；Scene Debug/O2 各 8 组，
保留 1072 次世界几何对照并新增 BVH 静止/材质/排序/TRS/成员与失败回滚验证。本步未修改 Shader、GPU buffer 绑定或画面，不作帧率收益结论。
主工程 Debug/Release、编辑器架构、工程 XML、runner 语法和差异空白检查通过。

第二步定向验证：距离查询 Debug/O2 各 6 组（512 次独立插值参考、524 次有限命中），Scene Debug/O2 各 10 组；
新局部 GPU helper Debug/O2 各 1807 条，最大误差 `7.1525574e-7`，D3D12/GBV warning/error 0，DXIL 无 FP64 或旧几何/材质资源。
主 Debug/Release 和工程检查通过。没有桌面操作、生产路径切换或当前帧率收益结论；完整约定与复现命令见 `VXGI.md` 第 2 步记录。

第三步定向验证：Debug/O2 新集成查询各 1549 条及 6 个生产缓存读取案例通过；最大误差 `1.3828278e-5`。
生产 shader 精确宏审计为 624B 常量和六个固定 SRV，无旧几何/材质资源或 FP64。Probe/Filter 三帧槽移动与 BVH 生命周期 Debug/O2 各 11 组通过，
D3D12/GBV warning/error 0。主 Debug/Release、工程 XML、runner 语法、编辑器架构及差异空白检查通过。
真实汽车/地面的生产 SSGIPass 离屏 64×40、四模式各 2 帧通过，三帧槽均为当前 3 节点 BVH，每槽上传一次 96B，HDR 全部有限、GBV 0 warning/error、Present 0。
仅预热 6 批，属于接入检查，不作为全场收敛、画质或性能验收；第 4 步随后使用已充分预热的冻结 RAW 完成测量。

第四步记录：实际模型 GPU Debug/O2 各 32,768 条射线，与 CPU 阈值查询无状态/实例/超容差差异，D3D12/GBV 0 warning/error；
两模型场景相对逐模型阈值查询的 top-two 漏查为 0，复杂空框第三模型的预期漏查仍按先前测试成立，不能据此推广到更多实例。
640×400 四相机、每模式 12 帧同 RAW 的纯 GI 和合成图完成；背景、光照与深度保持一致，前 3 视角纯 GI 前景约 28%–33% 像素改变。
跨后端屏幕模式有少量浮点差异，已定位 DXIL fast-FP 标记不同；同版 camera2/mode0 重复抓图逐位一致，未发现运行不稳定。
本轮只增加验收专用编译分支与工具，默认生产仍使用阈值回退。详细来源、限制、图像和复现命令见 `VXGI.md` 第 4 步。

## 探针事件驱动缓存

生产主流程已启用 `VXGIProbeCache`：首次分类/采光达到现有目标后停更；模型旧、新位置、材质/coverage、太阳光事件生成去重工作队列。
固定世界坐标 remap 复用幸存缓存；每探针 generation 拒绝拖动期间过期的异步反馈；全体失效 SH 立即清理，重采样分帧进行。
静止 clean 时 probe/filter 不 dispatch、不复制 history、不新增反馈读回。原目标 32 保留，首次填充仍需预算时间。
依赖粗 mask 覆盖分类/采光/miss/太阳阴影与邻边线段；域扩展或未知在途路径采用保守刷新。它只用于失效判断，不改变 Mesh SDF 精度。
活动批次的全 history copy 和 CPU World 分配重建仍保留；本次没有宣称所有编辑都只修改 GPU 子区域。
后台验收记录与命令见 `VXGI.md` 文末，原循环更新与历史窗口不停止的旧描述属于历史实现。

本次后台验证：scheduler/失效控制器 CPU Debug/O2 各 8 组；新 lazy GPU Debug/O2 各 8 组，64 idle 帧 raw/filtered/edges 逐字节一致、零缓存更新；
移动保留 244 个远处记录，17 探针预算立即清全部脏 SH，488 个过期工作被拒绝；旧 Mesh edit GPU Debug/O2 各 11 组通过。
GPU 均无 D3D12/GBV warning/error，无可见窗口/Present；主 Debug/Release 与工程检查通过。小场景参数和真实 FPS 未验收的范围见 `VXGI.md`。

懒缓存 GPU 空闲性能复测已完成：Native Debug、内部 1707×1067，所有 valid 探针精确到 32 次后 96 帧更新/复制/反馈计数不变。
72 个有效稳态 GPU 样本：SSGI.Trace 0.921 ms、完整 SSGI 1.425 ms、完整 HBAO 0.864 ms；
单独实际 HDR/depth 复放的 DLSS Quality 中位数 1.660 ms、ToneMap 0.059 ms，DLSS 是覆盖范围内最大单项。
填充期探针 CPU 最大单项为依赖登记 0.429 ms；静止 Cache Prepare 0.0067 ms。此前并发 GPU 负载下的 6–12 ms Trace 不作为代码回归结论。
已知 clear-value 性能警告 820 保留统计，错误/意外警告为 0；完整口径、限制和复现见 VXGI.md 文末及 tmp/VXGI-Lazy-idle-gpu-performance-report.md。

SSGI失败直接结束 vs VXGI回退的直接A/B已测：同生产PSO/充分填充RAW/固定视角/NativeDebug，连续ABBA×2，每种模式512有效帧。
Trace 0.388496→0.622928ms（+0.234432）；完整SSGI 0.893152→1.127264ms（+0.234112，约26.2%）。
非GI基线一致；该增量只代表静止消费，不含初次构建、Probe/Filter生产、动态编辑、DLSS/UI/Present。完整报告及重现见VXGI.md文末和tmp/VXGI-world-fallback-ab-report.md。
