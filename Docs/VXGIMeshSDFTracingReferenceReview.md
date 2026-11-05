# Mesh SDF 修法：公开实现核查

后续已结合当前源码确定[实施方案](VXGIMeshSDFEdgeRepairPlan.md)：保留距离场加速，
在不确定单元做局部几何确认。下文“优先纯场对照”的研究建议由该具体方案收敛；
选择依据是本工程误差带中的零安全步长及已存在的单元三角加速数据，不是将三角确认
当作行业通用要求。实施方案尚未完成画质和性能验收。

2026-11-03。结论：**SDF空域跳步 + 局部三角形确认在技术上合理，但本轮未找到
Lumen、Brixelizer 或 Godot SDFGI 将这种确认作为常规必经步骤的证据。不能把它
描述成行业通用修法，也不能把所有容差命中直接定性为逻辑错误。**

本轮只查资料、源码和修正方案表述，没有修改生产 shader 或做新的性能结论。

## 查到的实际做法

### Lumen：保留距离场近似，处理膨胀和自相交

Epic 的 SIGGRAPH 2022 讲稿第49–51页解释：为避免薄面漏检而膨胀距离场，
会带来过遮挡和较大的起点偏移。其GI策略是在起点不膨胀、沿射线逐渐增加膨胀，
以保留接触区域；反射的掠射情况使用另一种逐步限制膨胀的策略。
这几页讲的是距离场追踪启发式，没有要求最终转回原网格三角形确认。
[Epic讲稿，第49–51页](https://www.realtimerendering.com/advances/s2022/SIGGRAPH2022-Advances-Lumen-Wright%20et%20al.pdf#page=49)

Epic 官方5.4文档说明软件路径使用signed Mesh/Global Distance Fields；
光照查询使用对应表面的Surface Cache，硬件路径才另外讨论三角形追踪。
不能把Surface Cache与本工程自由空间探针SH插值当成同一种取光方式。
[Lumen 5.4 Technical Details](https://dev.epicgames.com/documentation/en-us/unreal-engine/lumen-technical-details-in-unreal-engine?application_version=5.4)

PDF原件和第49/50/51页的实际渲染图保留在
`tmp/vxgi-mesh-edge/research/`，已查看图中的L形墙角示意。
本轮旧本地路径 `D:\UE544` 不存在，没有声称直接核读了UE5.4.4 shader实现；
关于Lumen的结论限于上述官方材料。

### AMD Brixelizer：unsigned距离场也允许epsilon命中

AMD明确说明Brixelizer使用unsigned距离场。v1.1.4公开追踪函数在brick内按
epsilon判断命中并返回brick ID/位置，没有在这个返回路径复验原网格三角形。
因此“unsigned场必须再求一次三角形交点”并非普遍要求。
[AMD原始文档](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/v1.1.4/docs/techniques/brixelizer.md#L535-L556)，
[追踪源码，218–228行](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/v1.1.4/sdk/include/FidelityFX/gpu/brixelizer/ffx_brixelizer_trace_ops.h#L218-L228)

其GI另设法线方向的ray pushoff和SDF solve epsilon，命中后读取对应brick的
radiance cache。参数、brick尺度和缓存归属应一起看，不能只照抄一个阈值。
[GI实现，839–893行](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/v1.1.4/sdk/include/FidelityFX/gpu/brixelizergi/ffx_brixelizergi_main.h#L839-L893)

### Godot SDFGI：阈值命中后读取体积光照

Godot 4.3的公开probe积分shader使用偏移后的起点，在解码后的距离低于阈值时
停止；之后估计场梯度并读取体积光照/方向信息，未附加原网格三角形确认。
该阈值的编码和单位不同，不能直接与本工程的0.09世界单位作数值比较。
[Godot 4.3追踪与取光实现，176–241行](https://github.com/godotengine/godot/blob/4.3-stable/servers/rendering/renderer_rd/shaders/environment/sdfgi_integrate.glsl#L176-L241)

## 对前一轮诊断的修正

本工程三机位、零距离取光控制和三角形参考的观测仍有效：原路径在墙角产生
大量近起点命中，和真实三角形交点有明显差别；这些命中经现有SH取光后出现亮条。
但“与原三角形不一致”本身不证明距离场近似算法违反了自己的设计。
更准确的判断是：**当前粗unsigned场、固定全额阈值、较大起点偏移和SH取光组合，
在这些机位产生了不可接受的近表面误差。**

三角形参考证明改变命中位置能明显消除宽亮边，不证明三角形确认是唯一修法，
也不证明任何几何确认后的SH重建就具有正确的表面辐射语义。

## 对本工程的判断与后续顺序

以下是基于资料和本工程数据的工程判断，尚未实施验证：

1. 优先建立不增加逐贡献者遮挡射线的近表面查询对照，借鉴起点逃逸与随行进距离
   增加的膨胀/有效容差；同时检查起点偏移和命中后取光，避免把起点附近的膨胀层
   直接变成亮面。
2. 不能只把0.9改成较小常数。本工程用距离减插值误差界来计算安全步长，误差带内
   步长可为0；还必须解决如何继续推进、保留邻墙遮挡和避免薄面漏检。Lumen使用
   signed场，AMD使用不同的稀疏brick表示，不能直接复制它们的常数或完整假设。
3. 局部三角形确认保留为不确定区域的兜底候选。它可复用现有cell→triangle/BVH，
   对原三角形给出更明确的交点；它的代价、触发比例、访问单元数仍需GPU时间戳验证。
   不保证每条主射线只检查一个单元，也不保证比纯场路径便宜。
4. 用冻结的三个机位比较宽白条、过暗接触边、实际遮挡/薄面漏光与GPU耗时，
   再决定采用哪条路径。此前的三角形遍历对照不作为性能验收。

原始定位报告：[三个机位诊断](VXGIMeshSDFEdgeDiagnosis.md)。
