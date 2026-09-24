# TaskMgr

极小体积的 Windows 任务管理器：纯 C + Win32 API，不依赖 .NET / MFC / CRT。

## 功能

- **进程列表**：PID、名称、完整路径、内存 (MB)、CPU (%)
- **列排序**：点击列头切换升/降序（同列再点一次反向）
- **搜索过滤**：按名称 / PID / 路径大小写不敏感匹配（列表上方编辑框）
- **进程数汇总**：状态栏显示「共 N 个进程 · 显示 M 个」
- **结束任务**：`TerminateProcess`；受保护进程（PPL）失败时弹出明确错误提示
- **打开文件位置**：`explorer /select,"路径"`
- **复制路径**：完整路径写入剪贴板（Unicode）
- **刷新**：手动「刷新」按钮 / F5；可选「自动刷新 (2秒)」
- 列表刷新时冻结重绘、按 PID 恢复选中与滚动位置，避免闪烁/跳动

## 列含义

| 列 | 含义 |
|---|---|
| PID | 进程标识符 |
| 名称 | 可执行文件名（含扩展名） |
| 完整路径 | `QueryFullProcessImageName` 绝对路径；PID 4/100 对齐系统 TM 显示 `ntoskrnl.exe`；无权限时 `(无法访问)` |
| 内存 (MB) | 私有提交量 `PrivateUsage`，单位 MB（一位小数） |
| CPU (%) | 相对上次采样的 CPU 时间占比（按逻辑处理器数归一化，上限 99.9+） |

## 要求

- Windows 10/11 x64
- **管理员权限**（内嵌 `requireAdministrator` manifest；否则无法结束/查询多数进程）

## 使用

1. 以管理员身份运行 `taskmgr.exe`
2. 点击列头排序；在搜索框过滤
3. 选中行 →「结束任务」/「打开文件位置」/「复制路径」
4. 「刷新」或勾选「自动刷新 (2秒)」

## 构建

需要 [MSYS2](https://www.msys2.org/) UCRT64（`gcc` + `windres`）：

```bat
build.bat
```

脚本会检测本机 MinGW / MSVC（若存在且带 Windows SDK 头文件），分别尝试构建并**保留更小的产物**为 `taskmgr.exe`。当前 MinGW `-nostdlib` 构建约 **16 KB**，仅导入 `kernel32` / `user32` / `shell32` / `comctl32`。

无 CRT 关键参数：`-nostdlib`、`-Wl,-e,Entry`、自带 `memset`/`memcpy` 等；无浮点，CPU/内存为整数格式化。

## 自检（CI / 本地）

```bat
taskmgr.exe -selftest
```

枚举进程、解析自身路径并校验过滤逻辑，成功退出码 `0`；失败为 `2`–`7`。

## 技术说明

| 项 | 方案 |
|---|---|
| 进程枚举 | `CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS)` |
| 路径 | `OpenProcess(QUERY_LIMITED_INFORMATION)` + `QueryFullProcessImageNameW` |
| 内存 | `GetProcessMemoryInfo` → `PrivateUsage` |
| CPU | `GetProcessTimes` 内核+用户时间差 / 墙钟差 / 逻辑 CPU 数 |
| 结束 | `OpenProcess(PROCESS_TERMINATE)` + `TerminateProcess`（区分访问拒绝） |
| UI | 单窗口 + ListView 报告视图；`WM_SETREDRAW` 防闪烁 |

## 许可

[MIT](LICENSE)
