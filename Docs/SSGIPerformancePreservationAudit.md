# 相机质量改造后的既有提速项复核

历史记录：本文保留2026-10-29的审查结论；其临时输入和报告已于2026-10-31清理。最终收尾状态及检查见 `SSGIOptimizationCloseout.md`，本文不构成当前性能测量。

2026-10-29。按用户要求，对照 `tmp/ssgi-camera-quality/before/`、改造前全仓
SHA 清单与已有成功/失败报告，检查旧收益是否被撤回或绕过。
本次没有启动程序、运行性能/图像或单元测试。

## 保留情况

| 已有改动 | 检查结论 |
|---|---|
| 探针邻接查表 | 查询 shader、拓扑、GPUScene、探针与滤波模块与改造前 SHA 相同；byte768 邻接 SRV 绑定段保留 |
| S10 近远共前缀 | SharedWorld 仍仅对实际重合的 origin/direction 做一次屏幕/近 Mesh 前缀；far-only 不读取无用近场 SH |
| 关闭整块否决 | SelectBandTile 只处理成员有效性和首代表，空块才无地址；没有恢复全成员法线/切平面预计算 |
| 硬棋盘格 | 最终表面历史有效＋parity 跳过，band 到期/档位/搬运失败不能取消 skip |
| 缓存写入去重 | 有效像素只在实际终止出口写各份 band；没有恢复每个有效像素先清空再覆盖的写法 |
| 单 owner Repair 与 wave 队列 | lane&3 的单 owner、wave 批量原子和 prefix 入队保留，未恢复四线程协作 |
| 2/4 帧复用 | screen/Mesh 正常 2 帧，Global 正常 4 帧；不是全体每帧重追；局部档位不进入 CPU 全屏 reset Key |
| heap 与慢状态规避 | ExecuteComputeNoBarrier、worldMode 的 byte604 SRV alias、ImGui 后恢复 engine heap 均保留 |
| 生产/预览调度 | 没有重复添加第二套正式 SharedWorld/Trace；预览继续按需分配，没有每帧重建历史 |

零权提前跳过 S02、局部实例网格、球灯、quad Repair 等已撤回项目均未恢复。

## 本次查到并修复的两处重复工作

### 1. 近段补追拆分后重复检查屏幕

旧 near 一份估计内部先 screen、miss 后 Mesh；分开缓存后，screen 和 Mesh
同时需要新观测时，原新版本分别调用两个四方向循环，重复执行相同
index16+ray 的屏幕前缀。

当前 `EvaluateReceiverNearBands` 先独立检查两份历史，仅对需要更新的分量
执行同一四方向循环。接收点位置、法线与 noise handle 在循环外准备，
`cachedWorldOffset` 跨四条线保留懒计算。屏幕命中仅在该份屏幕观测需要更新
时计能；否则只阻挡世界贡献。缓存命中的分量保持原值、age 和 kind。
此修正覆盖 Repair 和非共享诊断路径。

SharedWorld 单样本 wrapper 行为保留；far-only 的 index32 方向序列没有改变。
没有为这项修正增加第三份近段追踪内核、资源或同步。

### 2. 不需要 coarse 射线时仍预选原点

原新版本在准备父块时就为四个象限读取 STBN 并扫描有效成员，即使最后
选择 fine 布局，或者 Mesh/Global 缓存都有效，不会消费这些选择。

当前仅在世界缓存重投影之后、`span==4` 且至少一份启用世界贡献确需新观测
时选择 coarse 原点。STBN 坐标、帧、index48、成员顺序和权重不变，fine 或
缓存命中不再做这项选择。沿用已有第三次 group 同步传递选择结果，
SharedWorld 仍为四次同步，没有新增 barrier。

## 验证

- 22 个 DXC `-O3 -WX` 变体重新编译通过。
- 其中 17 个变体与修正前 camera 版本逐字节一致；变化限于 SharedWorld、
  Repair、非共享 Trace 及其 Mesh/legacy 兼容变体。
- 十个受保护源码文件与 camera 改造前 SHA 相同；独立审查另核对绑定函数、
  source key、pass 名单和 history Init/Ensure/Flip 的保留情况。
- 两处修正均经独立源码复核：采样方向、每份四条归一化、偏置、首命中、
  缓存年龄/类型和屏幕计能规则保持。
- 宿主 C++ 本轮未修改，沿用上一轮成功的 Debug/Release 结果；本轮不重复
  构建宿主，也没有运行 GPU 程序。`git diff --check` 通过。

编译宏、二进制哈希、受保护源码哈希、修正前 shader 和结果保存在
`tmp/ssgi-camera-preservation-audit/`。这份记录不把静态 IR、源码步骤或
二进制大小换算成 FPS。

## 仍存在的新方案成本

旧优化保留不代表新版本一定保持旧帧率。与 210 FPS 对应的旧基线相比，
现在多一份独立 screenBand、一张 policy、第三次 Gather，以及 SharedWorld
从三次增加到四次的同步。新增持久 payload 为 24 B/GI texel（ping-pong
合计），另有 sparse atlas 变化；这些属于隔离屏幕与世界采样的真实成本。

世界远档正常观测密度降低，但近档同面积预算与旧版本相同；大部分画面都
在近档时，并不会自动获得远档的预算节省。边界缓存身份检查也可能减少复用。
实际全帧或分 pass 性能需要同条件运行才能确认，本次没有该测量。
