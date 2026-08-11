# ra2hook

通过 **Syringe** 注入《红色警戒 2：尤里的复仇》`gamemd.exe` 的 32 位 DLL 扩展，与 Ares / Phobos 并存。

**核心目标**：提供一套**独立于 Ares/Phobos `[#include]`** 的 rules 注入机制——在它们的 include 全部处理完之后，再加载用户指定的 ini，且互不干扰。

完整设计见 **[DEVELOPMENT.md](./DEVELOPMENT.md)**。`0x679A1B` 后置注入点的
IDA 依据与实机测试步骤见 **[INJECT_HOOK_ANALYSIS.md](./INJECT_HOOK_ANALYSIS.md)**。
单机运行时热重载、回滚和外部 UI 见 **[RUNTIME_INI.md](./RUNTIME_INI.md)**。
本文件讲怎么构建与运行。

---

## 目录

```
ra2hook/
├── DEVELOPMENT.md            # 设计文档（唯一事实来源）
├── INJECT_HOOK_ANALYSIS.md   # 后置注入点分析与实机测试说明
├── RUNTIME_INI.md            # 单机运行时 INI、回滚、UI 与测试说明
├── README.md                 # 本文件
├── ra2hook.sln               # 解决方案（Debug/DevBuild/Release，均 Win32）
├── ra2hook.vcxproj           # 工程
├── ra2hook.props             # 编译设置（改写自 yrpp-spawner，已知可用）
├── ra2hook.ini               # dump / inject / runtime 配置
├── hooks.json                # v1 设计存档，程序不再读取（见 DEVELOPMENT §5.1）
├── .gitmodules               # YRpp submodule
├── .github/workflows/build.yml
├── src/
│   ├── Main.cpp              # DllMain
│   ├── Logger.h              # 文件日志
│   ├── Hooks.RulesInject.cpp # 启动阶段多目标 INI 注入
│   └── Hooks.Runtime.cpp     # 每帧 RuntimeTick
├── ui/                       # 独立 WPF/.NET 8 运行时控制面板
├── runtime/                  # 运行时 patch 文件目录样例
└── YRpp/                     # submodule，clone 后才有
```

---

## 构建（纯 GitHub Actions，无需本地 VS）

工程文件已完整放进仓库，`.github/workflows/build.yml` 在 `windows-latest` 上用 v143 工具集编译，push 即出 artifact。**不需要在本地装 Visual Studio。**

流程：

1. 把本目录作为一个 git 仓库推到 GitHub。**YRpp 必须是 submodule**（`.gitmodules` 已配好）：
   ```bash
   git submodule add https://github.com/Phobos-developers/YRpp.git YRpp
   git commit -am "add YRpp submodule"
   git push
   ```
2. Actions 自动触发。绿勾后进 workflow 页，下 `ra2hook-<sha>` artifact，里面是 `ra2hook.dll`、PDB 和自包含的 `ui/ra2hook-ui.exe`。
3. CI 已带 `submodules: recursive`，会自动拉 YRpp。

> CI 只编译，**跑不了游戏**。测试要把 artifact 下载到本地 RA2 安装里手动验（下节）。这个"push→等→下载→本地测"的循环是纯 CI 方案的固有代价。

### 本地构建（可选）

若日后想本地快速迭代：VS Installer 导入 `.vsconfig`（见 DEVELOPMENT §2.4）装 C++ 工作负载，然后 `msbuild /p:Configuration=Release ra2hook.sln`，或直接开 `ra2hook.sln`。与 CI 用同一套工程文件。

---

## 运行与验证（阶段 0 出口）

1. 把 `ra2hook.dll` 放进 RA2 目录（与 `gamemd.exe`、Ares/Phobos 同级）。
2. 用 Syringe 启动游戏（`Syringe.exe "gamemd.exe" -SPAWN ...`，或你现有的启动器）。
   - Syringe 会扫描目录内 DLL 的 `.syhks00` 段，自动应用我们的 hook。**无需 host 声明或握手**。
3. 进入一场遭遇战，退出后看 RA2 目录下的 `ra2hook.log`。

**当前候选挂点的日志标志**：日志里出现一行
```
[INFO] probe @0x679A1B: [RA2HookProbe]FromAresInclude=[...]
```

这一行直接回答核心设计的致命假设（DEVELOPMENT §4.4 / §6 待验证第 1 项）：

- 方括号内是 **1** → `0x679A1B` 在 Ares include 之后，后置注入假设成立。
- 方括号内 **空** → 当前 exe/扩展组合的时序假设不成立，需要回到 IDA 查找更晚的 rules 消费前窗口。

配套：在一个由 Ares `[#include]` 引入的 ini 里放一个探针键
```ini
[RA2HookProbe]
FromAresInclude=1
```

---

## 状态

- ✅ 设计已定（DEVELOPMENT.md v0.2）
- ✅ 工程文件齐备，CI 编译通过
- ✅ **dump 方向已实现**：INI/CSF/VXL/SHP/HVA 导出（`src/DumpIni.cpp` + `src/ArtMap.cpp`），已对真实 MO 数据核对
- ✅ **inject 代码已支持多个 INI**：注入目标完全由 `enabled/<target>/` 子目录决定；目录内按文件名排序后逐个合并，后写覆盖前写；文件内 `[#include]` 由独立原始解析器展开，不经过 Ares/Phobos 的 `ReadCCFile` hook
- ⬜ **inject 待实测**：主 rules 候选点为 `0x679A1B`；sound 已改为 `0x52C6C4` 预备覆盖层、`0x7510F6` 内存应用。空目录二进制探针已运行 60 秒，但正式构建和实际声音键尚未验证。art/ai/uimd/sound 与 mix 仍需用 CI artifact 完成实机回归
- ✅ **runtime 代码已实现**：`0x55DE3A` 游戏线程 tick、单机硬门禁、文件监视、完整状态重建、类型基线、回滚、命名管道和 `ui/` 外部控制面板
- ✅ **UI 已本地构建**：.NET 8 Release 零警告，并通过 win-x64 自包含单文件发布
- ⬜ **runtime DLL 待构建/实测**：本机没有 MSVC；tick、`LoadFromINI`/`SaveToINI`、Ares/Phobos 共存和实际字段效果必须用 Action artifact 在游戏中验证

**下一步：Action 构建 → 下载 artifact → 先完成启动 inject 回归，再按 `RUNTIME_INI.md` 的顺序验证单机应用、拒绝项、失败保留、删除回滚和退出对局回滚。**
