# Cornell Box：探针烘焙漏光检查

后续更新：下文保留修复前的检查证据。已完成一个局部修复，结果见文末“源面自相交过滤修复”。

2026-11-02。**烘焙阶段存在少量太阳光漏遮挡：封闭盒内 440 个有效自由空间探针中，3 个具有不应出现的非零 SH。** 盒外自发光对照没有检出烘焙漏光。此次只检查并定位，未修改正式采光、太阳遮挡或 `ray_min_distance`。

## 检查方法

在初始化完整填充后，直接读回 GPU `VXGIProbeState::GPURecord` 缓冲：每条 96B，包含四组原始 RGB SH、分类、成功更新/采样计数。与 CPU 分配的探针位置按索引配对。没有通过运行时可见性、八邻插值、SH 负值钳制或时空降噪判断烘焙是否漏光。

探针与 Mesh SDF 格距仍为 0.5/0.1，padding=1，64 rays/update，32 次成功更新，共 2048 个采光方向/有效探针。保留封闭盒的零厚度墙和内部方块；内部灯与环境光关闭。探针中心按房间边界及 GLB 内部方块的逆变换分类，未把方块实体内的点当自由空间。

原始缓冲跟随普通帧 fence 读回，只在有限帧 CLI GI 抓帧时导出；普通帧和交互保存不新增该读回。元数据保存位置、epoch、stride/count，原始文件为 `vxgi-probes.bin`。

## 原始 SH 结果

| 条件 | 室内有效探针 | 非零 SH 探针 | 室内最大绝对 SH 系数 |
| --- | ---: | ---: | ---: |
| B0：全关灯 | 440 | 0 | 0 |
| B1：封闭，只开盒外自发光 | 440 | 0 | 0 |
| B2：开孔，只开盒外自发光 | 440 | 374 | 2.39073 |
| B1Sun：封闭，只开太阳光 | 440 | **3** | **0.00107603** |
| B2Sun：开孔，只开太阳光 | 440 | 423 | 0.822739 |

这些数值是未卷积 SH 系数，不是最终像素亮度。

- 自发光 B1 有 405 个有效室外探针，其中 156 个非零；室内 440 个（包含 284 个距墙小于 0.5 的探针）所有系数严格为零。与 B0 的室内系数差也严格为零，负对照有效。
- 自发光 B1 第 6 帧与第 160 帧的完整 1410 条 GPU 记录逐字节相同，SHA256 为 `4cd92971e3c57161219f2eb3f3724b96892d70c34cab25e0718819989267853c`。运行时没有把原始缓存改成零来掩盖污染。
- 每个有效探针都确认完成 32 次更新、2048 次采光，未分类/追踪失败/源采样失败均为零。有效探针没有落在内部实体方块中，禁用记录的 SH 也为零。
- 太阳光正负对照均移除外部自发光面实例，避免它作为遮阳板挡住开孔；两个太阳组仅在后墙窗口上存在几何差异。太阳传播方向为 `(0.5,-0.5,1)`，颜色 `(4,4,4)`，环境与材质发光均为零。
- 太阳开孔正对照明显增加非零室内记录至 423 个，不把“相同的 3 个漏光记录仍存在”当成正对照通过。

## 三个受污染探针

| 索引 | 世界位置 | L0.R | 最大绝对 SH 系数 |
| --- | --- | ---: | ---: |
| 652 | (-0.25, 2.25, -1.75) | 0.000626208 | 0.00107603 |
| 836 | (0.75, 0.25, -1.75) | 0.000626208 | 0.000676860 |
| 944 | (1.25, 0.25, -1.75) | 0.000626208 | 0.000812507 |

## GPU 见证：太阳遮挡的近端区间跳过了邻墙

生产 `VXGIProbeTraceSourceTracked` 的采光命中调用 `VXGIEvaluateSurfaceSource`。后者从源面命中位置向太阳发射射线，区间为 `[surface.ray_min_distance, surface.max_ray_distance]`。当前配置前者为 `0.0001`，后者为 128。

临时副本 `B1SunWitness` 只对原生产逻辑实际判定 `Lo0.r > 0` 的室内样本记录见证：保留原命中点，另外把太阳射线 `tMin` 改为 `1e-7`，通过 raw SH 编码源三角形、重新找到的遮挡三角形、距离和后墙间隙。正式着色器没有写入这段诊断。

结果如下：

| 探针 | 源三角形 | 小 tMin 下的遮挡三角形 | 遮挡距离 | 与原 0.0001 比较 |
| --- | ---: | ---: | ---: | --- |
| 652 | 0（地板） | 5（后墙） | 0.0000703725 | 被原区间跳过 |
| 836 | 0（地板） | 5（后墙） | 0.0000981127 | 被原区间跳过 |
| 944 | 0（地板） | 5（后墙） | 0.0000572324 | 被原区间跳过 |

三条诊断均返回真实 HIT/status=1，且挡住它们的是另一块三角形，不是原地板自身。GPU 记录的后墙 z 间隙除以朝太阳方向的 z 分量绝对值，也给出相同遮挡距离。光源只来自太阳，发光为零；原 `Lo0 > 0` 已经说明生产太阳可见性接受了它们。

所以本例的失效链是：**探针采到靠近墙根的地板点 → 太阳射线用全局 tMin 避自交 → 距起点更近的邻墙被一起跳过 → 错误的直接漫反射写入室内探针 SH。** 精确求交本身接受双面，但区间之外的三角形不会被检测。

见证数据是诊断载荷，不当作物理 SH 数值参与前表；原始物理记录与见证分开保存。

## 与运行时插值漏光的关系

先前的运行时问题是“室外正确采光的探针跨墙参与插值”；它已经通过逐贡献者可见性修复。

这里的 3 个探针位于室内，原始数据已经被污染。运行时只验证它们到查询点的可见性，无法自动纠正它们烘焙时错误接受的太阳光。两条问题链需要分别处理。

下一处修复应针对太阳遮挡的自身表面与邻近遮挡区分。直接把全局 `ray_min_distance` 改小可能引入自身阴影；此次未把诊断用 `1e-7` 写入正式配置，也未声称其他场景已通过。

## 产物与复现

- [原始缓存统计](../tmp/vxgi-cornell-bake/bake-analysis.json)
- [太阳射线见证](../tmp/vxgi-cornell-bake/sun-shadow-witnesses.json)
- `tmp/vxgi-cornell-bake/*-interior-probes.json`：逐探针位置、完整四组 RGB SH 与采样次数。
- `tmp/vxgi-cornell-bake/runs/`：每次进程、D3D12 验证、源码/可执行哈希及抓帧路径；每次抓帧目录包含 `vxgi-probes.bin` 与其元信息。

```powershell
$env:VXGI_CORNELL_OUTPUT='tmp/vxgi-cornell-bake'
python Tests/PrepareVXGICornellLeak.py
foreach ($case in @('B0','B1','B2')) {
    powershell -NoProfile -ExecutionPolicy Bypass -File Tests/RunVXGICornellLeak.ps1 -OutputDirectory tmp/vxgi-cornell-bake -Case $case -Frame 6
}
powershell -NoProfile -ExecutionPolicy Bypass -File Tests/RunVXGICornellLeak.ps1 -OutputDirectory tmp/vxgi-cornell-bake -Case B1 -Frame 160
python Tests/PrepareVXGICornellLeak.py --sun-control
foreach ($case in @('B1Sun','B2Sun')) {
    powershell -NoProfile -ExecutionPolicy Bypass -File Tests/RunVXGICornellLeak.ps1 -OutputDirectory tmp/vxgi-cornell-bake -Case $case -Frame 6
}
python Tests/PrepareVXGICornellLeak.py --bake-sun-witness
powershell -NoProfile -ExecutionPolicy Bypass -File Tests/RunVXGICornellLeak.ps1 -OutputDirectory tmp/vxgi-cornell-bake -Case B1SunWitness -Frame 6
python Tests/AnalyzeVXGICornellBake.py
```

正式模型、Scene/Lighting/Pipeline 配置哈希保持不变。Debug/Release 导出代码构建通过，本轮增加的是原始缓冲读回和测试工具，没有改变正式烘焙算法。

本轮保留 8 次物理缓存检查与 1 次 GPU 见证，共 9 次完成的运行；D3D12 warning/error 均为零。Release 复核同样得到：盒外自发光的室内 SH 全零，封闭太阳光组有相同 3 个非零室内探针、最大绝对系数 0.00107603。源码/元数据与原始二进制均已保留，可逐探针核查。

## 源面自相交过滤修复

用户要求按影响与实施成本判断。本例影响小，生产修改也小：四个现有 shader include 中增加约 32 行、替换一处太阳射线调用，复用原追踪器；不新增射线、GPU 资源、配置或采样预算。

`VXGISDFTraceFromSurface` 保留完整 `[0, maxRayDistance]` 区间。原 `rayMinDistance` 只作为识别源面近零自命中的范围：

- Mesh 后端只拒绝“同一实例、同一三角形、距离不超过该范围”的候选；同模型的其他面、共享资产的其他实例仍可遮挡。
- Legacy 后端按全局世界三角形索引识别源面。
- 拒绝候选后继续原区间的候选判断，不推进整条射线的起点或 tMin；邻墙距离再近也保留。
- 其他追踪调用、alpha 覆盖、正反面遮挡规则和正式 JSON 参数保持原值。没有把诊断 `1e-7` 写成全局配置。

隔离复测目录为 `tmp/vxgi-cornell-bake-fix/`，不覆盖旧缓存证据。相同探针位置与分类状态下：

| 对照 | 修复前 | 修复后 |
| --- | --- | --- |
| 封闭太阳组 | 440 个有效室内探针中 3 个非零 | **440 个全部严格为零**，只有原 3 条记录改变 |
| 开孔太阳组 | 423 个室内探针非零 | 仍为 423 个，最大系数不变 |
| 开孔太阳组室内 L0 亮度总和 | 15.67252464 | 15.66953381，下降约 **0.0191%** |
| 封闭/开孔自发光组 | 原始缓存 | 修复后完整 GPU 记录均与修复前逐字节一致 |

L0 总和用于同输入的信号变化比较，不是最终像素亮度，也不能把保留比例直接解释成正确光照比例。当前对照没有出现大范围失光的迹象；没有据此宣称所有几何边界均得到严格验证。

Debug/Release 工程与 UI 架构检查通过，Mesh/Legacy 探针更新 shader 均通过独立 DXC O3/WX 编译。Mesh 后端做了实际生产初始化和原始缓存读回；Legacy 本轮只检查编译。

本次局部修复保留 6 次完整运行（4 Debug、2 Release），D3D12 warning/error 均为零，Debug 验证均可用。Release 的封闭太阳组同样全零，开孔组仍为 423 个非零室内探针，L0 亮度总和与 Debug 完全一致。

复现：将上面的输出目录改成 `tmp/vxgi-cornell-bake-fix`，生成基本场景与 `--sun-control`，运行 B1Sun、B2Sun、B1、B2；再对 B1Sun/B2Sun 使用 `-Configuration Release`，最后执行 `Tests/AnalyzeVXGICornellBake.py`。修复前后比较需要保留旧 `tmp/vxgi-cornell-bake/` 目录。

[修复后的原始缓存统计](../tmp/vxgi-cornell-bake-fix/bake-analysis.json) · [逐项比较](../tmp/vxgi-cornell-bake-fix/sun-fix-comparison.json)。性能未做 timestamp 测量；这里只确认没有增加追踪次数，不给出耗时改善结论。
