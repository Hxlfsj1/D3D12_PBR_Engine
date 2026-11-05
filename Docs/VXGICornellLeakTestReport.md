# Cornell Box：VXGI 漏光测试结果

本文件保留修复前的诊断和数据。后续生产可见性修复及复测见 [遮挡修复报告](VXGICornellVisibilityFixReport.md)。

2026-11-02。结论：**当前零厚度 Cornell 派生封闭场景存在可复现漏光；本测试中的能量来自墙外探针，经缺少可见性约束的八邻 SH 查询进入盒内。** 近段贡献占主要部分，远段也出现非零贡献。正式取光算法尚未修复。

## 关键结果

1280×800，GI 640×400，AA=None，HBAO 关闭，保留 GI 时空滤波；探针格距 0.5、padding=1、Mesh SDF 格距 0.1、近/远采样 16/64。主实验仅关闭室内与环境光、封住入口、移除附加 Plain，并在后墙外放置独立自发光面。原 Cornell/Plain 和正式配置均未修改。

源面与后墙相距约 0.375 世界单位，GI 源 RGB=(2,1,0.5)。B0 保留同一源面几何，只令发光为零；B2 只在同一后墙开受控窗口。封口面并入 Cornell 模型，保持两个实例，避免新增封口实例改变最近两个模型的候选预算。

以下为第 256 帧，亮度采用线性 RGB 的 0.2126/0.7152/0.0722 加权，单位为接收端 `E/pi`，未乘接收材质、强度或显示变换：

| 用例 | 盒内全画面平均 | 最大值 | >1e-5 的像素比例 |
| --- | ---: | ---: | ---: |
| B0：封闭、外灯关闭 | 0 | 0 | 0 |
| B1：封闭、外灯开启 | 0.000168807 | 0.00936195 | 13.052% |
| B2：外灯开启、后墙开孔（正对照） | 0.0116617 | 0.0927758 | 见原始分析 |
| B1Visible：B1 加逐贡献探针的三角形可见性检查 | 0 | 0 | 0 |
| B1Interior：B1 只允许室内探针参与查询 | 0 | 0 | 0 |

固定后墙 ROI 排除窗口预留区域和墙边，包含 68,134 个 GI 像素。B1 在该 ROI 的平均亮度为 2.38796e-5、最大 0.00150011，5.938% 的像素超过 1e-5。因此不只是少量轮廓像素异常。

全画面 33,413 个 GI 像素超过门槛，最大四连通亮区为 30,046 像素，显著超过预设的连续 4 像素检出条件。B0 的尾段噪声为零，本轮 epsilon 按方案固定为 1e-5，未按结果放宽。

![同标尺线性 GI 对照](../tmp/vxgi-cornell-leak/comparison.png)

上图是线性亮度数据的灰度可视化：黑色=0、白色≥0.01 `E/pi`，四组标尺相同。B2 有超出标尺的合法进光，因此会饱和；图不是正常曝光下的游戏画面，也不能从灰度图读取红绿串色。

## 测试有效性与稳定性

- 对封闭壳体各发出 20,000 条随机室内射线，B0/B1 均无逃逸；另 20,000 条室内到盒外源面的有限射线全部被三角形参考挡住。B2 有 473 条随机室内射线从受控窗口逃逸。这些是明确数量的抽样，不冒充无限精度的全域证明。
- B0/B1 的屏幕 BounceSource RGB 最大值均为零。B2 的最大值为 2，且 GI 明显非零，正对照有效。
- B0/B1 都有 1,410 个探针：845 有效、565 禁用、0 未分类；总成功采集 27,040=845×32，追踪/采样失败为零。可见性诊断与室内探针诊断保持相同计数。全黑结果来自有效且完成填充的缓存。
- B0、B1、B2 分别在 160/192/224/256 帧采样。B0 始终严格为零。B1 平均亮度为 0.000168739、0.000168812、0.000168723、0.000168807；极差约为均值的 0.053%，稳定检出漏光。
- 每个采样点为独立冷启动、相同静态机位和确定性采样序列。这里的四点是尾段采样，不是保存了每一个中间帧的完整时间序列。
- 所有分析的线性纹理均检查了有限值；所有运行保存 D3D12 检查与完整进程退出状态，详见 runs 清单。

## 原因定位

### 屏幕光源与最终降噪

B1 的 SSGI-only 输出为零。完整生产路径的 world gather、原始观测和重建前后已经有非零值，所以漏光不需要最终降噪才产生。滤波会影响空间分布，不能解释最初的错误能量来源。

第 256 帧，完整路径各阶段的最大线性亮度：

| 阶段 | 最大值 |
| --- | ---: |
| raw（含棋盘格复用，不能全当新鲜射线） | 0.0104211 |
| 原始 world gather | 0.0116508 |
| reconstructed，最终 temporal/A-trous 前 | 0.0116508 |
| 最终 filtered | 0.00936195 |

### 近段与远段

不相减独立历史的三个显示结果来计算精确分量。新增导出同一次生产 Trace 的 packed world-owner atlas：左半 Mesh，右半 Global。

第 256 帧 Mesh atlas 平均亮度 1.752747e-4、最大 0.0116508；Global atlas 平均 1.491237e-7、最大 0.000143543。远段有真实非零贡献，不能归为“远段没有执行”。这些是不同 band 的 owner 槽位统计，粗档有重复，不能把平均值比直接宣称为屏幕能量占比或射线命中率。

本机位最外 clipmap 范围为 X=[-4,4]、Y=[-2,6]、Z=[-3,5]，包含盒子后墙和外部光源。未对所有远段射线做工作量计数或完整精度核对。

### 贡献探针可见性（决定性控制）

`B1Visible` 仅修改隔离副本中的 `VXGIProbeRadianceCommon.hlsli`：对每个原本有效的八邻贡献者，从其中心向查询命中位置发出有限线段，用该 Cornell 副本的全部 46 个三角形做双面求交；中间存在遮挡则拒绝该贡献。没有改变世界求交、源面、探针位置、分类、采集或 SH 阶数。

该控制使 Mesh/Global owner atlas、raw、world gather、reconstructed 和最终 filtered 全部严格为零。

第二项控制 `B1Interior` 只剔除室外探针，也得到全零。这进一步定位了本例的错误能量来自室外贡献者，而非合法室内反弹。

生产 `VXGIProbeQueryRadiance` 当前只判断记录存在、有效、已更新和有限值，然后按空间权重混合并重新归一；没有逐贡献者遮挡约束。**两项控制共同支持：本场景的主要失效链是盒外采光探针 → 无可见性约束的 SH 插值 → 盒内非零 GI。**

两项控制使用同一资产、参数和确定性重新初始化；探针更新 shader 的独立 O3 编译（`VXGI_MESH_SDF=1`、`VXGI_PROBE_SPARSE=1`）产物在原版与可见性副本逐字节相同，SHA256 为 `24910442F7FEC2FE3BAD6715F4BE729C272BAC7F81C1A87E42357EE768F1E524`。B0/B1/两项诊断控制的深度数据也完全一致。本轮未读回两次启动的完整 raw SH 缓冲作逐字节比较，不声称实际复用了同一个 GPU 缓冲。

## 原场景观察及结论范围

另保留原开口 Cornell + Plain 的入口总览纯 GI、内部正常合成及墙角纯 GI；GI 抓帧同时包含 GBuffer 深度/法线与 BounceSource。原场景仍有太阳、环境与内部顶灯，合法进光和本测试的错误能量可能并存。封闭组的定量错误率不能直接当作原场景的漏光比例。

本轮未加厚墙、未调 SDF/探针密度或射线偏移，也未做全场 SDF 对三角形的 GPU 射线精度扫描。已有证据足以确认这一例的探针跨墙贡献；不能据此宣称 Mesh SDF 几何求交没有其他问题。

诊断中的每贡献者三角形追踪用于归因。其性能没有测量，不直接把它作为正式实时方案。正式修复应另行选择可见性表示与预算，并保留此零能量用例作为回归。

## 代码和可复现产物

- `Tests/PrepareVXGICornellLeak.py`：生成独立 A/B0/B1/B2 场景，以及可见性/室内探针诊断副本。
- `Tests/RunVXGICornellLeak.ps1`：隐藏有限帧运行、固定相机、抓帧、进程退出/验证/探针填充检查。
- `Tests/AnalyzeVXGICornellLeak.py`：三角形参考、线性统计、固定 ROI、连通亮区和数据图。
- `PBR_Render.cpp`、`Passes/SSGIPass.h`：保存 GI 时额外导出已有线性资源，有限帧报告增加探针填充统计；普通帧不增加读回、射线或滤波，不改变取光算法。没有新增 UI 控件。
- [汇总数据](../tmp/vxgi-cornell-leak/analysis.json)、[几何检查](../tmp/vxgi-cornell-leak/geometry-validation.json)、[fixture 哈希](../tmp/vxgi-cornell-leak/fixtures.json)、[各次运行](../tmp/vxgi-cornell-leak/runs/)。每次运行指向自己的 metadata，后者描述 raw 文件格式和相机。

复现主实验（先构建 Debug）：

```powershell
python Tests/PrepareVXGICornellLeak.py
foreach ($frame in @(160,192,224,256)) {
    foreach ($case in @('B0','B1','B2')) {
        powershell -NoProfile -ExecutionPolicy Bypass -File Tests/RunVXGICornellLeak.ps1 -Case $case -Frame $frame
    }
}
python Tests/PrepareVXGICornellLeak.py --visibility-control
python Tests/PrepareVXGICornellLeak.py --interior-control
powershell -NoProfile -ExecutionPolicy Bypass -File Tests/RunVXGICornellLeak.ps1 -Case B1Visible -Frame 256
powershell -NoProfile -ExecutionPolicy Bypass -File Tests/RunVXGICornellLeak.ps1 -Case B1Interior -Frame 256
python Tests/AnalyzeVXGICornellLeak.py
```

先前 160 帧抓帧来自加入 owner atlas 导出前的 Debug 二进制；192/224/256 帧及可见性控制使用最终导出版本。两次均不改 GI shader；运行 JSON 分别记录可执行文件 SHA256。编译、实际运行与输入不变核查记录见 `tmp/vxgi-cornell-leak/validation.json`。

最终检查：Debug/Release 构建及编辑器 UI 架构检查通过；共 20 次隐藏运行（19 Debug、1 Release）完整退出，全部 D3D12 warning/error 为零，19 次 Debug 均启用验证。Release 的 B1 第 256 帧平均亮度为 0.0001688065，同样检出漏光；Release 未启用 D3D12 debug layer。所有正式测试输入哈希保持不变。构建仍存在项目已有的转换/编码/第三方 PDB 警告，本轮未扩展修复范围。
