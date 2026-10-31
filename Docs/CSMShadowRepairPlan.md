# CSM 级联交接与移动半影修复策略

2026-11-01。状态：步骤 A（世界锚定）、B（级联重叠与混合）已实施；步骤 C 中receiver-plane梯度尺度修复已实施，其余PCSS搜索稳定性改造待根据残留现象决定。

步骤 A 验证：直接调用生产 `ShadowPass::PrepareFrame` 的 Debug/Release 各 688 项 CPU 断言通过，旧生产头文件负对照会在分数 texel 漂移断言失败。9 次实际渲染捕获通过，包括固定朝向前后移动、Forward 和近处墙角，D3D12 0 警告/错误。详细证据见 [世界锚定验证报告](../tmp/shadow-world-anchor/REPORT.md)。这一步未消除尚无过渡带的级联接缝。

步骤 B 验证：默认过渡比例0.1，实际扩展三条过渡带，增加搜索/比较核覆盖余量，共享完整 PCSS 后混合。Debug/Release 各900项CPU断言、62个Shader变体通过；两种构建各16次真实渲染，Debug的16次调试层运行0警告/错误。GPU诊断覆盖5/15/50三条交界，独立重建视深度后，实际遮挡与两级结果的预期混合最大误差约0.000702（原始输出FP16）。这支持半影交接硬切得到平滑，不能认定所有空间折角已消失。详见[级联混合验证报告](../tmp/shadow-cascade-blend/REPORT.md)。PCSS搜索策略与随机核的相机相关波动仍未处理，过渡带额外GPU开销尚未量化。

用户补充机位 eye=[2.5680,2.2146,8.8899]、pitch62.5/yaw0.75，已存入 `Tests/Fixtures/ShadowCornerCamera.json`。外围折角仍可复现；级联编号捕获整张为第一级，没有级联交接。关闭软阴影过滤后硬轮廓正常，固定过滤半径后外围异常折角明显消退，关闭receiver-plane补偿仍有残留。应继续检查PCSS的blocker深度估计与变半径行为，而非扩大级联混合区。详见[补充机位诊断](../tmp/shadow-corner-followup/REPORT.md)。完成标准尚未达到。

目标是解决两个已复现的问题：远处阴影边缘在级联界线上出现额外小角；保持灯光和模型不变、固定朝向移动相机时，半影出现硬切及额外波动。墙角薄片漏光是另一项问题，不能用加厚模型代替这里的级联修复。

## 证据和参考范围

主要引擎参考是本机 `D:/UE_5_8`，`Engine/Build/Build.version` 确认为 5.8.4。下面对照的是其传统方向光 CSM 与可选 PCSS 路径，并非 Virtual Shadow Maps。该版本 `ShadowRendering.cpp:189` 的 `r.Shadow.FilterMethod` 仍注明默认 Uniform PCF、PCSS 为 experimental，不能将该分支描述成 UE 所有阴影的默认行为。

公开资料：

1. Zhang / Sun / Nyman，[Parallel-Split Shadow Maps on Programmable GPUs，GPU Gems 3 Chapter 10](https://developer.nvidia.com/gpugems/gpugems3/part-ii-light-and-shadows/chapter-10-parallel-split-shadow-maps-programmable-gpus)：分割视锥、分别拟合和投影。此轮保留已有四级与分割距离，先排除交接及稳定性缺陷。
2. Microsoft，[Cascaded Shadow Maps — Blend between Cascades](https://learn.microsoft.com/en-us/windows/win32/dxtecharts/cascaded-shadow-maps#blend-between-cascades)：在过渡区域求两级阴影，再插值测试结果；大部分像素仍只采一级。
3. NVIDIA Myers / Fernando / Bavoil，[Integrating Realistic Soft Shadows Into Your Game Engine](https://developer.download.nvidia.com/assets/gamedev/docs/PCSS_Integration.pdf)：blocker 搜索、半影估计、PCF 三阶段，以及逐样本深度梯度补偿。附录中的有限光源比例不能直接当作方向光公式照搬。
4. MJP，[A Sampling of Shadow Techniques](https://therealmjp.github.io/posts/shadow-maps/)及其作者示例：解释 Valient 稳定 CSM 的包围球拟合和 texel 对齐。本轮未取得 ShaderX6 原章全文，不宣称读过该原章。

UE 5.8.4 源码证据：

| 问题 | 实际机制 | 本机位置 |
|---|---|---|
| 覆盖重叠 | 用区间长度乘过渡比例得到 `FadeExtension`，非末级实际扩大 `SplitFar`；构造函数比例为 0.1 | `DirectionalLightComponent.cpp:949–991,1056` |
| 混合对象 | 投影 Shader 算出阴影后，把深度过渡权重输出到 Alpha；渲染器用 SourceAlpha / InverseSourceAlpha 混合遮挡缓冲 | `ShadowProjectionPixelShader.usf:267–270,424–428`，`ShadowRendering.cpp:824` |
| 投影稳定 | 光空间基变换不含平移；将 shadow position 对齐并反变换回世界位置，更新 PreShadowTranslation | `ShadowSetup.cpp:1057–1100` |
| 方向光半影 | CPU 用半光源角及深度范围/覆盖宽度换算；Shader 用 receiver 与 blocker 深度差估计半影，并限制核大小 | `ShadowRendering.h:1692–1703`，`ShadowPercentageCloserFiltering.ush:122–154,266–279` |
| 采样噪声 | Sobol 的种子来自屏幕位置及帧索引 | `ShadowPercentageCloserFiltering.ush:141` |

注意 UE 通过多次阴影投影及遮挡缓冲混合实现交接。本项目是全屏 Deferred/Forward 材质 Shader 内直接采样，宜在共享函数内双采样后 lerp，不必增加一份全屏阴影遮挡缓冲。

## 修复前实现的结论

**已由画面验证：** `LightingCommon.hlsli:88` 在 5/15/50 处直接换级，没有混合。固定截图相机后，级联着色界线与额外小角吻合；固定第二级后小角消失，移动选择阈值后界线移动。证据为 `../tmp/cornell-edge/FAR-REPORT.md`。

**已由移动捕获验证：** 原图的半影变化包含级联硬切；固定第二级后也还有波动。不能把所有模糊变化都归因于级联，也不能要求屏幕中的半影宽度不随透视缩放。证据为 `../tmp/cornell-edge/MOVE-REPORT.md`，21 次捕获均无 D3D12 警告/错误。

**新增源码与坐标分析结论：** 当前 `ShadowPass.h:70–81` 让 `lightView` 随相机平移，然后在这个坐标系里对齐级联中心（`124–126`）。固定朝向平移时，视锥中心和光空间原点会一同平移，变换后的中心不变；因此对齐值也不变，但世界点的阴影 UV 仍连续漂移。数值代入现有变换关系，移动 1/4 个阴影格后，同一世界点仍漂移 1/4 texel；这是坐标分析，不是新增 GPU 捕获。证据为 `../tmp/cornell-edge/projection-anchor-analysis.json`。

**应保留的正确部分：** 当前 `filterRadius = max(zReceiver - avgBlockerDepth, 0) * depthRange * tanSunAngularRadius / orthoWidth` 已把深度差转换成世界距离，再转换为 UV 半径。它不是直接用相机距离决定半影，不应为了修 CSM 而推倒。设置中的太阳角已经是半角，不能再照 UE 的 full-angle 接口额外除二。

## 实施顺序

每一步单独记录结果，合格后再叠加下一步。诊断时先保持四级 4096、分割 `[0.1,5,15,50,100]`、太阳角、16 次 blocker + 16 次 PCF、现有 Bias 与末级 90–100 衰减，避免将分辨率、Bias 调参的影响混进来。

### A. 先修正世界空间格子的对齐

- 使用只由灯光方向决定的光空间旋转基，级联中心变换到固定参考后再按 texel 对齐；平移必须在对齐后的矩阵或等价的世界空间 shadow center 中体现。
- 保留包围球拟合及稳定的投影尺寸；固定 FOV、宽高比、分割、灯光时，世界格子只应按整数 texel 平移，不能跟着相机发生任意亚 texel 漂移。
- 同步更新采样矩阵、绘制矩阵以及投影物剔除使用的 `lightView` / `cascadeShadowAreas`。不能只改采样坐标。
- 小幅平移、反向平移、负坐标和相机旋转分别检查；不承诺改变 FOV 或太阳方向后仍保留同一格网。

涉及：`Passes/ShadowPass.h`、`PBR_Render.cpp` 的阴影帧准备和 caster 剔除。

### B. 加入真实重叠覆盖及最终阴影混合

采用与 UE 相同的“向非末级远端扩展”约定，避免 CPU 和 Shader 采用两套不同的交接定义。

设逻辑分界为 `S_i`，上一逻辑近界为 `S_(i-1)`，初始过渡比例 `f=0.1`：

```text
B_i = f * (S_i - S_(i-1))
过渡区 = [S_i, S_i + B_i]
第 i 级实际渲染远界 = S_i + B_i
第 i+1 级正常近界 = S_i
```

因此三条初始过渡带是 `[5,5.49]`、`[15,16]`、`[50,53.5]`。10% 是起始设计值，非已验证的最优值；只在 `Pipeline.json` 中新增 `shadows.cascades.transition_fraction`，不新增配置文件和面板。

核心实现约束：

1. 逻辑分界与实际渲染边界分开保存；实际扩展范围参与包围球、矩阵、深度绘制和 caster 剔除。
2. 光空间 XY 还需给 **最大 blocker 搜索/PCF 核、硬件双线性采样及 snapping 位移** 留安全边。仅把视锥 Z 区间加长并不能保证核不越界。
3. 共享单级函数分别完成各级变换、receiver-plane 补偿、blocker 搜索和 PCF。过渡带里计算 `s0`、`s1`，用 `t=saturate((viewDepth-S_i)/B_i)` 得到 `lerp(s0,s1,t)`。
4. 带外保持单级采样。末级只做既有远处衰减，禁止访问第五级。不要混合深度、矩阵、blocker 均值后只执行一次比较。
5. `ddx(worldPos)` / `ddy(worldPos)` 在动态选择之前计算；两级分别用自己的矩阵变换导数，不能对跨级拼接的 UV 再求导。
6. 检查两个级联各自的有效范围和核边界；不能把越界返回的全亮值当作正常结果参与交接。有效范围不足时应修覆盖，不能靠权重掩盖。

共享入口建议为 `SampleDirectionalShadow(worldPos, worldPosDDX, worldPosDDY, viewDepth)`。Deferred、Forward/透明材质都使用它；Deferred 的反照源也沿用同一个混合后的 shadow。增加过渡常量时同步 CPU/HLSL 的完整 PassConstants 布局与对齐，防止尾部纹理索引错位。

涉及：`Passes/ShadowPass.h`、`Passes/RenderStructs.h`、`PBR_Render.cpp`、`Shaders/LightingCommon.hlsli`、Forward/Deferred 调用点及所有重复的 PassConstants 声明。

### C. 处理 PCSS 自身的相机相关波动

补充机位已发现一个应优先处理的具体错误：receiver-plane导数叉积未归一化，却被固定1e-6分母阈值压住；在该机位补偿仅剩约1.4%至5.3%。隔离Shader仅降低该阈值即可消退外围假轮廓，且存在半径与blocker数量都不变、错误遮挡消失的样点。应先修正这一尺度错误并验证退化保护，再评估下列搜索稳定性改造是否仍必要。见[补充诊断的后续发现](../tmp/shadow-corner-followup/REPORT.md)。用户已撤回“接收面越远越明显”的观察。

该尺度错误现已正式修复：共享函数先移除导数及叉积幅度，再保留原相对斜角限制；零、共线和非有限导数单独处理。32组GPU解析平面用例、62个Shader变体、Debug/Release构建、7次新机位/分辨率/Forward/负对照捕获及16次原级联验证通过。新机位1280x800与2560x1600实际画面均消退外围假轮廓，近处薄墙漏光仍在。详见[正式修复验证](../tmp/shadow-receiver-plane/REPORT.md)。下列搜索策略是后续候选，不等于全部已实施或仍全部必要。

这一步与 B 分开验证。两张图使用同样的世界半影半径，仍可能因深度图和采样位置不同得到不同结果，混合不能让这种差异凭空消失。

- 保留现有方向光半影深度差公式。优先核查 `zReceiver * depthRange` 作为搜索距离的做法：它以每级的裁剪近面为起点，这个起点随相机/级联变化，没有固定物理意义。
- 提议把搜索用的 caster-near 深度放到固定光空间中，由该方向光可投影的场景 caster 范围推导，并让各级共享。接收点到该界的距离转换为世界搜索半径，再按各级覆盖宽度转 UV。此方案是针对本项目的改造建议，不能声称 UE 已采用完全相同的策略。
- 搜索界限必须覆盖承诺支持的投影物范围，并与剔除预算一致。全场景界限可能偏松，必须检查有限样本下的 blocker 漏采；不能为追求稳定而随意缩成一小圈。
- 先用固定旋转的同一组 Poisson 样本作对照，去掉 `Rand(shadowUV)` 引入的相机相关核旋转。如果出现定向条纹，再验证世界/固定光空间中的稳定扰动。保留样本数，先不以加样本掩盖问题。
- UE 的屏幕+逐帧 Sobol 不是这里的直接替换件。本项目必须在无 AA 时也能看出改善，不能先加入逐帧噪声，再依赖 DLSS 才看起来稳定。
- 对最小抗锯齿核与最大软阴影核分别记录世界半径和 texel 半径，避免各级的独立 texel 上限在真实半影上引入新的尺寸断层。保留逐样本 receiver-plane 补偿，Bias 调整另做单变量验证。

### D. 验收与开销记录

1. 先复用已完成的固定相机和捕获，不重复找一个更有利的新镜头：eye `[-0.1158,3.2589,7.0816]`，pitch40/yaw50.25，FOV45；固定朝向沿视线移动 `[-1,-.5,-.1,0,.1,.5,1]`。
2. 同时保留原始画面、级联编号/过渡权重、两级独立 shadow、最终 shadow、世界半影/搜索半径。检查分界两端的值连续，权重和为1，没有缺影亮带或宽范围双轮廓。
3. 跟踪同一世界位置，测世界空间半影的10%–90%宽度及时间波动；与未跨级时的采样噪声比较。不能拿屏幕像素宽度自然随距离增加当作失败，也不要求16样本原始结果逐像素完全不变。
4. 继续覆盖15、50、100附近的交接/远淡出；检查正反向平移、侧向亚 texel 平移、转向、不同FOV、斜面、cutout、移动投影物及屏幕外投影物。
5. Forward 与 Deferred 在同一表面使用一致的阴影因子，屏幕反照源使用相同遮挡；近处墙角不能新增明显漏光，受光面不能新增自阴影条纹。既有近处薄墙漏光不能被冒充为本轮已解决。
6. Debug / Release、相关 Shader 变体、D3D12 Debug Layer/GBV 检查通过，并检查实际捕获。
7. 四张4096纹理保持不变，但扩展覆盖可能增加绘制对象；过渡区最多算两次PCSS，现有配置名义采样调用由32变64（不等于硬件实际texel读取数）。记录过渡像素比例、caster/三角形数量、Shadow Pass 与 Deferred/Forward 的 GPU 时间；不预先承诺“无性能成本”。

## 完成标准

交接小角和额外硬切消失；固定场景下相机移动引起的半影异常波动降低到可解释的采样噪声范围；没有以大面积柔化、全亮回退、缺影或额外自阴影换取平滑。达到这三点后再决定是否需要改变级联分割、样本数或 Bias。
