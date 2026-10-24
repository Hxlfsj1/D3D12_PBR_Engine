# 模型选择与描边

## UI 创建中心

所有面板及控件统一通过 `EditorUICenter.h` 创建，颜色、字体和尺寸集中在 `EditorTheme.h`。参照本机 UE 5.4.4 Starship 源码，使用随项目分发的 Roboto 字体。字号按 DPI 缩放，不再跟随窗口宽度变化。Outliner、Details、Output Log 和 File / Edit / Debug 菜单均使用统一组件；搜索框实际过滤对应面板内容。

按 **F10** 打开或关闭 UI 创建中心的组件模板窗口。新 UI 的接入方式和参考源码见 [STYLE_REFERENCE.md](STYLE_REFERENCE.md)。MSBuild 自动执行 `CheckUIArchitecture.ps1`，阻止功能面板绕过中心直接创建 ImGui 控件。模板是 ImGui 对 Starship 的适配，字体和语义色有明确来源，图标与布局不是 UE 整个窗口的逐像素复制。

顶部 **Edit → 相机** 在鼠标悬停时显示当前相机的位置、旋转、固定单位缩放、FOV、裁剪距离与视口大小。点击 **Copy camera JSON** 可复制完整机位数据，直接粘贴用于复现测试；面板只读，复制不会关闭，点击外部关闭。相机使用渲染器原生的左手 Y-up 坐标，旋转 XYZ 分别是 Pitch、Yaw、Roll（度），与下述模型 Details 的 Z-up 轴映射不同。复制的 JSON 包含精确的 eye、forward、up、target 和投影参数，不受界面小数位显示影响。

顶部 **Debug → Pass** 按完整逻辑 Pass 查看结果。每个启用的 Pass 可以选择 **本步结果** 或 **截至本步**，以该 Pass 支持的模式为准。GBuffer 在同一个 Pass 内选择 Albedo、Normal、ORM、Emissive 或 Depth 附件；Shadow 将四级阴影深度排为 2×2 图。HBAO 与 GI 不展开追踪、滤波等内部步骤。仅抗锯齿和后处理只提供累计画面；TAA、TSR、DLSS、SMAA 共用一个抗锯齿入口，ToneMap 归入后处理。SMAA 实际在 ToneMap 后执行，其他三种在 ToneMap 前，快照遵循实际执行位置。

未启用或不属于当前渲染路径的 Pass 置灰并显示“未启用”，失效选择回到默认。查看操作不启用功能、不修改 JSON；正常 GI 计算与历史继续更新，阶段快照单独用于显示。GI 历史不接受诊断图，颜色抗锯齿历史在视图或来源切换时重置。GBuffer 原色按线性颜色显示，法线保留编码映射，AO 灰度显示，深度线性化后以对数压缩距离显示，运动向量单独可视化。GI 本步的数据来源是滤波后的 `E/pi`，不乘接收面的材质响应；纯 GI 显示随后经过下述抗锯齿与 ToneMap。Skybox、Transparent 的本步结果表示真实线性 HDR 的 `阶段后 − 阶段前` 颜色变化：橙色为变亮，蓝色为变暗；原始文件保留正负值。它们的累计模式显示正常场景颜色。

GI 子菜单只有三个条目，依次为 **默认（SSGI + Mesh SDF VXGI + Global SDF GI）**、**SSGI**、**SSGI + Mesh SDF VXGI**。默认项的完整标签在窄窗口自动换行，字体大小不变。Global SDF 未启用时，默认项显示 **默认（SSGI + Mesh SDF VXGI）**；VXGI 未启用时显示 **默认（SSGI）**，Mesh SDF 项置灰；GI 未启用时整个入口置灰。诊断来源使用独立输出及历史，切换诊断来源时重置该历史，正常混合输出继续更新。GI 累计画面仍可由 CLI 捕获。

三个纯 GI 画面经过配置中的抗锯齿与 ToneMap 后显示；截至本步仍查看 GI 合成边界的累计快照。抗锯齿沿用 `Pipeline.json` 的选择，`None` 保持关闭，不自动改成其他算法。GI 菜单下方明确显示“纯 GI 输出：抗锯齿已启用”或“纯 GI 输出：抗锯齿未启用”。正常混合与诊断来源的 GI 历史隔离，切换查看来源时重新开始颜色抗锯齿历史，避免沿用其他画面的颜色。

F10 模板复用同一个菜单组件，可切换 SSGI、VXGI、Global SDF、AA 等可用状态检查正常、禁用和累计样式。

有限帧后台复现也走同一条生产渲染与保存路径。例如：

```powershell
.\x64\Debug\LearnDirectX.exe --hidden --frames=8 --capture-frame=6 --pass-view=gi --pass-result=output
.\x64\Debug\LearnDirectX.exe --hidden --frames=8 --capture-frame=6 --pass-view=gi --pass-result=output --pass-gi-source=ssgi
.\x64\Debug\LearnDirectX.exe --hidden --frames=8 --capture-frame=6 --pass-view=anti-aliasing --pass-result=accumulated
.\x64\Debug\LearnDirectX.exe --hidden --frames=8 --capture-frame=6 --pass-view=gbuffer --pass-channel=normal
```

参数不更改配置中的功能开关。`--pass-view` 使用 `RenderPassCatalog` 的稳定 ID；未启用的目标仍回到默认。正常启动无需这些参数。有限帧验收将渲染、保存和 D3D12 检查结果写入 `PassViewRun.json`；它在驱动清理前生成，进程是否完整退出仍须单独检查。

GI 本步结果可使用 `--pass-gi-source=default|mesh-sdf|ssgi` 选择正常混合、SSGI 加近段 Mesh SDF 或仅 SSGI，省略时为 `default`。该参数要求 `--pass-view=gi --pass-result=output`，不会更改功能开关。

后台验收通过 `--capture-frame=N` 在运行工作目录的 `Captures/<pass>-<frame>-<unique>/` 中生成 `preview.png`、原始 `.raw` 和 `metadata.json`，默认画面也可导出。截图是管线结果，不包含编辑器面板、描边、操作轴或后续 VXGI 诊断叠加；原始文件保存所选输出资源本身：三个纯 GI 输出是经过配置抗锯齿与一次 ToneMap 后的全输出分辨率 RGBA8，不再是 GI 内部半分辨率 `E/pi`；截至本步仍保存 GI 合成边界的 HDR 资源。其他阶段保留各自的资源格式，深度与阴影不经色调映射，Shadow 的四级全部保存。元信息记录每个文件的 DXGI 格式、尺寸、行字节、mip、数组层，以及相机、逻辑帧号、查看模式、渲染路径、抗锯齿算法和关键配置。复制使用正常帧的 fence，完成后再读回，不增加全局 GPU 等待。GI 元信息另外记录 `gi_output_stage`（`after_configured_aa_and_tonemap` 或 `after_gi`）与 `gi_preview_aa_enabled`，便于区分纯 GI 显示输出和 GI 边界快照。导出成功或失败显示在 Output Log。

Outliner 和 Output Log 使用 Unity 式深浅交替行底色，颜色仍由统一主题管理。搜索后按显示顺序重新交替，模型选中与悬停高亮覆盖行底色。

Console 单击一行（包括文字右侧空白）后整行选中，Ctrl+C 复制完整内容和换行；超出可视宽度的内容也会复制。拖动仍可选择部分文字或多行，Ctrl+A 选择当前日志文本。选中文字期间冻结当前日志快照，避免新日志改变复制范围；离开输入区域后更新。滚动查看旧日志时不自动跳到底部。

## Outliner / 模型名称

模型实例加载后默认使用源文件名（包含扩展名，例如 `CyberPunk_Car.glb`）。双击 Outliner 中的名称进入编辑，Enter 或点击其他位置确认，Esc 取消；空白名称保留原名。同名实例仍按各自稳定 ID 独立选择和编辑。改名只修改运行中实例的显示名称，不重命名模型文件，也不自动写回场景配置。

## Detail / Transform

选中实例后，Detail 的第一组属性为 Transform：Location、Rotation、Scale。红/绿/蓝对应 X/Y/Z；单击数值框即可选中原值并直接输入。窄面板会将 XYZ 输入框纵向排列。

面板采用固定的世界坐标：X 向前、Y 向右、Z 向上，与视角无关。渲染器和场景文件仍保留原来的 Y-up 坐标；编辑器通过 `(X,Y,Z) = (引擎 Z,引擎 X,引擎 Y)` 转换位置和缩放轴。没有父子层级的当前实例，其 Transform 即世界变换；非均匀缩放沿模型自身变换轴生效，不按相机轴拉伸。

旋转输入单位为度，XYZ 分别为 Roll、Pitch、Yaw，符号遵循 [UE Rotator 约定](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/Core/TRotator)。内部仍以弧度存储，映射为 `(pitch,yaw,roll) = (-Y,Z,-X)`。位置沿用项目现有长度单位，不自动换算为 UE 的厘米。

修改在下一帧统一更新模型矩阵、裁剪、阴影及选择描边，并重置相关时域历史。零缩放及非有限数值会被拒绝，以免法线矩阵不可逆。修改仅作用于运行中的场景，目前不自动写回 Scene.json。

左键点击场景模型或 Outliner 条目会选中同一个模型实例，Detail 同步显示名称。
点击场景空白处取消选择。拖动鼠标、拖动面板分隔条和按住右键操作相机不触发拾取。
选中效果只有橙色描边，没有内部染色，也不会透过其他模型显示。

## 视口操作轴

选中模型或 Outliner 条目后，在模型原点显示操作轴。默认位移，W 切换三轴位移，E 切换三圈旋转，R 切换缩放。红/绿/蓝对应 X/Y/Z，悬停或拖动时变黄。左键拖动生效，松开提交；拖动中 Esc 恢复本次拖动前的变换。输入数值、改名或操作面板时不会触发工具快捷键。

位移只改变所选世界轴分量，旋转围绕模型原点的固定世界轴合成到当前姿态，再转换为 Detail 使用的欧拉角；不直接累加某个欧拉角字段。Detail 与操作轴共用实例 Transform。右键拖动控制相机朝向，按住右键时 WASD 移动相机，E 上升、Q 下降（世界竖直方向，与视角无关）。移动中按住 Shift，速度在约 8 秒内从原速逐渐增加到 64 倍；松开 Shift、停止移动、松开右键或输入被 UI 接管时恢复原速。组合方向不会额外加速。未按右键时，W/E/R 仍用于模型操作工具。

缩放轴端的小方块控制单轴；轴间的直角符号及其淡色区域控制对应两轴；中心大方块等比缩放全部三轴。单轴或双轴沿手柄向外拖放大、向内拖缩小，中心方块向右上拖放大、向左下拖缩小。双轴和整体缩放保留原来的分量比例。缩放轴跟随模型旋转，与 `S*R*T` 和 Detail 的 Scale 分量一致；位移和旋转继续使用 Z 向上的固定世界轴。缩放不改变原点或旋转，保留负缩放的符号，分量绝对值最小为 0.0001，避免零缩放。近乎侧视、投影退化的双轴手柄隐藏，可转动相机后操作。

`EditorGizmo.h` 使用无抖动相机矩阵，将轴和圆环投影到完整客户区，再交给 ImGui 的背景 DrawList 绘制，复用现有 ImGui DX12 shader，不新增 shader/PSO。操作轴覆盖场景、不受场景深度遮挡，仅在中央未被面板覆盖的区域显示和响应。尺寸和拾取容差以客户区像素计，避免 back buffer 缩放造成偏移。射线与世界平面相交用于拖动，旋转圈接近侧视时采用屏幕切线拖动。

操作轴优先于模型拾取，开始拖动会废弃尚未返回的旧拾取结果；面板、分隔条和相机输入有独立优先级。失去焦点、调整窗口尺寸或拖动期间更换选择会取消本次拖动。目前不包含吸附。

## 撤销与重做

Ctrl+Z 撤销，Ctrl+Y 或 Ctrl+Shift+Z 重做。`EditorHistory.h` 保存最多 100 条历史，位移、旋转、缩放、改名和模型选择共用此上限。目录选择、场景点击选择及点击空白取消选择都记录；重复选择同一模型不增加记录。超过上限丢弃最早记录，撤销后完成新的有效操作会清空重做分支。

一次操作轴拖动只记一步，Esc 取消、失去焦点取消和未改变值的操作不记。Detail 数值在结束该输入框编辑时合并成一步；改名在 Enter 或点击别处确认时记一步。输入框编辑期间 Ctrl+Z 交给 ImGui 的文本撤销，拖动操作轴期间先松开完成操作或按 Esc 取消，再使用历史快捷键。

记录通过稳定实例 ID 保存修改前后的值，不依赖实例排序。模型选择独立于修改记录：选择 A、修改 A、再选择 B，撤销一次回到 A，再撤销才恢复 A 的修改。恢复 Transform 复用原有 dirty 更新路径；撤销/重做选择会同步 Detail、描边和操作轴，并使尚未完成的旧 GPU 拾取失效。历史仅保存在本次运行内，不记录相机和面板布局，也不自动保存到场景文件。

## 实现与约定

- `EditorSelection.h` 保存实例 ID 和选择操作的版本号。GPU 回读只能更新对应版本的选择，防止旧结果覆盖后来的目录点击。
- `ModelInstance::editorId` 在场景实例创建时分配，0 表示背景。实例排序不改变 ID；以后新增实例时也必须分配新的 ID，不能复制其他实例的 ID。
- `EditorSelectionRenderer.h` 在两条场景渲染路径汇合后、ImGui 之前执行。它独立持有 ID/depth 纹理、描述符和 PSO，不依赖 RDG 的临时资源。
- `Shaders/EditorSelection.hlsl` 包含 ID 绘制与描边两组入口。ID 绘制使用当前 LOD、无抖动相机矩阵和输出分辨率；描边约为客户区 2 像素，绘制在物体可见轮廓内侧。
- 拾取坐标按**完整客户区**归一化，再映射到 ID 纹理。当前场景铺满窗口背景，中央未被 UI 遮挡的区域不是独立渲染视口。
- 点击时只复制一个 `R32_UINT` 像素，每个在途帧使用独立 readback buffer，frame fence 完成后读取。结果可能延迟若干帧，不额外阻塞 CPU。
- 镂空材质使用 alpha 0.5 阈值，透明模型采用 alpha 0.05 阈值下最近表面的拾取规则。透明模型不是按最终混合颜色推断归属。
- 没有选择、没有点击请求时跳过编辑器绘制；有选择时额外绘制一遍场景 ID 和一遍全屏描边。暂未对该通道做实例批处理。

ID 纹理尺寸跟随初始化时的 back buffer。若以后加入 `ResizeBuffers`，需要在 GPU 空闲后同步重建编辑器 ID/depth 资源和 ImGui RTV。
