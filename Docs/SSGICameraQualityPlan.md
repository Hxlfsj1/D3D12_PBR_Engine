# Mesh / Global SDF 摄像机距离质量：实现与设计记录

历史记录：本文保留2026-10-29初次改造的设计和验收边界，不代表当前射线数与缓存实现。当前状态以 `SSGI.md`、`SSGIOptimizationCloseout.md` 和 `Settings/Pipeline.json` 为准；历史临时文件已于2026-10-31清理。

状态：2026-10-29，用户已批准完整实施，Mesh 与 Global 两条世界路径均已接入。
本文记录当前实现及其设计边界，已完成的设计不再是待确认提案。编译结果见实现记录；
尚未运行应用、画面或性能验收。原约 210 FPS 属于改造前用户反馈，不能当成新版本实测。

## 1. 已完成范围与不变量

- 仅世界贡献按接收点到摄像机的距离分配质量；没有恢复旧的四档手动 quality。
- 纯屏幕 SSGI 保留原 GBuffer 接收位置、0–1 m 范围、2×2 成员方向、STBN、两帧刷新、硬棋盘和既有重建政策。
- Mesh 接收沿射线 0–2 m 的世界命中；Global 接收 (2,8] m 的世界命中。相机分类不会替换这些几何距离判断。
- 每条世界方向仍检查前景。黑色/无 SH 支持的命中仍阻挡，未完成或无效查询不能当 MISS。
- 保留探针邻接查表和 S10 的真实相同 origin/direction 前缀复用；整块几何一票否决保持关闭。
- S02 零权重提前跳过、局部实例候选网格、四线程 Repair、球灯/代理光源未恢复。

原 near RGB 合并屏幕与 Mesh 的耦合已经通过独立 screenBand 记录解除。
当前是屏幕、Mesh、Global 三份独立 RGB/age，而不是 Global-only 过渡方案。

## 2. 距离定义与已采用参数

| 项目 | 当前实现 |
|---|---|
| 相机度量 | 欧氏距离 `length(viewPosition)`，以距离平方比较阈值平方 |
| 进入近档 | 代表点距离 <= 2.0 m，使用 2×2 |
| 退出近档 | 代表点距离 > 2.2 m，使用 4×4 |
| 2.0–2.2 m 滞回壳 | 几何检查通过后继承上一表面的 policy；没有可用同表面历史时使用粗档 |
| 参数位置 | `Settings/Pipeline.json` → `ssgi.world_sampling.near_distance / near_exit_distance` |
| 参数验证 | 有限且 `0 < near_distance <= near_exit_distance`，退出距离平方仍有限 |

分类使用 receiver 当前几何位置，不使用射线命中位置，也不把射线原点搬到摄像机。
屏幕 SSGI 不读取这个质量等级来改变其采样策略。相机距离和沿射线路径距离各司其职。

## 3. 2×2 / 4×4 层级布局

在每个 4×4 父块中，选取四个 2×2 子块的首个有效成员作为分类代表。
只要任一有效子块代表请求近档，整个父块使用四个细 2×2 估计；否则父块使用一个粗 4×4 估计。
这是提升采样布局，绝不将整个父块判无效、也不把父块所有 receiver 推入 Repair。
分类沿用深度/ORM 有效性，没有恢复全成员法线/切平面几何预计算。

代表点分类是有限采样近似：极薄近物体与远背景共处一个子块时，首个有效代表不一定覆盖
每个成员的距离需求。现有逐 receiver 几何 Gather 与局部 Repair 继续处理可用性，
但不宣称完全消除混合表面光照或所有阈值边界误差。

屏幕始终保留自己的细 2×2 布局。Mesh 和 Global 使用相同的相机分类，
但各自的光照、缓存有效性、刷新周期和 Repair 状态独立。

## 4. 四条世界方向与部分块权重

| 贡献 | 近档 | 远档 | 正常周期 |
|---|---|---|---|
| 屏幕 SSGI | 固定 2×2，全有效时 4 条 | 同左，不分档 | 2 帧 |
| Mesh | 2×2，全有效时最多 4 条 | 4×4，最多 4 条 | 2 帧 |
| Global | 2×2，全有效时最多 4 条 | 4×4，最多 4 条 | 4 帧 |

细档世界估计由有效成员各发一条方向，以有效成员数归一化。粗档将 4×4 分为四个 2×2 象限，
每个非空象限从有效成员中选一个原点，贡献权重为 `Nq/N`：Nq 是该象限有效数，N 是父块有效总数。
全有效时四象限各占 1/4；部分块不能沿用 16 作除数，也不能按命中数重新归一化。
黑色命中与 miss 都是合法的零贡献观测。象限内选择使用当前提交帧 STBN，
未新增声称能记录真实刷新次数的 update ordinal。

全有效稳态的名义世界观测密度：Mesh 近/远分别为 0.5/0.125 条每 GI 像素每帧；
Global 近/远分别为 0.25/0.0625。相对旧版，Global 近档密度相同但估计范围更小、
每份估计从 16 条变为 4 条；远档平均密度为旧版的 1/4。预算算术不是 GPU 加速或降噪测量。

## 5. 统一 atlas 与 Gather

屏幕、Mesh、Global 共享结果统一采用 GI/2 的最细 atlas 容量。
粗世界估计及其带跨度编码的代表地址复制到对应四个细槽位；Gather 用归一化插值权重合成，
复制槽位不作为四次独立能量累加，也不改变估计的观测数。

- 细档世界 receiver 拒绝粗档世界 tap，避免高质量区域直接继承低质量估计。
- 粗档可以使用几何兼容的细档邻居。
- 屏幕 tap 不带世界质量限制，原几何过滤政策保留。
- 逐 receiver 法线/切平面检查和权重仍在；没有全块几何否决。

## 6. 三份贡献历史和 policy

每个 normal/diagnostic 历史集合拥有独立的 screenBand、meshBand、globalBand
RGBA16F ping-pong，以及 R32_UINT worldPolicy ping-pong。RGB 是对应贡献，alpha 是 age，
没有把跨度、命中比例或 Repair 类型塞入 age。

Policy 编码：bit 0 表示 2×2/4×4 布局；Mesh 使用 bits 1–2，Global 使用 bits 3–4，
各自区分 invalid、fine shared、coarse shared、point Repair。两个世界贡献可以有不同 cache kind。

世界重投影保留同表面检查和到期规则，同时拒绝不匹配的共享档位。
共享 owner 不可复用 point Repair 结果；逐 receiver Repair 可在同表面条件下复用 point 历史。
同一次世界重投影不混合 point 与 shared，防止粗共享值借 point 标签丢失档位身份。
换档不能仅将旧 age 清零以冒充新质量观测。全局配置进入源身份，局部档位不触发逐帧全屏 reset。

硬棋盘格仍由最终表面历史与 parity 决定，独立于三份 band 到期。
跳过相位搬运三份当前缓存及世界 kind；转存失败不取消 skip，档位变化不能强迫屏幕重新采样。

相对旧两份 band 历史，新增 screenBand 和 policy ping-pong 的 payload 为每 GI texel 24 B。
854×534 的 normal set 约增加 10.44 MiB；另有按需分配的 preview 集合和 sparse/transient 成本。
这不是实测 VRAM 增量，不能忽略资源对齐、RDG 复用或现有常驻分配。

## 7. 屏幕前缀、S10 与独立计能

`TraceSharedReceiverBands` 分别接收 needScreen、needMesh、needFar。
只有当前原点和方向真正相同，才共用屏幕/近 Mesh 前缀。

1. 屏幕命中且 needScreen 为真：读取 BounceSource，写入屏幕贡献一次。
2. 世界额外方向的屏幕命中：只用于遮挡，不读源色增加屏幕估计；黑色命中仍阻挡。
3. Mesh 首命中：仅在请求 Mesh 时计入 Mesh；无论亮度和 SH 支持如何，都阻挡 Global。
4. 只有完整近段 MISS 才能进入 Global；Global 只接受 (2,8] m，并扣除已花在 normal offset 上的距离。

屏幕更新与世界更新不再由一个合并 near 缓存 age 决定。额外世界方向可能增加可见性查询，
因此分离缓存和减少远处观测并不自动保证净提速。屏幕源观测政策不变，
但世界输入改变后，最终混合 GI 的非线性降噪图像不要求全通道逐 bit 相同。

## 8. Repair 与实际工作上界

Repair 队列独立打包 screen/Mesh/Global 三个请求 flag；每个 receiver 仍由单 owner 处理。
每份贡献先尝试自己的历史，失效时最多各追 4 条，周期分别为 2/2/4 帧。
三份同时失败时最多请求 12 个 Repair 观测，而不是旧近 4 / 远 16 的组合。
兼容非共享诊断路径也使用分开的三个四条估计。

正常共享密度不能作为绝对物理成本上限。需另外观察全历史失效、父块升档、
反遮挡与相机跨阈值时的工作，保留黑色遮挡与 INVALID/EXHAUSTED 处理。
不得为了工作预算静默跳过必要遮挡、丢失 receiver，或将未完成查询改成 MISS。

## 9. Host / shader 合同

- `Settings_Manager.h` / `Pipeline.json`：世界阈值及校验。
- `SSGIHybridState.h` / `PBR_Render.cpp`：实际策略身份、提交历史与 source reset。
- `ResourceManager.h`：三个贡献缓存与 policy 历史，normal/preview 的独立集合。
- `SSGIPass.h`：统一 atlas、SRV/UAV 绑定、三个贡献与 policy 的 RDG 依赖。
- `Shaders_For_SSGI.hlsl`：分类结果应用、象限方向、独立贡献、Gather/Repair/棋盘转存。
- `SSGICameraQuality.hlsli`：距离分类、policy 打包与 world-kind 重投影。

Trace CB 仍为 1024 B；worldMode SRV alias byte 604、probe-neighbor byte 768、
Global 参数 byte 784 均保留。新增描述符及阈值平方利用原 depth levels 的 yzw 保留分量，
x 仍是各级深度描述符；没有把新字段继续追加到 1024 B 之外。

## 10. 已完成设计步骤与验收停点

| 原设计步骤 | 当前状态 |
|---|---|
| 固定屏幕范围并隔离世界调度 | 已实现，screenBand 独立记录 |
| 距离度量与阈值 | 已采用欧氏距离、2.0/2.2 m |
| Mesh + Global 接收布局 | 已完整实现，不再停留在 Global-only |
| 四方向预算与部分块加权 | 已实现，粗块四象限 Nq/N |
| atlas、policy、缓存 kind 与局部 Repair | 已接线 |
| 屏幕前缀/S10/硬棋盘不变量 | 已按上述合同接入，运行行为尚未验收 |
| 编译结果 | Debug / Release 构建通过，22 个 DXC `-O3 -WX` 变体通过；见 `Docs/SSGICameraQualityImplementation.md` |
| 运行、画质、性能 | 尚未执行，不宣称收益 |

若以后验收发现车内性能下降、跨阈值闪烁、遮挡错误或 Repair 数量激增，
针对单个原因回退或修复，不恢复全块否决或额外模糊掩盖问题。

## 11. 后续验证边界

本次文档更新只记录已获授权的实现；没有在文档任务中编译、运行或测试。
编译结果见实现记录，尚未运行。未来若获准运行，固定配置、分辨率、DLSS、预热与源码版本，
车内通过 `--camera-preset=Settings/CameraPresets/VXGICavityCamera.json` 使用精确姿态，隐藏/后台进行。

需覆盖静止、接近/远离 2.0/2.2 m、转身新显露、薄近景遮挡远背景和残缺父块。
记录 SharedWorld/Trace/Repair/滤波/全帧 GPU 时间、分类与缓存带宽、实际新方向数、
仅遮挡前缀数、Repair 队列、升档比例和额外资源成本；核对 screen 四方向/两帧政策不被世界档位带动。
检查噪声、局部细节、跨表面混光和移动响应，使用同条件数据决定当前参数是否保留。

参考：`Docs/SSGIOptimizationCloseout.md`、当前 `SSGI.md` 和上述源码模块。
`tmp/gi-deep-audit-20261028/sections/sampling-caches.md` 保存早期 S06/S07/S08/S11/S12/S15 分析，
但旧文中合并 near、16 条 Global 和未实施相机策略的描述不代表当前实现。
