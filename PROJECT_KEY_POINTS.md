# ra2hook 项目完成关键点

本文记录 ra2hook 从方案探索到可用版本所依赖的关键技术决策。它用于项目收尾、后续
维护和回归测试，不代替面向使用者的 `README.md`。

## 1. 最终目标

项目最终解决了三个相互关联的问题：

1. 在 Ares/Phobos 完成原有 INI include 后，继续叠加 ra2hook 的独立 INI；
2. 让新增 rules 类型真正进入游戏类型数组，而不只停留在 INI 内存对象；
3. 在单机游戏中安全地监视、应用和回滚一部分运行时 INI 修改。

对应的最终架构由启动注入、运行时系统、Dump、外部 UI 四部分组成。

## 2. Hook 选择是项目成立的基础

目标 EXE：

```text
MD5:    56D582A1D6F3C144D3ADC867D7A4D91B
SHA256: 7CD005D263FDE203D9C84548200A057A8DF61D724DA3C6BD1E521EEB61CD0747
```

最终 Hook 表：

| 地址 | 长度 | 用途 | 关键条件 |
|---|---:|---|---|
| `0x679A1B` | 5 | rules/ra2md/art 后置注入 | 位于 Ares/Phobos `0x679A15..1A` 之后、原生类型读取之前 |
| `0x668EF5` | 5 | 新增类型加载后诊断 | 只核验数组，不修改游戏状态 |
| `0x52D37D` | 7 | ai 注入 | `INI_AI` 装载完成、消费之前 |
| `0x53531A` | 5 | uimd 注入 | `INI_UIMD` 装载完成、消费之前 |
| `0x52C6C4` | 5 | sound 覆盖层预备 | 尚未打开 SOUNDMD，可安全做文件 I/O |
| `0x7510F6` | 5 | sound 覆盖层应用 | `ECX` 已指向 SOUNDMD 对象，首个配置消费之前 |
| `0x668F6A` | 5 | Runtime 初始化与 Dump | 所有主要数据装载完成 |
| `0x55DE3A` | 6 | 游戏主线程 RuntimeTick | 正常帧返回路径，每帧执行一次 |

`0x679A1B` 是最关键的选择。Ares 和 Phobos 共同占用 `0x679A15..0x679A1A`，直接
复用该地址会让多个项目竞争同一个注入口。将 ra2hook 放到下一条完整指令，既能看到
它们已经处理的结果，又仍处于原生类型读取之前。

Hook 地址与目标 EXE 强绑定。任何 EXE 变化都必须重新做 IDA 和字节区间检查，不能
只因游戏能够启动就认为所有寄存器和时序仍然正确。

## 3. 独立 include，而不是复用扩展 include

ra2hook 没有把自己的入口文件交给 `CCINIClass::ReadCCFile`。原因是 Ares 可能 Hook
这个函数并自动展开 `[#include]`，会导致重复处理、顺序不确定和项目间耦合。

最终方案：

- 用 `CCFileClass` 获取散装或 MIX 中的原始 INI 字节；
- 由 `IniOverlay` 解析普通段和私有 `[#include]`；
- 先合并当前文件正文，再按 include 出现顺序递归合并；
- 后写覆盖前写；
- 循环 include 和超过 32 层的递归被拒绝；
- 缺失 include 与可恢复文本问题只记警告，不中断兄弟文件；
- include 段本身不写入目标 INI；
- `+=` 内部使用 `__RA2HOOK_APPEND_`，输出时使用 `RA2Hook_N`，不与
  Ares/Phobos 的 `var_N` 冲突。

这使 inject 目录中的 include 成为 ra2hook 自己的配置入口，同时不改变原 MOD 的
Ares/Phobos include 链。

## 4. 多 INI 和多目标通过目录表达

项目早期曾考虑在 `ra2hook.ini` 中使用 `Files=`，但该配置不能表达一个文件究竟属于
rules、art 还是其他对象，最终被删除。

现在目标完全由目录决定：

```text
inject/enabled/rules/*.ini
inject/enabled/art/*.ini
inject/enabled/ra2md/*.ini
inject/enabled/ai/*.ini
inject/enabled/uimd/*.ini
inject/enabled/sound/*.ini
```

每个目录支持多个入口文件，按不区分大小写的文件名排序。这个约定简单、可观察，且
不需要为每一种目标继续扩展总配置格式。

## 5. 写入 INI 不等于新增类型生效

实机测试证明：把定义段写进 `INI_Rules`，并不会自动把新 ID 加入所有原生类型数组。
因此 rules 注入还需要补注册：

- 对发生变化的原生类型列表调用对应 `RulesClass::Read_*`；
- `WeaponTypes` 和 `Projectiles` 逐项调用 `FindOrAllocate`；
- 单数 `[Projectile]` 作为社区兼容别名注册为 BulletType；
- 注册完成后继续走游戏原生 `LoadTypesFromINI` 读取完整定义；
- 在 `0x668EF5` 对新增候选做加载后核验。

已验证实例中，rules 私有 include 合并 11320 个键，新增候选 1097 个；诊断缓冲区
跟踪的 1024 项全部找到，游戏内目标内容最终生效。这一过程证明了“解析、写入、注册、
读取、实际玩法”是五个不同层次，不能只看日志中出现某个段就认定成功。

## 6. 已经缓存的全局段必须选择性重读

部分 rules 全局段在 `0x679A1B` 之前已经被原程序消费。ra2hook 对发生变化的以下段
按依赖顺序主动重读：

```text
Maximums
JumpjetControls
MultiplayerDialogSettings
AI
Powerups
LandCharacteristics
IQ
General
```

并非所有段都可以重放。`Sides`、`Colors`、`ColorAdd` 等会改变全局结构的内容没有
被强制重读。扩展在更早阶段缓存的私有字段也仍然受时序限制。

## 7. Sound Hook 必须拆成两个阶段

Sound 是项目中最典型的失败驱动设计。以下候选点均被实机崩溃否决：

- `0x52C796`：覆盖相对 `call`，SyringeIH 重放不安全；
- `0x52C78F`：被盗指令依赖原始 ESP；
- `0x7510D0`：函数入口指令直接修改 ESP。

最终拆分为：

1. `0x52C6C4` 预读配置、注册 MIX、构建持久 sound 覆盖层；
2. `0x7510F6` 只验证对象并做内存复制，不执行文件 I/O。

这里的关键经验是：Hook 点不能只看“逻辑位置够晚”，还必须确认覆盖的是完整指令、
被盗指令可安全重放、ESP/寄存器约定稳定，并用空配置连续启动排除控制流问题。

## 8. 运行时写入必须回到游戏主线程

文件监视和命名管道运行在工作线程，但它们只向有界队列投递命令。所有
`CCINIClass`、`RulesClass` 和 TypeClass 操作都在 `0x55DE3A` 的 `RuntimeTick` 中执行。

这条线程边界避免了工作线程与游戏主线程同时访问引擎对象，是运行时功能能够稳定
存在的前提。

## 9. 运行时采用完整状态重建和事务语义

运行时不是在旧状态上无限叠加修改。每次加载都会：

1. 重新扫描全部启用的 `*.ini`；
2. 构建完整目标覆盖层；
3. 校验语法、include 和安全等级；
4. 从 rules 与本局类型基线构建 staging INI；
5. 调用原生读取路径应用；
6. 失败时恢复上一代有效状态；
7. 删除键或文件时恢复基线值。

类型在本局第一次修改前通过 `SaveToINI` 保存实际基线，退出单机对局时自动回滚并
清理本局状态。

## 10. 单机门禁是硬约束

运行时仅允许 Campaign 和 Skirmish，并拒绝：

- LAN；
- Internet；
- Replay/Record/Attract；
- 不在对局中的写入。

这是正确性约束，不是 UI 选项。多人客户端规则不同会造成不同步甚至崩溃，因此不应
提供绕过开关。

## 11. 运行时修改按风险分级

最终安全等级：

| 等级 | 含义 |
|---|---|
| `Immediate` | 原生类型实时读取的标量，可立即影响类型数据 |
| `FutureObjects` | 类型值已改变，但已有对象可能保留旧副本 |
| `ControlledReload` | 通过已知 `RulesClass::Read_*` 或原生 `LoadFromINI` 路径重读 |
| `RestartRequired` | 识别但拒绝运行时写入，要求重启 |

类型注册段、图像/体素、Foundation、Locomotor、MovementZone 等资源或结构字段不会
被强写。未知扩展字段也不承诺通用回滚。

## 12. UI 的启停状态直接映射到文件系统

UI 没有引入额外数据库：

```text
name.ini           启用
name.ini.disabled  停用
```

DLL 对 `.ini` 结尾做严格校验，因此停用文件不会因 Windows 通配符或 8.3 文件名行为
被意外加载。UI 同时支持重命名、同名冲突检查、原子保存和目录变更刷新。

## 13. 路径和 IPC 都绑定实际游戏目录

路径通过 `GetModuleFileName(nullptr, ...)` 从实际运行 EXE 解析，不依赖：

- 进程名称；
- 窗口标题；
- 启动器修改的当前工作目录。

最终约定：

```text
配置  <game>\ra2hook\ra2hook.ini
日志  <game>\ra2hook\ra2hook.log
补丁  <game>\ra2hook\runtime
UI    <game>\ra2hook\ra2hook-ui.exe
```

命名管道包含游戏目录哈希，避免多个 RA2 安装目录互相误连。管道拒绝远程客户端，
并允许本机已认证普通用户连接由管理员权限游戏创建的管道。DLL 与 UI 的管道算法是
同一版本协议，部署时必须来自同一次 artifact。

## 14. 失败必须降级，不能阻止游戏启动

项目中的配置、日志、目录、include 和单个目标加载都遵循失败降级原则：

- 配置缺失时所有高风险功能默认关闭；
- 日志无法创建时静默放弃日志，不让游戏崩溃；
- 单个 include 缺失时跳过并继续兄弟项；
- 目标对象尚未装载时跳过该目标；
- 运行时候选状态无效时保留上一代状态；
- 高风险运行时键标记为 `RestartRequired`，不尝试强写。

日志必须记录选中的配置路径、目标目录、文件数、键数、类型注册、重读结果、运行时
代数和失败原因，使实机问题能够从日志还原。

## 15. Dump 是验证工具，也是独立能力

Dump 使用引擎自己的文件类读取 MIX/加密资源，并从内存 INI 对象输出最终合并结果。
它在开发阶段用于确认：

- Ares/Phobos include 是否已进入目标对象；
- ra2hook 覆盖是否最终胜出；
- 新增定义是否出现在最终快照；
- 游戏实际引用了哪些 CSF/VXL/HVA/SHP 文件。

因此 Dump 不只是附加导出功能，也是 Hook 时序和注入结果的可观察性手段。

## 16. 构建和部署必须成套

正式构建由 GitHub Actions 完成：

- MSBuild/v143 构建 Win32 DLL；
- .NET 8 发布自包含 win-x64 UI；
- 配置模板放入 `ra2hook/ra2hook.ini`；
- DLL、PDB、UI 和配置作为同一 artifact 发布。

特别是运行时 IPC 发生协议或管道命名变化时，不能只替换 DLL 或只替换 UI。

## 17. 项目验收证据

项目收尾所依据的证据包括：

- GitHub Actions 完整构建通过；
- Syringe 能加载 DLL，游戏可以正常启动运行；
- rules/art 私有 include 在日志中完成展开和合并；
- rules 段数、类型数组和新增候选有加载后诊断；
- 已跟踪新增类型 `missing=0`；
- 用户在游戏内确认目标注入内容实际生效；
- 最新 UI 版本能够运行，并具备补丁编辑、启停和连接诊断；
- 配置与日志已统一收口到游戏目录下的 `ra2hook` 子目录。

## 18. 明确保留的边界

以下内容不影响项目按当前目标收尾，但后续扩展时必须保留记录：

- `ai`、`uimd`、`sound` 需要用真实内容分别专项回归；
- MIX 资源应验证唯一命名资源的游戏内加载；
- 扩展提前缓存的字段不能由后置 Hook 普遍解决；
- 运行时不支持创建新类型或热替换资源/结构；
- 不支持 `evamd.ini` 和 `thememd.ini` 目标；
- 不支持任何多人运行时写入；
- 更换 `gamemd.exe` 后必须重新逆向和验证全部 Hook。

## 19. 后续维护检查表

修改 Hook、解析器、类型注册或运行时逻辑后，至少检查：

1. Action 同时构建 DLL 和 UI；
2. 连续启动游戏，无启动崩溃；
3. Ares/Phobos 原 include 和主要功能正常；
4. 私有 include 数量和键数没有异常下降；
5. 新类型诊断没有新增 `missing`；
6. 实际生产新增单位并让新增武器/弹体完成一次开火；
7. UI 与 DLL 来自同一 artifact，连接到正确游戏目录；
8. 运行时测试应用、语法失败保留、删除回滚和离局回滚；
9. LAN/Internet/Replay 仍拒绝写入；
10. 日志、配置、补丁和 Dump 仍位于 `<game>\ra2hook`。

## 20. 收尾结论

ra2hook 的关键成果不是单个 Hook，而是一条完整且可验证的链路：

```text
正确时机
  -> 独立解析
  -> 目标对象合并
  -> 类型补注册/全局段重读
  -> 原生消费
  -> 加载后诊断
  -> 单机运行时事务和回滚
  -> UI 与日志可观察性
```

这条链路已经形成稳定架构。未来工作应围绕新目标的专项验证和兼容性维护展开，而不应
重新回到共享 include、工作线程写引擎对象或无回滚的运行时叠加方案。
