# ra2hook 开发文档

> **文档版本**：v0.2（技术底座已确定，无可运行代码）
> **v0.1 → v0.2 的变化**：确定走 Syringe + YRpp 生态；地址不再需要逆向核实（YRpp 已提供）；`hooks.json` 的职责从"地址表"变为"运行时配置"
> **事实来源**：本文中标注为「已核实」的内容均来自 YRpp / Phobos 上游源码与 Ares 官方文档的实际抓取，见 §9

---

## 1. 项目定义

ra2hook 是一个通过 **Syringe** 注入《红色警戒 2：尤里的复仇》`gamemd.exe` 的 **32 位 DLL 扩展**，与 Ares / Phobos 并存。

### 1.1 核心需求（唯一的第一优先级）

**独立于 Ares / Phobos 的 INI 注入机制**：

- 在 Ares / Phobos 的 `[#include]` 全部处理完毕**之后**，加载用户指定的一批 rules ini
- 与原有 `[#include]` 机制**完全独立**：不占用 `[#include]` 段、不参与其包含链、不改变其行为
- 注入内容对引擎的类型解析可见，即实际生效

### 1.2 为什么不能用 Ares 的 `[#include]`

Ares 自 0.1 起原生支持（[文档](https://ares-developers.github.io/Ares-docs/new/misc/include.html)）：

```ini
[#include]
1=rules_sw.ini
2=rules_vehicles.ini
```

语义：主文件加载完毕后处理，**深度优先**，被包含文件可再声明自己的 `[#include]`，无深度限制，重复键后读胜出，路径相对 RA2 目录，无散装文件时回落 MIX（路径含子目录则不回落）。

**它满足不了本项目的需求**，原因有两条:

1. **顺序不可控**。一旦写进 `[#include]`,加载顺序就由包含链的深度优先遍历决定,无法表达"在所有 include 之后"。
2. **不独立**。它就是 Ares 机制的一部分,会与 mod 自身的 include 配置相互干扰。

因此核心 hook 必须存在。

### 1.3 后续能力（不阻塞核心）

| 能力 | 依赖 | 优先级 |
|---|---|---|
| 生效 rules 快照 dump | 核心注入点 | 随核心附带 |
| 运行时改值（热键改单位/武器数值） | 每帧 tick + 单位创建 | 后续 |
| 素材 dump（SHP → .shp） | 每帧 tick | 可选 |

### 1.4 明确的非目标

- ❌ 不生成魔改 `gamemd.exe`（不做静态补丁）
- ❌ 不复刻 Ares / Phobos 已有功能
- ❌ **不保证联机同步**。运行时改值必然 desync,定位为单机与调试工具
- ❌ 不做反检测、不绕过任何校验
- ❌ **不自研 hook 加载器**。见 §2.1

---

## 2. 技术底座

### 2.1 为什么必须用 Syringe,而不是自研加载器

`hooks.json` v1 的设计假设是"DLL 自己读配置、自己写内存补丁、换点不重编译"。**在已有 Syringe 的环境里这个方案是错的**:

- Syringe 在同一地址上会把多个 host 的 hook **串联**依次调用;自研加载器直接覆盖字节,会把 Ares 的 patch 拍掉。
- 两套注入机制并存于一个进程,先崩的是后来者。
- Syringe 是唯一的仲裁者,撞点由它协调;绕过它就等于放弃协调。

**代价**:hook 地址变成编译期常量（写在 `DEFINE_HOOK` 宏里）,换地址要重编译。

**保留运行时灵活性的做法**:所有 hook 一律安装,但每个 handler 开头读配置决定是否立即 `return`。这样"运行时开关"能力几乎完整保留,只有"运行时换地址"这一项失去——而后者在有 Syringe 仲裁的前提下需求很低。

### 2.2 依赖

| 依赖 | 作用 | 获取方式 |
|---|---|---|
| **Syringe / SyringeEx** | 注入器,读 DLL 导出表决定挂哪些点 | [Ares-Developers/Syringe](https://github.com/Ares-Developers/Syringe) / [Phobos-developers/SyringeEx](https://github.com/Phobos-developers/SyringeEx) |
| **YRpp** | 引擎类型定义 + 命名地址库 + `DEFINE_HOOK` 宏 | git submodule → `https://github.com/Phobos-developers/YRpp.git` |

**YRpp 是本项目最大的杠杆**。它把 `RulesClass`、`CCINIClass`、`TechnoClass`、`WeaponTypeClass` 的内存布局声明成了 C++ 类（含虚表与成员偏移）,并给出了成员函数的实际地址。v0.1 文档里"阶段 0:IDA 逆向核实 7 个地址"因此**整体作废**——地址已经是现成的。

### 2.3 编译配置（已核实,抄自 Phobos.props）

这些不是建议值,是 YRpp 生态**必须**的设置。偏离任何一项都可能编译通过但运行时崩溃:

| 设置 | 值 | 为什么 |
|---|---|---|
| `Platform` | **Win32**（仅此一个） | YRpp 只支持 32 位;Phobos.sln 里 `x86` 全部映射到 `Win32`,无 x64 配置 |
| `PlatformToolset` | **v143** | ✅ 上游当前值。**旧文档要求的 `v141_xp` 已不适用** |
| `ConfigurationType` | `DynamicLibrary` | |
| `LanguageStandard` | `stdcpp20` | |
| `ExceptionHandling` | **false** + `HAS_EXCEPTIONS=0` | **不能用 try/catch**,见 §4.4 |
| `RuntimeLibrary` | `MultiThreaded`（静态 CRT） | 不依赖 VC 运行库,DLL 可直接分发 |
| `CallingConvention` | `StdCall` | |
| `StructMemberAlignment` | **8Bytes** | 必须与引擎结构布局一致 |
| `RuntimeTypeInfo` | false | |
| `BufferSecurityCheck` / `ControlFlowGuard` | false | 会干扰裸 hook |
| `CharacterSet` | `NotSet` | 非 Unicode,与引擎一致 |
| `GenerateManifest` | false | |
| `SYR_VER` | 2 | 声明 SyringeEx |
| `WindowsTargetPlatformVersion` | `10.0` | 通配"最新已安装 SDK",**不锁定具体版本** |

配置名沿用 `Debug` / `DevBuild` / `Release`,输出目录 `$(Configuration)\`。

### 2.4 本机环境现状

**当前无法编译**。实测结果:

- `/mnt/c/Program Files/Microsoft Visual Studio/2022/` **是空目录**——VS 壳装了,C++ 工作负载没装
- 找不到 `cl.exe`、`vcvarsall.bat`；`vswhere` 查不到带 VC 的产品
- `Windows Kits\10` 存在,但那只是 SDK,不含编译器
- 无 msys2 / mingw;WSL 侧只有 Linux g++（64 位 ELF,对 Win32 DLL 无用）

**补齐方式**:用现成的 VS Installer 加装组件比全新装 Build Tools 省事。上游 `.vsconfig` 给出了精确清单（已核实）,可直接在 VS Installer 里"导入配置":

```json
{
  "version": "1.0",
  "components": [
    "Microsoft.VisualStudio.Component.CoreEditor",
    "Microsoft.VisualStudio.Workload.CoreEditor",
    "Microsoft.VisualStudio.Component.VC.CoreIde",
    "Microsoft.VisualStudio.Component.Windows10SDK.20348",
    "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
    "Microsoft.VisualStudio.Component.VC.ATL"
  ]
}
```

---

## 3. 构建与 CI

### 3.1 GitHub Actions 可行

Phobos 自己就在 Actions 上编译,配置很短（已核实其 `nightly.yml` + composite action）:

```yaml
name: Build
on: [push, pull_request]

jobs:
  build:
    runs-on: windows-latest
    steps:
    - uses: actions/checkout@v7
      with:
        submodules: recursive        # ← YRpp 是 submodule,漏了就是一堆缺头文件
    - uses: microsoft/setup-msbuild@v1.1
      with:
        vs-version: '[16.0, )'
    - uses: ammaraskar/msvc-problem-matcher@master   # 让报错在 PR 里高亮
    - run: msbuild /m /p:Configuration=Release .
      shell: cmd
    - uses: actions/upload-artifact@v4
      with:
        name: ra2hook-${{github.sha}}
        path: |
          Release/ra2hook.dll
          Release/ra2hook.pdb
```

**风险已排除**:我上一轮担心 hosted runner 没有 XP 工具集会导致首次构建失败——实测上游 `PlatformToolset` 是 `v143`,且 `WindowsTargetPlatformVersion` 是通配的 `10.0`,windows-latest 自带即可满足。

### 3.2 但 CI 不能替代本地环境

CI 只给编译器,**给不了运行环境**——runner 上跑不了 YR。迭代循环会变成:

```
push → 等数分钟 → 下 artifact → 解压进 RA2 目录 → 手动启动 Syringe → 看日志
```

对一个需要反复试验 hook 行为的项目,这个循环慢得难受。

**建议**:本地装 C++ 工作负载做快速迭代,CI 只用于可复现的正式构建与 PR 校验。两者不冲突,同一个 `.sln` 两边都能用。

---

## 4. 核心设计:独立 INI 注入

### 4.1 时间窗

```
读 rulesmd.ini
  └─ Ares 处理 [#include] 链（深度优先,递归）
       ↓
  ★ 注入窗口:CCINIClass 内容完整,但尚未解析成 TypeClass ★
       ↓
  引擎读取类型数据 → TypeClass 实例
```

窗口的两条边界都是硬的:

- **太早**会被后续的 include 处理覆盖
- **太晚**（类型数据已解析）改 `CCINIClass` 完全无效,引擎已经把值拷进 TypeClass 了

### 4.2 注入点:`0x679A1B`

Phobos 的 hook 命名恰好把这个窗口标注了出来（已核实）:

| 地址 | Phobos 的命名 | 相对窗口的位置 |
|---|---|---|
| `0x679A15` | `RulesData_LoadBeforeTypeData` 入口 | Ares/Phobos 的处理点 |
| `0x679A1B` | 同一函数中完成参数取值后的后置点 | **窗口内** ← ra2hook 使用 |
| `0x679CAF` | `RulesData_LoadAfterTypeData` | 太晚 |
| `0x668F6A` | `Read_File_LoadTypes` / `InitializeAfterAllLoaded` | 太晚 |

`0x679A15` 的原始指令是 `push ebx; push esi; mov esi,[esp+0Ch]`，因此到达
`0x679A1B` 时 `ESI` 已经是 `CCINIClass*`。ra2hook 使用这个相邻的 5 字节点：

```cpp
DEFINE_HOOK(0x679A1B, RA2Hook_RulesInject_PostAresPhobos, 0x5)
{
    GET(CCINIClass*, pINI, ESI);           // 0x679A15 已完成参数取值
    ...
    return 0;                              // 0 = 不改控制流,继续原流程
}
```

补丁长度 `0x5`。这是与 Ares/Phobos 地址不重叠的后置点；当前只完成 IDA 静态确认，
仍需在目标 `gamemd.exe` 上实机验证。

### 4.3 不要依赖 Syringe 的链序

Phobos 在 `0x668F6A` 上挂了**两个**函数,证明同址串联可行——撞点不等于崩溃。但**顺序取决于 DLL 加载次序,不可控**。

这对我们有实际影响:如果我们在 Phobos 的 handler 之前注入,Phobos 读到的是我们注入后的值;之后注入则读到原值。对 Phobos 自有的配置键,这个差异是可观测的。

**结论**:rules 主注入不再依赖同址 handler 的链序，而是依赖 `0x679A15` 正常续行后到达
`0x679A1B`。对 Phobos/Ares 在 `0x679A15` 已经读取并缓存的自定义字段，后置写入仍然太晚，
这属于语义限制，不是地址冲突。

### 4.4 关键验证项:此点是否已包含 `[#include]` 的内容

**这是整个设计唯一的致命假设**,而且我没能从源码确认。

推理是:Ares 文档说 include 在"主文件加载完毕后"处理,属于 INI 读取阶段;而 `0x679A1B` 属于 rules 消费阶段,在其之后。该时序仍要用探针确认。

**验证方法（便宜、确定,应作为第一个跑通的功能）**:探针键。

1. 在一个由 Ares `[#include]` 引入的 ini 里放:
   ```ini
   [RA2HookProbe]
   FromAresInclude=1
   ```
2. 在 `0x679A1B` 的 handler 里读它并记日志:
   ```cpp
   char buf[8] = {};
   pINI->ReadString("RA2HookProbe", "FromAresInclude", "", buf, sizeof(buf));
   Log::Info("probe at 0x679A1B: FromAresInclude=[%s]", buf);
   ```
3. 读到 `1` → 假设成立。读到空 → 当前 exe/扩展组合需要继续在 IDA 查找更晚的 rules 消费前窗口。

`ReadString` 地址 `0x528A10`（已核实）。这个探针只用已核实的 API,零风险。

### 4.5 合并实现

有两条路:

**路线 A（当前实现）:原始读取 + 自己解析 + `WriteString` 逐键写入**

```cpp
// INIClass::WriteString @ 0x528660 —— 键已存在则覆盖,不存在则新增
pTarget->WriteString(section, key, value);
```

- ✅ 语义完全确定:"后写胜出",正好对应需求里的"在 include 之后加载"
- ✅ 只依赖已核实的单个 API
- ✅ 写入方式与引擎自身一致,下游读取者无法区分
- ✅ 原始字节通过 `CCFileClass` 获取，仍支持散装文件和已注册 MIX 内文件
- ✅ 不调用 `CCINIClass::ReadCCFile`，不会被 Ares 的原生 include hook 重复展开
- ✅ inject 文件内可用 ra2hook 私有 `[#include]`:当前文件先合并,再按 include 出现顺序深度优先合并引用文件；重复的 `+=文件.ini` 会保留；该段不写入目标对象,不参与 Ares/Phobos 原 include 链
- ❌ 不复刻 Ares 其他扩展语义（如 `$Inherits`）。需求是独立注入机制,不是复制 Ares INI 处理器

**路线 B:`ReadCCFile` 直接读进目标 `CCINIClass`**

```cpp
CCINIClass* ReadCCFile(FileClass* pCCFile, byte bUnk = 0, int iUnk = 0)  // 0x4741F0
```

- ✅ 免费获得引擎的解析规则与 MIX 回落
- ❌ **合并语义未知**。如果它内部先 `Reset()`（`0x526B00`）再读,会**清空整个 rules**——灾难性
- ❌ 两个未文档化的参数含义不明

**决策**:走路线 A。路线 B 作为后续优化,验证前**绝不**在真实 rules 上调用——要验就在一个临时 `CCINIClass` 上验。

### 4.6 无异常约束

`ExceptionHandling=false` + `HAS_EXCEPTIONS=0` 意味着**不能用 try/catch**。因此:

- 解析错误必须用返回值传递,不能 throw
- 容器分配失败等于直接终止（可接受）
- 需要防御的地方用显式检查,或 SEH（`__try`/`__except`）
- handler 内不能让任何异常泄漏到引擎栈上——现在这一条由编译器强制保证了

### 4.7 幂等

handler 必须自带一次性保护:

```cpp
static bool s_done = false;
if (s_done) return;
s_done = true;
```

理由:不能假设引擎只走一次该路径（存档读取、重开局都可能重入）,也不能假设 Syringe 不会重复调用。

### 4.8 代码骨架

```cpp
// src/Hooks.RulesInject.cpp
#include <CCINIClass.h>
#include <RulesClass.h>
#include "Config.h"
#include "Logger.h"

namespace RulesInject {
    static bool s_done = false;

    static int MergeFile(CCINIClass* pTarget, const char* path);   // 路线 A
    static void DumpSnapshot(CCINIClass* pINI, const char* path);

    static void Apply(CCINIClass* pINI) {
        if (s_done) return;
        s_done = true;

        auto const& cfg = Config::Get().injection;
        if (!cfg.enabled) return;                       // 运行时开关,见 §2.1

        Probe(pINI);                                    // §4.4 探针

        int total = 0;
        for (auto const& f : cfg.files)
            total += MergeFile(pINI, f.c_str());
        Log::Info("RulesInject: %d files, %d keys", (int)cfg.files.size(), total);

        if (!cfg.dumpMergedRules.empty())
            DumpSnapshot(pINI, cfg.dumpMergedRules.c_str());
    }
}

DEFINE_HOOK(0x679A1B, RA2Hook_RulesInject_PostAresPhobos, 0x5)
{
    GET(CCINIClass*, pINI, ESI);
    RulesInject::Apply(pINI);
    return 0;
}
```

`DEFINE_HOOK(addr, funcname, size)` 展开为 `declhook` + `EXPORT_FUNC`（已核实）——它把 hook 声明写进 DLL 导出表,Syringe 启动时读取。**这就是"不需要自己写 trampoline"的全部原因**:寄存器保存、现场恢复、被覆盖指令的重放全部由 Syringe 负责。

---

## 5. 配置文件

### 5.1 `hooks.json` v1 的职责已经变了

v1 的核心内容是**地址表**:`address: { "YR-1.001-EN-标准": null }` 加上按 exe 版本的映射、`coldness` 分级、`originalBytes` 校验。这些在 Syringe 方案下**全部不再需要**:

| v1 字段 | 现状 |
|---|---|
| `address` | 作废——地址是 `DEFINE_HOOK` 里的编译期常量 |
| `targetExecutables` + size/md5 | 作废——Syringe 负责宿主匹配,且 Ares 生态本就锁定单一 exe |
| `originalBytes` / `patchLength` | 作废——Syringe 负责字节校验与重放 |
| `coldness` / `aresPhobosConflict` | 降级为注释——撞点由 Syringe 串联,不再是崩溃 |
| `enabled` | **保留**,这是唯一仍有运行时意义的字段 |

⚠️ **`hooks.json` 仍是早期设计记录，程序不读取它。** 当前所有开关都在
游戏目录下的 `ra2hook.ini`，避免引入另一套配置解析器。

### 5.2 当前配置

```ini
[Log]
Level=3

[Inject]
Enabled=no
Files=
Mix=yes

[Runtime]
Enabled=no
AutoApply=yes
Directory=ra2hook\runtime
DebounceMs=500
```

`[Dump]` 的完整字段及注释直接见仓库根目录的 `ra2hook.ini`。运行时行为、文件格式和
UI 操作见 `RUNTIME_INI.md`。

加载器规则:

```
1. ra2hook.ini 不存在       → 使用安全默认值，dump/inject/runtime 全部关闭
2. 对应 section 不存在      → 该子系统使用默认值，不回退读取其他 section
3. Inject.Enabled == no     → 启动注入 handler 立即返回
4. Inject.Files 指定文件缺失 → 记录 WARN；自动目录模式按文件名顺序合并现有文件
5. Runtime.Enabled == no    → IPC 仍可报告状态，但拒绝所有游戏数据写入
6. runtime 文件语法/include 错误 → 保留上一代有效状态，不做部分应用
```

**任何失败都不得阻止游戏启动。** 装不上就不装,把原因写清。崩在启动阶段会让用户完全无法定位问题。

---

## 6. 待验证清单

按"阻塞程度"排序。前两项在写核心代码之前就该有答案。

| # | 待验证 | 方法 | 若为否 |
|---|---|---|---|
| 1 | `0x679A1B` 处 `[#include]` 内容是否已并入 | §4.4 探针键 | 继续找更晚的 rules 消费前窗口 |
| 2 | `WriteString` 在此阶段写入是否被引擎读到 | 注入一个改血量的键,游戏内验证 | 整个方案不成立,退回解析后直写 TypeClass |
| 3 | 我们的 DLL 是否必须 SyringeEx | 在旧版 Syringe 下试跑 | 需分发 SyringeEx（Phobos 已强制要求它） |
| 4 | 原始解析器对目标 INI 的语法覆盖是否足够 | 多文件/include/mix 实机测试 | 补充解析语法，不回到 Ares hook |
| 5 | `0x679A1B` 是否能在 Ares/Phobos 续行后到达 | 双 DLL 同时加载并看日志 | 重新在 IDA 查找后置点 |

---

## 7. 路线图

### 阶段 0:能编译（当前阻塞项）

- [x] 工程文件已就位:`ra2hook.props/.vcxproj/.sln`（改写自 yrpp-spawner,Win32 单平台,已导入 `YRpp.props` 以编译 `StaticInits.cpp`）
- [x] `.github/workflows/build.yml`（§3.1）
- [x] `src/Main.cpp`（DllMain）+ `src/Hooks.RulesInject.cpp`（一个 `DEFINE_HOOK` + 探针日志）
- [ ] **你**:建仓 → `git submodule add YRpp` → push
- [ ] 看首次 CI 是否绿;红则按报错调工程文件

**关于 SyringeEx 握手**:上游 yrpp-spawner 实测**没有** `SYRINGE_HANDSHAKE` 导出、也没有 host 声明,仅靠 `DEFINE_HOOK` 写入 `.syhks00` 段即可被 Syringe 加载。故本项目**不需要**手写握手——这纠正了本文早期版本的说法。握手只用于版本门禁（如 Phobos 拒绝旧 Syringe）,基本注入用不上。

**出口条件**:CI 出 `ra2hook.dll`,拷进 RA2 目录后 Syringe 能加载、生成含探针行的 `ra2hook.log`。

这一步验证的是编译设置、`.syhks00` 段生成、Syringe 识别、DLL 加载这整条链路。**必须独立通过再往下走**,否则后面每个 bug 都要在"逻辑错了"和"链路没通"之间二分查找。

### 阶段 1:探针

- [x] Logger（文件输出,带级别）— `src/Logger.h`
- [x] Config 加载 — `src/Config.cpp`（ra2hook.ini,见 §5）
- [x] 在 `0x679A15` 挂探针 — 已实测:该点 pINI==INI_Rules,写入 `[E1]Strength=543` 被类型解析采纳（见 `src/Hooks.RulesInject.cpp` 头部注释）
- [x] IDA 确认并切换到后置候选点 `0x679A1B`（源码已切换，实机待验证）

**出口条件**:已达成。此点 CCINIClass 含全部 include 内容,手工 WriteString 能在游戏内生效。

### 阶段 2:核心注入（最小可用版本）

- [x] 注入文件读取 — `CCFileClass` 取原始字节，自有解析器写入临时 `CCINIClass`，不经过 Ares 的 `ReadCCFile` hook
- [x] `MergeFile`:逐键 `WriteString`,后写胜出；显式 `Files=` 和各目标目录均支持多个文件
- [x] inject 私有 `[#include]` 展开 — 不写入/干扰 Ares/Phobos 原 include 链;路径优先按当前文件目录解析,找不到再走 RA2/引擎文件系统（可引用已注册 mix 内 INI）
- [x] 合并后 rules 快照 dump — dump 点在 inject 之后,`rulesmd.ini` dump 即包含注入结果（两者共用 INI_Rules 同一对象)
- [ ] 与 Ares / Phobos 同时加载的共存测试（依赖本机/游戏环境,尚未跑）

**出口条件**:外部 ini 的改动在游戏内生效,Ares/Phobos 功能不受影响,且与它们的 `[#include]` 互不干扰。当前实现已就绪,待实测。

### 阶段 3:运行时功能

- [x] IDA 确认 `MainLoop(0x55D360)` 的外层回边在调用方；正常帧 tick 使用
      `0x55DE3A`，避开当前公开 Phobos 的主循环 hook 地址。
- [x] 文件 watcher、命令队列、游戏线程 staging/transaction/rollback。
- [x] Campaign/Skirmish 硬门禁；LAN/Internet/录像回放拒绝并在离局时回滚。
- [x] `RulesClass::Read_*` 与 `AbstractTypeClass::LoadFromINI` 路由，类型基线由
      `SaveToINI` 在本局首次写前捕获。
- [x] 外部 WPF 控制面板（`ui/`）与命名管道协议。
- [ ] Action C++ 编译与真实游戏验证。

完整实现边界、配置和测试顺序见 `RUNTIME_INI.md`。

### 阶段 4:可选

- [ ] SHP 内存 dump → `.shp`
- [ ] 开火点观察 hook（**默认不做**,见 §8）

---

## 8. 属性的两种类型

这个区分决定了阶段 3 需要挂几个点:

| 类型 | 行为 | 处理方式 |
|---|---|---|
| **现读型** | 每次使用时才从 TypeClass 读 | 改 TypeClass 字段即时生效,**无需 hook** |
| **拷贝型** | 单位创建时拷进实例 | 已存在的单位改不动,只能在新单位创建时补 |

武器数值多为现读型——这也是 `hooks.json` 自己指出"未必需要 hook 开火点"的原因。用一个高冲突点去做一件本不需要 hook 的事,收益与风险不成比例,因此开火观察点默认不做。

最大血量（`Strength`）是典型的拷贝型:改了 TypeClass,场上老单位的血量上限不变,只有之后新造的才生效。这就是单位创建 hook 存在的唯一理由。

---

## 9. 已核实的事实与来源

**编译配置** — `Phobos.props`、`Phobos.sln`、`.vsconfig`（[Phobos](https://github.com/Phobos-developers/Phobos) develop 分支实际抓取）:v143 / Win32 单平台 / stdcpp20 / 无异常 / 静态 CRT / StdCall / 8 字节对齐 / `SYR_VER=2`。

**CI 配置** — `.github/workflows/nightly.yml` 与其复用的 composite action:`windows-latest` + `submodules: recursive` + `setup-msbuild@v1.1` + `msbuild /m` + `upload-artifact@v4`。上游还会把 `gamemd.edb` 和 SyringeEx 打进 artifact。**Phobos 要求 SyringeEx,在旧版 Syringe 下拒绝运行。**

**YRpp API 地址** — [YRpp](https://github.com/Phobos-developers/YRpp) `INIClass.h` / `CCINIClass.h` / `RulesClass.h`:

| 符号 | 地址 |
|---|---|
| `INIClass::WriteString` | `0x528660` |
| `INIClass::ReadString` | `0x528A10` |
| `INIClass::GetSection` | `0x526810` |
| `INIClass::Exists` | `0x679F40` |
| `INIClass::Reset` | `0x526B00` |
| `CCINIClass::ReadCCFile` | `0x4741F0` |
| `CCINIClass::WriteCCFile` | `0x474430` |
| `RulesClass::Init` | `0x6686C0` |
| `RulesClass::Read_File` | `0x668BF0` |

**注入点约定** — Phobos `src/Ext/Rules/Body.cpp` 在 `0x679A15` 使用 `ECX = RulesClass*`、
`[esp+4] = CCINIClass*`。ra2hook 不再占用这个 6 字节点，而是使用其后的 `0x679A1B`、
`ESI = CCINIClass*`、补丁长度 `0x5`；`return 0` 表示继续原流程。

**Phobos 已占用地址**（避让参考）:`0x667A1D`、`0x667A30`、`0x668BF0`、`0x668F6A`（两个 handler）、`0x674730`、`0x6744E4`、`0x675205`、`0x675210`、`0x678841`、`0x679A15`、`0x679CAF`、`0x7115AE`。

**Ares `[#include]` 语义** — [Ares 官方文档](https://ares-developers.github.io/Ares-docs/new/misc/include.html)。

**未能核实**:Phobos 的 INI 相关特性文档（readthedocs 特性页多个 URL 均 404,只拿到首页）。

---

## 10. 风险

| 风险 | 影响 | 缓解 |
|---|---|---|
| **`0x679A1B` 早于 include 处理** | 核心需求落空 | §4.4 探针;按当前 exe 继续找 rules 消费前窗口 |
| 本机无编译环境 | 无法迭代 | 阶段 0 加装 C++ 工作负载;CI 作为兜底 |
| 仅靠 CI 迭代 | 循环慢到难以调试 | 本地环境优先,CI 只做正式构建 |
| 私有解析器与特殊 INI 语法差异 | 个别非标准语法未生效 | 先使用普通 section/key=value；发现实际语法缺口后补解析器 |
| Syringe 链序不可控 | 与 Phobos 的可见性差异 | 不依赖链序,只依赖控制流位置（§4.3） |
| 无异常可用 | 错误处理受限 | 返回值传错误 + 显式检查,必要时 SEH |
| 联机不同步 | 改值必然 desync | 定位为单机/调试工具,不做联机承诺 |
| 每帧 tick 性能 | 掉帧 | handler 开头早退;耗时操作分帧 |
| SyringeEx 依赖 | 旧 Syringe 下可能不加载 | 待验证清单第 3 项;必要时随包分发 |

---

## 11. 术语

| 术语 | 说明 |
|---|---|
| **gamemd.exe** | 尤里的复仇主程序,注入宿主 |
| **Syringe / SyringeEx** | 注入器,读 DLL 导出表决定挂哪些 hook,同址多 hook 时负责串联 |
| **YRpp** | 引擎类型定义 + 命名地址库,提供 `DEFINE_HOOK` |
| **Ares / Phobos** | 社区主流引擎扩展,共存对象 |
| **`[#include]`** | Ares 原生的 INI 包含机制,本项目要独立于它 |
| **CCINIClass** | 引擎的 INI 容器类,注入目标 |
| **TypeClass** | 单位/武器等的类型定义对象（`WeaponTypeClass` 等） |
| **现读型 / 拷贝型属性** | 见 §8 |
| **探针键** | 用于实测某 hook 点能否看到 include 内容的一个特殊 ini 键,见 §4.4 |
| **desync** | 联机不同步 |
