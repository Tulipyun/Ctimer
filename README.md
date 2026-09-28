<img src="icon.png" width="96" height="96" alt="Ctimer 图标">

# 定时点击器 · Ctimer

[![Windows build](https://github.com/Tulipyun/Ctimer/actions/workflows/build.yml/badge.svg)](https://github.com/Tulipyun/Ctimer/actions/workflows/build.yml)
[![Release](https://img.shields.io/github/v/release/Tulipyun/Ctimer)](https://github.com/Tulipyun/Ctimer/releases/latest)

Windows x64 桌面定时鼠标/键盘工具，使用 **C++20 + Win32**。通过多源 NTP 校准内部时间，在每小时指定的分、秒和毫秒提交输入，支持自动准备、重复预约和窗口置顶。

## 下载

在 [Releases](https://github.com/Tulipyun/Ctimer/releases/latest) 下载 exe 或便携 ZIP。运行文件名保持为 Ctimer.exe，中文界面与 Windows 文件属性名称为“定时点击器”。

无需安装 Python、.NET 或 Qt。启用系统时间写回时，Windows 可能请求管理员授权。更新时关闭旧程序再替换 exe，保留已有 Ctimer.ini；发布包的示例配置不会覆盖你的预约。

## 功能

- 分、秒、毫秒三栏输入，自动补齐格式；默认鼠标左键，也支持键盘组合键。
- 保存时间、操作、重复参数与窗口位置，启动后自动恢复预约。
- 多源自动校时，常用源约 10 秒查询；新增 NTSC、CNNIC、中国 NTP Pool 等候选，按实际路径和运营商去重。
- 低延迟历史采样、明确故障预算的区间选择、按路径稳健频率回归；主用源评分与误差范围分开计算。
- 默认在目标前 3 分钟进入精校准，显示进度；到前 1 分钟停止精校准并冻结模型。
- 目标前 60 秒冻结校时，鸣音并显示准备状态；操作完成后恢复同步、预约下一小时。
- 每次输入包含按下、保持和释放，定时起点为按下提交。
- 精简窗口、置顶、同步状态、上次成功对时后的时间，以及本机与 NTP 推定时间的差值。
- CSV 诊断、模拟 NTP/状态测试、嵌入图标和窗口位置恢复。

## 快速使用

1. 启动程序，按需要确认 Windows 管理员授权，等待状态栏显示同步成功。
2. 输入每小时的分、秒、毫秒并选择操作。示例：分填 5 补为 05，毫秒填 1 补为 100，空栏补零。
3. 开启“自动预约”后，输入稳定约一秒自动预约；将光标或前台焦点移到需要接收输入的软件。
4. 到达目标前一分钟，程序冻结时间模型并提示准备；成功后继续预约下一小时。
5. 可用取消按钮停止当前预约。关闭“自动预约”可禁用下次启动自动执行。

紧急停止默认 Ctrl+Alt+F10；冲突时尝试 Ctrl+Shift+F11。具体行为、配置格式和权限说明见 [使用说明](docs/USAGE.md)。

## 从源码构建

在 Windows PowerShell 中运行：

~~~powershell
./scripts/build.ps1
./scripts/package-release.ps1
~~~

构建会下载固定版本并校验 SHA-256 的 LLVM/MinGW-w64 工具链，运行测试后生成 Windows x64 程序。版本号集中在 [VERSION](VERSION)。详细说明见 [构建与发布](docs/BUILDING.md)。

~~~text
src/         应用、NTP、时钟模型、执行与 Win32 界面
tests/       算法、协议、状态、窗口与资源验证
resources/   图标、提示音、版本和清单模板
config/      可分发配置模板
scripts/     工具链、构建、格式化、打包与可选 GUI 验证
docs/        使用、架构、测试及历史设计记录
licenses/    第三方许可
~~~

参见 [程序结构](docs/ARCHITECTURE.md) 和 [更新记录](CHANGELOG.md)。构建产物、运行日志、个人配置、配置备份和凭据不进入源码仓库；二进制通过 Release 分发。

v0.6 的算法、来源策略与验证结果见 [同步升级说明](docs/v0.6-同步升级与测试.md)。精校准进度表示时间窗口进度，不表示精度达标比例。

## 精度说明

NTP 校时误差、Windows 输入提交误差和目标软件处理延迟是不同指标。界面的带符号数值表示本机时间减去 NTP 推定时间；UTC ± 是不确定度预算，不能当作经过独立验证的真实 UTC 偏差。

公网路径不对称和 Windows 调度仍会影响结果，项目不承诺严格的真实 UTC ±1 ms 或硬实时上限。历史等待测试出现过超过 1 ms 的尾延迟；详见 [测试记录](docs/v0.4-测试与变更记录.md)。重要用途应在自己的硬件、网络与目标软件上验证。

## 许可

项目当前未指定独立的开源许可证。第三方组件及其许可见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)；项目公开可见不自动授予超出这些许可的权利。
