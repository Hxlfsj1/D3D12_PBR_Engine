# Cornell Box：生产探针插值遮挡修复

**状态：2026-11-03 已撤回，不属于当前生产实现。** 用户实测反馈 GI 密集视角从约 60 FPS 降到 8 FPS，实时成本不可接受。下文保留当时的正确性对照，不能将其通过视为性能验收通过。生产 shader、八参数 root signature 和六个世界资源绑定已恢复原版；烘焙太阳遮挡的小修独立保留。

撤回验证记录：`tmp/vxgi-runtime-visibility-revert/`。五个运行时 Trace/SharedWorld/SharedTrace/SharedClear/Repair 变体的 DXC O3/WX 产物与修改前副本逐字节一致，未通过减射线、降分辨率或关闭 GI 来规避回退。用户报告的帧率是现场证据，不是本轮重新测量的 GPU timestamp 结果。

2026-11-02。生产近段 Mesh SDF、远段 Global SDF 和 Repair 的 raw SH 查询已加入逐贡献探针的双面遮挡检查。复测中，封闭盒的盒外光源漏光归零，开孔后的合法进光保持非零。

修复前的定位与原始证据保留在 [漏光测试报告](VXGICornellLeakTestReport.md)。本轮在独立目录 `tmp/vxgi-cornell-visibility/` 运行，未覆盖修复前的 `tmp/vxgi-cornell-leak/` 产物。

## 改动

- 每个已分配、有效且已更新的贡献探针，在插值前追踪“探针中心 → 世界查询点”的有限线段。复用已有 Mesh SDF 加速、精确三角形求交、实例材质映射与 alpha 覆盖，不限定为近段主射线的最近两个模型。
- 不做正面或背面剔除，任何中途不透明三角形都阻挡贡献；invalid/exhausted 按不可见处理，并累计查询失败数。只有通过检查的探针进入 SH 加权和及归一化。
- 若线段恰好结束在真实表面上，不能把目标面本身一律当成遮挡，也不能允许墙另一侧的探针借此贡献。对距端点不超过 `min(1e-5, segmentLength*1e-5)` 的命中，比较探针与原主射线接收侧：同侧才接受；交换三角形绕序不会改变判断结果。中间命中仍拒绝，不忽略整个三角形或实例。
- 原主射线、0–2/2–8 距离分段、射线预算、探针布局、分类/采集和 SH 表示均保持原值。没有新增现场着色、光照反馈或全亮回退。
- SSGI root signature 从 8 扩为 13 个参数（26 DWORD）及静态 sampler s0，绑定已有的 cell offsets、triangle references、triangles、materials 和实例材质映射。所有读取声明到 RDG，仍先绑定 engine descriptor heap 再绑定 root signature/参数；不新增场景几何或光照缓存分配。
- 生产五个 Trace/SharedWorld/SharedTrace/SharedClear/Repair shader 变体显式启用 `VXGI_MESH_SDF=1` 与 `VXGI_PROBE_RUNTIME_VISIBILITY=1`。旧无几何的重建重载只保留给独立诊断，生产调用没有该回退。

主要源码：`Shaders/VXGIProbeVisibilityCommon.hlsli`、`VXGIProbeRadianceCommon.hlsli`、`SSGIHybridCommon.hlsli`、`Shaders_For_SSGI.hlsl`；绑定位于 `Passes/SSGIPass.h`、`Graphics/PipelineManager.h`，材质 mip 从正式 VXGI 配置传入。

## 相同输入的修复前后对照

1280×800，GI 640×400，AA=None，HBAO 关闭，保留 GI 时空滤波。探针格距 0.5、padding=1、Mesh SDF 格距 0.1、世界近/远采样 16/64。相机、模型/材质、灯光与测试设置不变，B0/B1/B2 各自重新初始化并填满探针。

下表为第 256 帧的线性 `E/pi` 亮度，不乘接收材质，不经过 AA/ToneMap：

| 用例 | 修复前平均 | 修复后平均 | 修复后最大 |
| --- | ---: | ---: | ---: |
| B0：封闭，外灯关闭 | 0 | 0 | 0 |
| B1：封闭，只开外灯 | 0.000168807 | **0** | **0** |
| B2：只开外灯，后墙开孔 | 0.0116617 | **0.0114513** | 0.0900471 |

B1 超过原检出门槛 1e-5 的像素比例由 13.052% 降到 0；packed Mesh/Global owner atlas、raw、world gather、reconstructed、最终 filtered 均为零。B0 仍为零。

B2 平均值保留原来的 98.195%，正对照没有被压黑。这个比值表示该场景的总信号变化，不等于“正确反弹保留率”或路径追踪误差：修复前 B2 也可能同时包含合法进光和跨墙错误贡献。

Debug 与 Release 的 B1 都为零，B2 都保持上述非零值。最终 B0/B1/B2 Debug 与 B1/B2 Release 五次记录的四个相关 shader SHA256 均与当前正式源文件一致。

修复前后 A/B0/B1/B2 fixture 的模型哈希逐项一致；B1 和 B2 分别对应的深度数据修复前后完全一致。正式 Cornell/Plain 模型及 Scene/Lighting/Pipeline 文件哈希均未改变。

![统一标尺的线性 GI 修复前后](../tmp/vxgi-cornell-visibility/before-after.png)

图中上排为封闭 B1，下排为开孔 B2；左列修复前、右列修复后。统一灰度标尺为黑=0、白≥0.01 `E/pi`，合法较强进光会饱和；不是正常曝光画面。

## 端点、正面和背面的专项验证

测试副本 `B1Endpoints` 直接调用同一个生产 `VXGIProbeContributorVisible`，使用实际已上传的 Cornell 场景资源，覆盖六项断言：

1. 墙外探针到墙内查询点：背面命中必须遮挡。
2. 墙内探针到墙外查询点：正面命中必须遮挡。
3. 查询点恰在后墙上、原主射线从室内到墙：室内探针可贡献。
4. 同一端点与主射线：室外探针被拒绝。
5. 原主射线反向、从室外到墙：室外探针可贡献。
6. 反向主射线：室内探针被拒绝。

该副本仅把断言结果编码为颜色，green>0 表示确实执行，red>0 表示断言失败；它不是物理 GI 数据，不计入漏光亮度统计。断言与实际执行见 `tmp/vxgi-cornell-visibility/endpoint-validation.json`。

实际结果：六项断言通过，owner atlas 的 red 最大值为 0、green 最大值为 1。修复后的墙角纯 GI 已实际查看，截图为 `tmp/vxgi-cornell-visibility/A/Captures/gi-160-437221984/preview.png`。

## 覆盖与限制

- Debug/Release 构建、编辑器 UI 架构检查和生产 Trace 的独立 DXC O3/WX 编译通过；实际隐藏运行记录、D3D12 验证与探针填充见 `validation.json`。构建仍有项目既有的类型转换、编码及第三方 PDB 警告。
- 汇总保留 9 次完成的运行（7 Debug、2 Release），均完整退出、D3D12 warning/error 为零，Debug 验证均可用；最终关键五次的 shader 哈希与正式源码一致。开发期重跑不当作额外独立样本。
- B1 有 845 个有效探针，均完成 32 次采集；B2 有 851 个有效探针，均完成 32 次采集；未分类、采光/追踪失败为零。未把未填满或无有效探针的全黑当作通过。
- 原开口 Cornell 的内部正常合成与墙角纯 GI 也保留了实际抓帧。早期开发抓帧位于同目录，最终关键五次复测另用 shader 哈希确认，见 `before-after.json` 的 `final_source_runs`。
- 本轮确认当前静态 Cornell 的穿墙能量问题改善，不承诺所有模型、动态物体、支持缺口或 GI 能量表示均正确。Alpha 路径复用现有实现，本轮没有新增纹理 cutout 专项场景。
- 每个有效候选最多增加一条短可见性射线。使用精确几何会增加运行成本，八角点循环保持动态循环以避免复制八份追踪代码。本轮没有 GPU timestamp 的受控性能对照，不把隐藏进程总时长当 GPU 帧耗或 FPS，也不声称性能已验收。

## 复现

```powershell
# 已构建 Debug/Release 后，生成隔离的正式修复场景。
$env:VXGI_CORNELL_OUTPUT='tmp/vxgi-cornell-visibility'
python Tests/PrepareVXGICornellLeak.py
foreach ($case in @('B0','B1','B2')) {
    powershell -NoProfile -ExecutionPolicy Bypass -File Tests/RunVXGICornellLeak.ps1 -OutputDirectory tmp/vxgi-cornell-visibility -Case $case -Frame 256
}
foreach ($case in @('B1','B2')) {
    powershell -NoProfile -ExecutionPolicy Bypass -File Tests/RunVXGICornellLeak.ps1 -OutputDirectory tmp/vxgi-cornell-visibility -Configuration Release -Case $case -Frame 256
}
python Tests/PrepareVXGICornellLeak.py --endpoint-control
powershell -NoProfile -ExecutionPolicy Bypass -File Tests/RunVXGICornellLeak.ps1 -OutputDirectory tmp/vxgi-cornell-visibility -Case B1Endpoints -Frame 64
python Tests/AnalyzeVXGICornellLeak.py
```

分析需要保留修复前 `tmp/vxgi-cornell-leak/` 的 fixture、256 帧 B1/B2 记录及抓帧，以生成同输入对照；缺少它们时不能凭新图重新生成旧版基线。
