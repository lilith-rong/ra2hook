# ra2hook

ra2hook 是面向《红色警戒 2：尤里的复仇》`gamemd.exe` 的 32 位 Syringe 扩展。
它可以与 Ares、Phobos 共存，在游戏启动阶段追加独立 INI 覆盖层，并为单机游戏提供
受约束的运行时 INI 热重载和可视化控制界面。

项目的核心不是替代 Ares/Phobos，而是在它们完成原有 INI 处理后，再叠加 ra2hook
自己的配置，同时避免干扰它们的 `[#include]` 链。

## 当前状态

统一有序启动补丁已实现：文件放在 `inject/<target>`，普通赋值、`+=`、`-=` 按行
交错执行，include 在出现处展开，后出现者优先。本轮仅静态核验，新增测试交由 Actions
运行，完整 Win32 构建和实机验证待执行。旧 set/remove 的 27 组测试与 ASan/UBSan
结果只属于历史版本；原生删键分支已由 IDA 核实，不等于新流程已通过游戏测试。

截至 2026-08-13：

- GitHub Actions 可以构建 `ra2hook.dll`、PDB 和自包含的运行时 UI；
- DLL 能够通过 Syringe 加载并与目标游戏版本共同运行；
- `rules`/`art` 私有 INI 注入已通过实机验证，新增类型和目标规则已在游戏内生效；
- 历史启动覆盖支持多个 INI、私有 include 和 MIX 内 INI；当时缺失 include 可跳过，
  **当前版本改为整目标严格预校验，缺失文件会拒绝该目标补丁**；
- 单机运行时系统、目录监视、事务式应用、回滚、命名管道和 WPF UI 已完成；
- UI 支持新建、编辑、保存、重命名、启用/停用和删除运行时补丁；
- INI、CSF、VXL、HVA、SHP 导出功能已实现。

`ai`、`uimd`、`sound` 和 MIX 资源加载已经接入，但具体游戏内容仍应分别做专项测试。
扩展在 Hook 之前已经缓存的字段，也不能依靠后置注入强制覆盖。

## 支持环境

当前 Hook 地址只针对以下 `gamemd.exe`：

```text
MD5:    56D582A1D6F3C144D3ADC867D7A4D91B
SHA256: 7CD005D263FDE203D9C84548200A057A8DF61D724DA3C6BD1E521EEB61CD0747
```

已验证环境包含 Ares 3.0p1 与 Phobos Build #47+6_0。更换 EXE、保护壳或大幅修改
游戏主程序后，必须重新核对 Hook 地址、覆盖长度、寄存器约定和控制流。

## 主要功能

### 启动阶段 INI 注入

注入目标由目录决定，不需要在 `ra2hook.ini` 中维护文件列表：

| 目录 | 目标对象 | 状态 |
|---|---|---|
| `inject/rules` | `rulesmd.ini` / `INI_Rules` | 写入、追加、显式删键；新流程待实机验证 |
| `inject/art` | `artmd.ini` / `INI_Art` | 写入、追加；有早期缓存限制 |
| `inject/ra2md` | `ra2md.ini` / `INI_RA2MD` | 写入、追加；有早期缓存限制 |
| `inject/ai` | `aimd.ini` / `INI_AI` | 写入、追加；需专项测试 |
| `inject/uimd` | `uimd.ini` / `INI_UIMD` | 写入、追加；扩展局部对象有限制 |
| `inject/sound` | `soundmd.ini` 局部对象 | 两阶段计划/执行；需专项测试 |

旧 `enabled/set/remove` 目录**不会自动加载**，发现时记录迁移警告。将旧 set 的目标
子目录移到 inject 下，把删除指令合并进对应文件，并复核 include 路径和执行顺序。
`[Inject] Enabled` 名称不变，`Mix` 及 `inject/mix` 不变。

每个目标按文件名不区分大小写排序读取 `*.ini`，文件内按行执行，后出现者优先。
入口 INI 可以使用 ra2hook 自己的 `[#include]`，在该行展开子文件后继续父文件：

```ini
[#include]
+=units.ini
+=weapons.ini
+=balance.ini
```

该 include 机制不调用可能已被 Ares Hook 的 `CCINIClass::ReadCCFile`，不会改写或
重复展开 Ares/Phobos 的 include。`+=` 项使用 ra2hook 独立键名保存，不与它们的
`var_N` 命名空间冲突。

对 `rules` 新增的单位、建筑、武器、弹体等类型，ra2hook 会补跑原生注册逻辑，
随后让游戏原流程读取类型定义，而不是只把文本写进 `CCINIClass`。

### 启动阶段显式键删除

主 Hook 在 rules/ra2md/art 各目标完成有序补丁后，再调用 `RegisterInjectedTypes`、
`ReloadInjectedGlobalRules` 并继续原生类型读取。不再有最终统一 remove 阶段。
仅 `inject/rules` 允许 `-=`，同一单位可直接混写：

```ini
[UNIT_ID]
Strength=800
-=Prerequisite
Prerequisite=GAWEAP
-=FactoryOwners
-=ForbiddenHouses
```

最终 `Prerequisite=GAWEAP`，另外两个属性缺失。`+=`、`-=` 是独立指令，不能作为
普通同名 INI 键压缩；普通 `属性=值` 保留既有赋值语义。

每个目标的所有根文件和 include 必须完整读取、校验成功，才开始任何写入/追加/删除。
预校验失败该目标零修改，其他目标独立处理；原生执行失败会停止，但不承诺回滚。
删除精确匹配、不存在跳过，禁止通配符、整段删除和注册表删除；注册表仍可赋值/追加。

remove 不写 `no`/空值，不恢复旧覆盖层值，也不修改默认值或已缓存的 TypeClass 字段。
Dump 中键缺失只证明显式删除，不保证解除生产限制。编辑后必须完整重启游戏；
严格语法、include 和错误规则见 [REMOVE_INI.md](./REMOVE_INI.md)。

### 单机运行时补丁

运行时功能只允许战役和遭遇战。局域网、互联网和录像/回放会被硬阻止，离开单机
对局时会自动回滚，避免不同玩家数据不一致导致崩溃。

运行时管线：

```text
ra2hook/runtime/*.ini 或 UI 命令
  -> 文件监视/命名管道线程
  -> 有界命令队列
  -> 游戏主线程 RuntimeTick
  -> 完整目标状态重建与校验
  -> 原生 RulesClass/TypeClass 读取路径
```

运行时不会在工作线程直接操作游戏对象。应用失败会保留上一代有效状态，删除文件或
键会按本局首次修改前捕获的基线回滚。

UI 中补丁状态对应文件后缀：

```text
name.ini           已启用，会被 DLL 合并
name.ini.disabled  已停用，仍可编辑但不会被 DLL 合并
```

### Dump

Dump 可以导出：

- 引擎内存中已经合并完成的 rules/art/ai/uimd/ra2md INI；
- CSF 字符串表；
- VXL、HVA 和 SHP 资源；
- 按单位归类或按 `OnlyUnits` 精确筛选的资源。

### 单位提取

游戏结束后可以直接在 `ra2hook-ui.exe` 的“单位提取”页面处理已有 Dump，不需要重新
启动游戏。页面从 `dump/ini/rulesmd.ini` 和 `dump/ini/artmd.ini` 读取四类注册表：
`BuildingTypes`、`InfantryTypes`、`VehicleTypes`、`AircraftTypes`。

选择注册名称后，提取器会递归收集该单位及其规则依赖，包括武器、抛射体、弹头、粒子
系统、粒子、动画、残骸和生成/部署单位等已存在的段，并把对应注册表项写入 `rules.ini`。
单位主 Art 段、重定向 Art 段及关联动画段写入 `art.ini`。原始 Dump 不会被修改。

`rules.ini` 和 `art.ini` 会直接写入该单位已有的模型目录，不另外创建单位包目录：

```text
<game>\ra2hook\dump\vxl\<注册名称>\   # Voxel 单位
<game>\ra2hook\dump\shp\<注册名称>\   # SHP 单位
|-- rules.ini
|-- art.ini
`-- 原有 VXL/HVA/SHP/PCX 等素材
```

Voxel 单位优先选择 `vxl/<ID>`，SHP 单位优先选择 `shp/<ID>`；首选目录不存在时会使用
另一种已存在的素材目录。两种目录都不存在时，会根据 Art 类型自动创建对应的模型目录。

`rules.ini` 中的类型注册项使用 `单位名称_类别_序号=实际ID`，例如：

```ini
[VehicleTypes]
HTNK_Vehicle_1=HTNK
```

武器、抛射体、弹头、粒子系统等依赖注册也使用同一命名规则。

导出的 INI 默认移除历史 `[#include]` 段，避免将快照重新交给 Ares 时再次展开。

## 安装目录

将 Action artifact 解压到游戏目录，并保持以下结构：

```text
<game>/
|-- gamemd.exe
|-- ra2hook.dll
|-- ra2hook.pdb                 # 可选，仅调试需要
`-- ra2hook/
    |-- ra2hook.ini
    |-- ra2hook-ui.exe
    |-- ra2hook.log             # 首次运行后生成
    |-- inject/
    |   |-- rules/
    |   |-- art/
    |   |-- ra2md/
    |   |-- ai/
    |   |-- uimd/
    |   |-- sound/
    |   `-- mix/
    |-- runtime/
    `-- dump/
```

DLL、配置、日志和运行时目录都根据实际运行的游戏 EXE 路径定位，不依赖进程名或
启动器设置的当前工作目录。配置优先读取 `<game>\ra2hook\ra2hook.ini`；仅在新
路径不存在时兼容读取 `<game>\ra2hook.ini`。日志固定写入
`<game>\ra2hook\ra2hook.log`。

## 快速开始

Action 成品的 `ra2hook.ini` 默认关闭 Dump、Inject、Runtime、全部导出子项、MIX、
自动应用及日志。使用前手动开启需要的总开关和子项；仅安装成品不会自动启用这些功能。
Actions 会检查打包后的配置，意外启用任一默认开关时拒绝打包。

### 1. 启用启动注入

编辑 `<game>\ra2hook\ra2hook.ini`：

```ini
[Inject]
Enabled=yes
Mix=yes

[Log]
Level=3
```

将入口文件放入对应目录，例如：

```text
<game>\ra2hook\inject\rules\index.ini
```

`[Inject] Enabled` 不改名，控制启动补丁。启动层不热重载，修改文件或开关后须完整
重启游戏。旧目录迁移及顺序变化见 [INJECT_INI.md](./INJECT_INI.md)。

然后通过现有 Syringe/Ares/Phobos 启动链运行游戏。Syringe 会读取 DLL 的
`.syhks00` 段并安装 Hook，不需要额外握手配置。

### 2. 启用运行时 UI

在同一配置中设置：

```ini
[Runtime]
Enabled=yes
AutoApply=yes
Directory=ra2hook\runtime
DebounceMs=500
```

`Enabled` 只在 DLL 初始化时读取，修改后必须完整重启游戏。进入战役或遭遇战后运行：

```text
<game>\ra2hook\ra2hook-ui.exe
```

UI 自动将自身目录的上一级识别为游戏目录，也可以手动选择。创建或编辑补丁后可直接
应用、回滚或开启自动应用。复选框用于单独启用/停用每个补丁。

### 3. 启用 Dump

```ini
[Dump]
Enabled=yes
INI=yes
CSF=yes
VXL=no
SHP=no
SortByOwner=yes
OnlyUnits=
StripInclude=yes
```

Dump 会增加启动时间和磁盘占用，不使用时保持默认关闭。成品中 INI/CSF/VXL/SHP
子项也为 no，开启 Dump 总开关后还需选择导出类型；INI 导出建议同时开启 StripInclude。

## 构建

项目以 GitHub Actions 为正式构建方式：

1. 仓库包含 YRpp submodule；
2. push 到 `main`、`master` 或 `develop`，也可手动运行 workflow；
3. Action 先运行 Win32 CTest，再使用 MSBuild/v143 构建 Win32 DLL；
4. 使用 .NET 8 发布自包含的 `win-x64` 单文件 UI；
5. 下载名为 `ra2hook-<commit>` 的 artifact。

Action 产物包含：

```text
ra2hook.dll
ra2hook.pdb
ra2hook/ra2hook-ui.exe
ra2hook/ra2hook.ini
ra2hook/INJECT_INI.md
ra2hook/REMOVE_INI.md
ra2hook/inject/rules/
ra2hook/inject/art/
ra2hook/inject/ra2md/
ra2hook/inject/ai/
ra2hook/inject/uimd/
ra2hook/inject/sound/
ra2hook/inject/mix/
```

Actions 在打包阶段直接生成上述目录，不依赖仓库中的空文件夹，也不会把仓库 `inject`
下的 INI/MIX 内容复制进成品。每个目录仅含一个零字节 `.gitkeep`，防止 artifact ZIP
忽略空目录；解压后可直接放入自己的文件，无需手动建目录。
不再生成 set/remove 操作目录；只有 rules 目标允许删除指令。
测试说明见 [tests/README.md](./tests/README.md)，本轮不本地编译或安装环境。
CI 验证编译、自动测试和打包，无法代替真实游戏、Ares、Phobos 与具体 MOD 内容测试。

## 常见问题

| 现象 | 检查项 |
|---|---|
| 完全没有日志 | DLL 是否与 `gamemd.exe` 同级；Syringe 是否加载 DLL |
| 找不到配置 | 使用 `<game>\ra2hook\ra2hook.ini`；查看日志中的 `Config: 已加载` |
| 没有 inject 日志 | `[Inject] Enabled=yes`；是否迁移到 `inject/<target>` |
| include 文件缺失 | 检查相对路径、MIX 和文件名；整目标预校验失败，零修改 |
| 删除后属性又出现 | 后续行/include/根文件可能重新赋值；以最后出现的操作为准 |
| 补丁未执行 | 查看 `patch: rejected entire target` 的文件名、行号和错误 |
| INI 已合并但新增单位不存在 | 检查类型列表、`missing` 日志、生产条件和科技条件 |
| UI 显示运行时未启用 | `[Runtime] Enabled=yes` 后完整重启游戏 |
| UI 显示未连接 | 游戏是否运行；DLL/UI 是否来自同一次 artifact；游戏目录是否选对 |
| UI 显示连接权限不足 | 更新同一 artifact 中的 DLL/UI；新管道允许本机普通 UI 连接提权游戏 |
| 补丁显示停用 | 文件后缀为 `.ini.disabled`，勾选后恢复为 `.ini` |
| 运行时键被拒绝 | 查看安全等级；结构、资源和类型注册改动需要重启游戏 |
| Ares/Phobos 扩展字段未生效 | 该字段可能在 ra2hook Hook 之前已被扩展缓存 |

## 设计边界

- 运行时写入只支持单机，不支持多人或录像；
- 运行时不能安全创建新类型，也不能热替换图像、体素、运动方式、Foundation 等结构；
- Ares/Phobos 私有字段只有在对应扩展后续仍会读取时才可能生效；
- `ra2md` 后置注入不能覆盖 Phobos 已提前读取的启动配置；
- `uimd` 全局对象不能保证覆盖扩展重新打开的局部 UI 配置；
- MIX 同名资源的最终查找优先级由游戏文件系统决定，建议使用唯一资源名；
- PDB 不参与运行，只有调试崩溃和符号定位时需要。

## 文档索引

- [PROJECT_KEY_POINTS.md](./PROJECT_KEY_POINTS.md)：项目完成关键点、技术决策与维护清单
- [INJECT_INI.md](./INJECT_INI.md)：启动有序补丁语法、include 顺序和迁移
- [REMOVE_INI.md](./REMOVE_INI.md)：混合文件中的显式删键语义、预校验与边界
- [INJECT_HOOK_ANALYSIS.md](./INJECT_HOOK_ANALYSIS.md)：IDA、Hook 地址和冲突分析
- [RUNTIME_INI.md](./RUNTIME_INI.md)：运行时架构、安全分级与测试边界
- [DEVELOPMENT.md](./DEVELOPMENT.md)：完整开发过程和底层设计
- [TODO.md](./TODO.md)：非核心扩展项和专项验证记录

## 源码结构

```text
src/
|-- Hooks.RulesInject.cpp  启动阶段多目标注入与类型补注册
|-- IniOverlay.cpp         Runtime 既有 INI/include 解析及共享扫描/复制工具
|-- IniPatch.cpp           启动逐行有序指令计划、原地 include、整目标校验
|-- StartupPatch.cpp       启动文件读取、有序赋值/追加和原生精确删键
|-- Hooks.Runtime.cpp      游戏主线程 Tick Hook
|-- Runtime.cpp            安全分类、应用、基线和回滚
|-- RuntimeWatcher.cpp     文件系统监视
|-- RuntimeProtocol.cpp    命名管道协议
|-- Hooks.Dump.cpp         Dump 入口
|-- DumpIni.cpp            INI/CSF 导出
|-- ArtMap.cpp             VXL/HVA/SHP 映射与导出
|-- Config.cpp             配置读取
|-- GamePaths.h            以游戏 EXE 为基准的路径和 IPC 名称
`-- Logger.h               文件日志

ui/
|-- MainWindow.xaml        WPF 界面
|-- MainWindow.xaml.cs     补丁文件管理和运行时操作
|-- Services/PipeClient.cs IPC 客户端
`-- Services/IniDocument.cs 配置和补丁辅助逻辑
```

## 项目结论

ra2hook 已完成最初目标：在不接管 Ares/Phobos include 系统的前提下，为目标游戏提供
可组合的后置 INI 注入，并扩展出受安全约束的单机运行时修改、可视化控制和资源导出。
后续工作应以兼容性回归和特定目标专项测试为主，不再需要改变当前总体架构。
