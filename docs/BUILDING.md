# 构建与发布

## PowerShell 构建（已验证）

需要 Windows x64、PowerShell、网络连接。构建脚本会按需下载固定版本 LLVM/MinGW-w64，并验证 SHA-256；不修改全局 PATH。

```powershell
./scripts/build.ps1
./scripts/package-release.ps1
```

默认生成 `dist/Ctimer.exe`，打包生成带版本号的 exe、便携 ZIP 和 `SHA256SUMS-v版本.txt`。`VERSION` 是版本号的唯一来源，脚本据此生成 C++/资源版本头文件和应用清单。

旧版本正在运行时，可以指定其他输出名：

```powershell
./scripts/build.ps1 -ExecutableName Ctimer-preview.exe
./scripts/package-release.ps1 -ExecutablePath dist/Ctimer-preview.exe
```

发布包只包含可执行文件、示例配置、使用说明和第三方许可，不包含个人 `Ctimer.ini`。升级时关闭旧程序，再替换 exe；保留原配置文件。

## 图标与代码格式

`icon.png` 是图标源文件。构建时使用 Windows 自带的 .NET 图形库生成多尺寸 `resources/app.ico`，随后嵌入 exe。运行时无需图片文件。

```powershell
./scripts/format-code.ps1
```

格式化工具使用相同工具链提供的 clang-format；样式由 `.clang-format` 管理，不调整 Windows 头文件的包含顺序。

## CMake

也可使用已安装的 MSVC、Windows SDK 和 CMake：

```powershell
cmake -S . -B build/msvc -A x64
cmake --build build/msvc --config Release
ctest --test-dir build/msvc -C Release --output-on-failure
```

PowerShell/LLVM 构建路径用于本地验证和 GitHub Actions；MSVC 路径保留兼容工程配置，尚未在本项目报告中独立完成验收。

## 测试范围

默认构建运行 `CtimerTests.exe`：NTP 数学与协议、localhost UDP、来源筛选、冻结竞争、失败状态、取消、配置持久化、窗口位置恢复及嵌入图标。测试不向其他软件发送输入，不修改系统时间。

v0.6.1 另外验证主时钟的实际 GDI 绘制：使用不激活的离屏窗口检查局部重绘、擦除消息、跨秒、缩放和资源复用，不会覆盖桌面或抢占焦点。

以下为可选、需要可交互桌面的真实输入测试：

```powershell
python ./scripts/verify-auto-ui.py --executable ./dist/Ctimer.exe
```

自测窗口需要保持前台。测试使用隔离配置和本地 NTP 服务，只向自己的窗口发送 F8，并验证重启后的预约恢复；管理员请求与系统时间写回被关闭。

等待延迟基准使用 `build/CtimerTests.exe --benchmark`，它不代表真实 UTC 或目标应用响应精度。

v0.6 添加了 2,000 个随机单故障覆盖案例、路径回归、精校准与端点去重验证。可使用以下命令在当前网络采样六分钟，并分析诊断日志（不会创建点击预约或写系统时间）：

```powershell
./build/CtimerTests.exe --observe 360
python ./scripts/analyze-ntp-log.py ./build/ntp-observation-实际输出编号
```

`--survey` 只做一轮公开源查询，不能用来证明长期精度。观测中的 `CtimerTests.exe` 需运行结束后再构建，以免 Windows 锁住测试可执行文件。测试输出的 JSON 和 CSV 默认保留在 build 中，不进入公开仓库。

## 仓库与 Release

- 源码、资源、文档、配置模板和构建脚本进入 Git。
- `build/`、`dist/`、运行日志、个人预约配置和备份均被忽略。
- GitHub Actions 在 Windows 上构建、测试并保存可下载的构建产物。
- 正式版本使用 `v版本号` 标签，Release 提供 exe、ZIP 和 SHA-256 校验文件。
- 仓库暂未指定项目代码的开源许可证；发布为公开仓库不自动授予额外许可。第三方组件许可单独列于 `THIRD_PARTY_NOTICES.md`。
