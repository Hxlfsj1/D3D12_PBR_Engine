# Mesh / Global 相机质量：完整实现记录

历史记录：下方参数及“未运行”仅描述2026-10-29初次实现；当前保留版、射线数和最终检查见 `SSGI.md` 与 `SSGIOptimizationCloseout.md`。历史临时文件已于2026-10-31清理。

2026-10-29。用户批准一次完成 Mesh 与 Global 两条路径。本次没有启动程序、
截图、运行单元或性能测试，约 210 FPS 仍属于改造前的用户反馈。

## 当前参数

| 质量 | Mesh SDF | Global SDF |
|---|---|---|
| 近档 | 2×2 GI 像素，每块最多 4 个方向，正常 2 帧 | 2×2 GI 像素，每块最多 4 个方向，正常 4 帧 |
| 远档 | 4×4 GI 像素，每块最多 4 个方向，正常 2 帧 | 4×4 GI 像素，每块最多 4 个方向，正常 4 帧 |

以接收点到摄像机的欧氏距离分类：到 2 m 升档，超过 2.2 m 降档。
滞回壳内仅继承经过上一帧同表面检查的等级，无可用历史时为远档。
参数集中在 `Settings/Pipeline.json` 的 `ssgi.world_sampling`。
4×4 父块内只要任一有效 2×2 代表请求近档，就使用四个细估计；这会提升
布局，不会把父块判无效或强迫整块 Repair。

射线路径范围仍为：屏幕 0–1 m、Mesh 0–2 m、Global 接受 (2,8] m 命中。
近处遮挡、黑色命中、无 SH 支持的阻挡、未完成查询状态和偏置扣减均保留。
SDF 分辨率、步数及探针光照表示未修改。

## 屏幕隔离与共享查询

原来的合并 near 历史拆成 screenBand 和纯 meshBand；globalBand 独立保留。
屏幕沿用原接收像素、2×2 成员、STBN index16、两帧刷新、棋盘和降噪政策。
投影、余弦方向、屏幕遍历、几何法线、世界偏置和屏幕历史重投影等十个
函数体与修改前快照完全相同。

SharedWorld 只在 origin/direction 真正相同时复用屏幕/Mesh 前缀。额外世界
方向的屏幕 HIT 只阻挡，不读取或额外计入 BounceSource。每次观测分别写
屏幕、Mesh、Global 的贡献，避免重复能量。世界单独更新不会重置屏幕 age。

## 世界样本与重建

细块的有效成员各贡献一个方向，按有效成员数平均。粗块在四个 2×2 象限
各选一个有效成员，按象限有效数/父块有效数加权；空象限不发射。
STBN 帧值改变刷新时的选择，不伪称实际更新 ordinal；不按命中数归一化。

三组共享 atlas 均为 GI/2。粗块把同一 estimate/owner 复制到四个细槽，
后续 Gather 仍归一化权重。fine receiver 不接受 coarse tap；coarse receiver
可使用相容 fine 邻居。没有恢复全成员几何否决或 S02 零权提前跳过。
粗块复制改变了世界重建核的形状，不宣称新图逐像素等价。

## 缓存与边界

新增 full-GI `R32_UINT worldPolicy` ping-pong：最低位是当前布局，Mesh 和
Global 各自用两位保存 invalid/fine/coarse/point 类型。世界重投影拒绝错档，
shared owner 拒绝 point；per-receiver 可以复用纯 point 或纯同档 shared，
但禁止两者混合后将共享估计洗成可跨档 point。真实新鲜度仍独立存于 alpha。

棋盘跳过条件仍只有最终表面历史与 parity，搬运三份 band 和类型不会取消
跳过相位。Repair 使用三种独立 flags，各贡献至多四条，失败部分才补追。
normal/preview 历史都覆盖新增资源，诊断非共享路径也采用独立贡献。

新增持久 payload 是每个 GI texel 24 B（screenBand 与 policy 双缓冲合计）。
按旧示例 854×534 约 10.44 MiB，另计按需 preview 和新增/扩大的 sparse atlas。
这不是测量的 VRAM 或性能变化。三贡献 Gather、policy 和独立缓存增加读写；
实际节省还取决于远区覆盖、缓存命中、S10 重合率与 Repair。

Trace CB 保留 1024 B、worldMode byte604、probe-neighbor byte768 和 Global
byte784。新描述符与阈值平方放在 `levels` 的保留 yzw 槽；根签名保持不变。
主程序、normal/preview 资源、RDG SRV/UAV 依赖、源策略身份及 capture metadata
均同步接入。新增 shader include 已登记到项目及过滤器。

## 验证

- C++ Debug / Release 编译链接成功，Editor UI architecture 检查通过。
- 22 个生产与兼容 DXC 变体以 `-O3 -WX` 编译通过。
- Clear、probe filter、SDF debug、两个 hybrid 兼容入口、Composite VS/两个 PS
  的 DXIL 与旧基线逐字节一致；空间、Temporal、Clamp 源文件未修改。
- 两次独立源码审查覆盖 host/shader 槽、RDG 顺序、四次同步、四槽广播、
  partial 权重、三 flags、类型与屏幕计能隔离。
- `git diff --check` 通过。C++ 保留未修改文件中的既有类型转换、代码页、
  struct/class 及缺少第三方 PDB 的告警。

初次扩展编译脚本时 HistoryClamp 宏有拼写错误；修正为项目使用的
`SSGI_PASS_HISTORYCLAMP` 后最终 22 入口均成功，未因此修改生产入口。

源码快照、构建日志、宏、哈希与结果保存在 `tmp/ssgi-camera-quality/`。
这些记录不构成运行画质或 FPS 验收。完整设计见 `Docs/SSGICameraQualityPlan.md`。

## 既有提速项复核补充

随后按用户要求对照旧快照复核，探针邻接查表、S10、整块否决关闭、硬棋盘格、
缓存写入去重、串行 Repair、wave 队列及 heap/alias 合同均保留。
修正了拆分后近段补追重复 screen 前缀、fine/缓存命中仍预选 coarse 原点两处
不必要工作。22 个 shader 变体重新编译通过，未运行性能测试；详见
`Docs/SSGIPerformancePreservationAudit.md` 与 `tmp/ssgi-camera-preservation-audit/`。
