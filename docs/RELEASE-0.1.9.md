# OptiScaler MFG Ada 0.1.9

- 原神 native DX11 提交整组 color/depth/motion/mask 的屏幕坐标版本给外部 RenoDX NR；depth plane 转为 R32_FLOAT，Y metadata 取反，DLSS 输出翻回游戏空间，返回前恢复原参数。
- FG shared depth/motion 重写前提交并等待所有旧 UI reader，保留 CPU 退休同步。失败的 cross-API GPU Wait 性能试验不在此发布版本内。
- DX11/DX12 FG resize 使用各自的 buffer count/flags，并将 sRGB映射为DX12 flip支持的格式；保持NGX输出原格式。
- OptiScaler DX12菜单使用提交完成fence，替换device/queue后重建后端；字体上传资源创建/Map失败安全返回，避免Release版null访问。
- 与Bridge 2.3.2配合使用，Bridge按角色/主场景FSR实例隔离上下文和prepared资源。

## 验证与限制

用户确认NR闪烁消失、depth/motion匹配；最新CPU同步版完成反复C、活动、进房间/切地图，日志无GPU HUNG/设备丢失/等待失败，正常退出。不是所有机器、无限次切换或所有游戏保证。实际GPU回归覆盖原生guide转换/packed depth/sRGB输出/失败恢复、DXGI resize、ImGui字型分配失败及菜单提交同步、D3D11/D3D12共享读写退休。

原始Present频率有约14fps低帧率窗口，NR两pass成本和切换长帧仍需优化。此版优先保留正确性，不宣称提高fps。Genshin专用预设启用[DLSS] NativeScreenSpaceGuides=true；其他游戏默认false。

发布包只包含本DLL、配置、源码许可证说明和官方运行库下载器，不携带NR模型或NVIDIA FG运行库。已有安装可继续使用其运行库。开启Genshin NR需要外部RenoDX DLSS5 addon及其运行库。
