# OptiScaler MFG Ada 0.1.6 — FG-only / DLSS5 Companion

## 重点变化

- **移除发布版内置 NR 功能与界面**，由外部 RenoDX DLSS5 插件提供神经渲染。历史 NR 源码保留供维护，不代表发布版仍提供该功能。
- 兼容外部 DLSS5 插件的“呈现 / Present”模式，保留原生 DX11/DX12 DLSS 与 FG/MFG；不再使用 W12 超分后端。
- 修复崩铁 DX11 深度读写绑定冲突造成的全零 FG 深度，并正确保存恢复游戏绑定。
- 修复 FG 深度/MV 翻转的帧槽、首帧与尺寸问题；增加实际 FG 深度调试、对数增强和中文图例。
- 保留 FG 运行选项与软暂停路径。
- **禁用内置在线更新检测**，不创建版本检查线程，不请求上游更新接口。

## 验证与限制

用户已确认崩铁 DX11 + 外部 DLSS5 呈现 NR + FG 正常工作，最新深度读取修复生效。绝区零 DX12 接入保留，但本版尚未完成独立游戏回归。其他 GPU/游戏/HDR 组合不保证兼容。

D3D11 WARP 测试复现旧路径读零，生产绑定保护下恢复正确深度，并验证状态恢复；深度预览和翻转 GPU 回归通过。构建为 Release x64 FG-only。

## 升级注意

退出游戏并备份后替换 Opt DLL，保留原有 INI、OptiScaler/streamline、ReShade 和外部 DLSS5 插件及模型。外部插件继续使用“呈现”。调试预览增益不修改 FG 深度，正常游戏请关闭预览。

本包不再附带 NR 模型/forwarder，也不打包 NVIDIA FG 运行库；新安装通过包内 get_streamline.ps1 获取校验过的官方运行库，接受相关许可证后使用。现有 0.1.5 用户可以继续使用原有 FG 运行库。完整步骤见 README。

OptiScaler MFG Ada 0.1.6 removes the built-in NR product features, preserves native DLSS and frame generation, and works alongside the external RenoDX DLSS5 addon in Present mode. Online update checks are disabled. HSR DX11 is user-tested; ZZZ DX12 remains pending a dedicated regression test.
