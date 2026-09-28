# OptiScaler MFG Ada 0.1.6 — FG-only / DLSS5 Companion

面向原生 DLSS 的帧生成版本，可与外部 **RenoDX DLSS5 Neural Rendering 插件**配合使用。

**0.1.6 已移除 Opt 内置 NR 功能。** Opt 负责原生 DLSS 接入和 FG/MFG；神经渲染由外部 DLSS5 插件负责。Opt 不再提供 NR、模型、多 pass、皮肤调节或 NR 成片处理界面，也不加载内置 NR 处理链。

[下载 0.1.6](https://github.com/thx114/OptiScaler-MFG-Ada/releases/tag/mfg-ada-0.1.6) · [发布说明](docs/RELEASE-0.1.6.md) · [反馈问题](https://github.com/thx114/OptiScaler-MFG-Ada/issues)

## 本版变化

- 仅帧生成模式：保留原生 DX11/DX12 DLSS，不再使用 W12 超分作为本版后端。DX11 游戏的 DX11→DX12 **FG 桥接仍然保留**。
- 兼容外部 DLSS5 插件的“呈现 / Present”模式：在插件完成处理后、原生交换链翻转前捕获画面交给 FG。
- 修复 DX11 深度输出绑定与 FG 深度读取冲突导致的全零深度；恢复转换前的游戏绑定。
- 修复深度/运动向量翻转的帧槽资源复用、首帧和尺寸处理问题。
- 提供实际 FG 深度调试视图、对数增强及中文图例。预览增益/反转不修改真实 FG 深度；正常游玩请关闭调试视图。
- 保留帧生成倍率及运行设置，保留崩铁 DX11 的软暂停路径。
- **禁用 Opt 内置在线更新检测**：不启动检测线程、不向上游版本接口发出请求。请从本仓库手动更新。

## 验证范围

| 游戏 / 路径 | 状态 |
| --- | --- |
| 崩坏：星穹铁道 DX11 + RTX 40 DLSSG/MFG + 外部 DLSS5 呈现 NR | 本轮用户实测确认正常；覆盖 2x/6x 的此前测试，最新修复已确认深度恢复 |
| 绝区零 DX12 | 保留原生 DX12 DLSS/FG 接入；0.1.6 尚未完成独立游戏回归，不能视为保证兼容 |
| 其他游戏、GPU、HDR | 未作本次发布的完整验证 |

“兼容 DLSS5 插件”是上述组合的接入兼容，不表示所有模型、画质设置和游戏均无伪影。报告问题请附 GPU、驱动、游戏 API、FG 倍率、Opt 日志与 ReShade.log。

## 安装与升级

### 从 0.1.5 升级（推荐现有用户）

1. 退出游戏，备份现有 Opt DLL 和配置。
2. 用压缩包中的 OptiScaler.dll 替换现有 Opt DLL；若原安装使用重命名代理 DLL，沿用原文件名。管理器若维护多份副本，更新其实际加载的副本。
3. **保留原有 OptiScaler/streamline 运行库、ReShade、DLSS5 addon 及其模型/配置**。不要把 Opt DLL 覆盖到游戏的原生 DLSS 模型文件上。
4. 保留自己的 INI。旧 NR 配置即使仍在文件中，本版也不会启用 Opt 内置 NR。
5. 游戏选择 DLSS；Opt 使用 Upscaler 输入和 DLSSG 输出；外部插件保持“呈现 / Present”。先确认正常画面，再开启所需 FG 倍率。

### 新安装

发布包不包含外部 DLSS5 插件、NR 模型或 NVIDIA FG 运行库。需要分别安装兼容的 ReShade/DLSS5 插件及它们要求的运行库；**无需为了 Opt 再安装 nvngx_dlssnr.dll 或旧 NR forwarder**。

包内提供最小默认 INI、Windows 安装脚本和经过固定版本/哈希验证的 FG 运行库下载脚本。新安装使用 setup_windows.bat 配置注入；已装 ReShade 时先备份，避免两个代理 DLL 使用同一文件名。

阅读 [FG 运行库与许可证说明](docs/DLSS-FRAME-GENERATION.md)，接受相关许可证后，在解压目录运行（将路径替换为实际安装位置）：

```powershell
.\get_streamline.ps1 -Destination 'D:\Path\To\Game\OptiScaler\streamline' -AcceptNvidiaLicenses
```

默认配置关闭 FG，首次运行确认 DLSS 与 NR 正常后手动启用。不要在反作弊保护的多人游戏中使用。

## 编译

Visual Studio 2022 C++ 工具链、Windows SDK，初始化仓库所需子模块后：

```powershell
MSBuild.exe OptiScaler.sln /p:Configuration=Release /p:Platform=x64 /p:OptiScalerFgOnly=true /t:OptiScaler /m:1
.\package_fg_only.ps1 -Version 0.1.6
```

本分支默认构建 FG-only。历史 NR 实现仍保留在源树中供维护和参考；“移除 NR”指 **0.1.6 发布产品不再提供或启用该功能**，并非宣称删除了所有历史源码。旧版 NR 文档不适用于此发布，见 [历史 README](docs/LEGACY-NR-README.md)。

## Credits / Licence

基于 [OptiScaler](https://github.com/optiscaler/OptiScaler) 及其社区分支，感谢 DLSSNR、RenoDX 和 RTX40 MFG 解锁相关项目的贡献。

[完整致谢](docs/CREDITS.md) · [许可证](LICENSE) · [RTX40 MFG 技术说明](docs/RTX40-MFG.md)

