# 运行时精确遮挡：性能回退与撤回

2026-11-03。用户反馈：逐贡献探针精确遮挡使 GI 密集视角约 60 FPS 降至 8 FPS，成本不可接受。该方案已撤回；独立的烘焙太阳遮挡自命中过滤保留。

后续添加了默认关闭的[方向遮挡缓存实验](VXGIProbeDistanceExperiment.md)。下文的源码/DXIL 字节一致性是撤回当时的验证记录；实验代码加入后不再以源码相同描述当前状态。精确几何可见性射线仍未恢复，主目录可执行程序未被实验替换。

## 改动与代价

旧实验在每条世界射线命中后，最多追加八次精确 Mesh SDF/三角形遮挡查询，放大了 GI 密集区域的几何工作。此前的正确性通过不能代替实时性能验收。

- 三个运行时查询 shader 与 PipelineManager 恢复为修改前原文件，字节核对一致。
- Trace、SharedWorld、SharedTrace、Clear、Repair 五个变体以相同 DXC O3/WX 参数编译，与修改前保留副本的 DXIL 逐字节一致。
- 恢复六个世界 root buffer、八参数 root signature 和原角点 unroll；移除新增五个资源绑定及运行时可见性辅助 shader。
- 正式 Pipeline/Scene/Lighting、模型、射线预算与分辨率保持原值；没有降档处理性能回退。
- 保留烘焙小修与仅保存时执行的诊断导出。普通帧不增加可见性射线或缓存读回。

原跨墙插值缺陷因此仍是已知画质限制。旧正确性图与报告仅作历史记录，不再代表当前生产实现。

## 验证及边界

Debug/Release 构建、UI 架构检查通过。两次隔离的无 DLSS 短运行完整退出、D3D12 warning/error 为零；原始 SH 确认烘焙小修仍有效：封闭太阳组全零，开孔正对照有效。

按正式设置（2560×1600、DLSS Quality、近/远16/64、HBAO启用）的固定室内机位完成了35帧并保存正常合成，渲染报告零 warning/error；进程退出清理超过300秒，由 runner 终止，不能记作完整关闭通过。详细记录为 `dlss-run-limitation.json`。

没有重新测量用户反馈机位的 steady-state FPS/GPU timestamp，不宣称已现场确认恢复60FPS。确认的是额外追踪已移除、运行时 GPU 程序恢复原版本；不从隐藏进程总时长推算帧率。

产物在 `tmp/vxgi-runtime-visibility-revert/`：`validation.json`、`shader-comparison.json`、构建日志、原始抓帧与 `before/` 撤回前快照。
