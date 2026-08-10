# ra2hook

通过 **Syringe** 注入《红色警戒 2：尤里的复仇》`gamemd.exe` 的 32 位 DLL 扩展，与 Ares / Phobos 并存。

**核心目标**：提供一套**独立于 Ares/Phobos `[#include]`** 的 rules 注入机制——在它们的 include 全部处理完之后，再加载用户指定的 ini，且互不干扰。

完整设计见 **[DEVELOPMENT.md](./DEVELOPMENT.md)**。本文件讲怎么构建与运行。

---

## 目录

```
ra2hook/
├── DEVELOPMENT.md            # 设计文档（唯一事实来源）
├── README.md                 # 本文件
├── ra2hook.sln               # 解决方案（Debug/DevBuild/Release，均 Win32）
├── ra2hook.vcxproj           # 工程
├── ra2hook.props             # 编译设置（改写自 yrpp-spawner，已知可用）
├── ra2hook.config.json       # 运行时配置样例（放到 RA2 目录）
├── hooks.json                # v1 设计存档，程序不再读取（见 DEVELOPMENT §5.1）
├── .gitmodules               # YRpp submodule
├── .github/workflows/build.yml
├── src/
│   ├── Main.cpp              # DllMain
│   ├── Logger.h              # 文件日志
│   └── Hooks.RulesInject.cpp # 核心 hook（阶段1=探针，阶段2=注入）
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
2. Actions 自动触发。绿勾后进 workflow 页，下 `ra2hook-<sha>` artifact，里面是 `ra2hook.dll`（+ pdb）。
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

**成功标志**：日志里出现一行
```
[INFO] probe @0x679A15: [RA2HookProbe]FromAresInclude=[...]
```

这一行直接回答核心设计的致命假设（DEVELOPMENT §4.4 / §6 待验证第 1 项）：

- 方括号内是 **1** → `0x679A15` 在 Ares include 之后，假设成立，进入阶段 2。
- 方括号内 **空** → 此点早于 include 处理，**换注入点**（候选 `ReadCCFile` 返回处）。

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
- ✅ **inject 核心已实现**：`0x679A15` 注入点探针通过（写入 `[E1]Strength` 被引擎真实采纳），合并逻辑见 `src/Hooks.RulesInject.cpp`——按目标目录注入：`ra2hook\inject\enabled\<rules|ra2md|art|ai|uimd>\*.ini`（目标挂点见 TODO.md），`[Inject] Files=` 显式列表仍可直注 rules，后写覆盖；inject 文件自己的 `[#include]` 会由 ra2hook 独立展开，不干扰 Ares/Phobos 原 include 链；`ra2hook\inject\mix\*.mix` 注册进引擎文件系统（内置 INI/SHP/VXL/PCX 用）
- ⬜ **inject 待实测**：rules/ra2md 目录注入与 mix 装载尚未本地编译/进游戏验证（本机暂无 C++ 工作负载），推 CI 或加装 VS 后验证
- ⬜ 阶段 3：运行时功能（热键改值等）

**下一步：push 触发 CI → 下载 artifact → 把 dll 放进 RA2 目录 → `[Inject] Enabled=yes` + 放一个 `index.ini`，用 `[#include]` 指向 mix/散装里的规则 ini → Syringe 启动 → 看 `ra2hook.log` 的 inject/include/mix 行与游戏内效果。**
