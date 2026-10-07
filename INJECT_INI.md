# ra2hook 启动 INI 有序补丁

当前实现：统一 `inject/<target>` 目录，同文件混写普通赋值、`+=`、`-=`，
按文件/行顺序交错执行，include 在出现处展开。**本轮只做静态核验，不本地编译、
安装环境或生成编译缓存；新增测试、Win32 DLL 构建交给 GitHub Actions，实机待验证。**
旧 set/remove 的测试结果与 2026-08-13 的覆盖注入实机结果不能代替本版本验证。

## 1. 目录与开关

```text
<game>/ra2hook/
├── ra2hook.ini
└── inject/
    ├── rules/   -> INI_Rules（允许赋值、追加、删除显式属性）
    ├── art/     -> INI_Art（赋值、追加）
    ├── ra2md/   -> INI_RA2MD（赋值、追加）
    ├── ai/      -> INI_AI（赋值、追加）
    ├── uimd/    -> INI_UIMD（赋值、追加）
    ├── sound/   -> SOUNDMD 局部对象（赋值、追加）
    └── mix/     -> 注册其中的全部 .mix
```

目标仍由目录决定，不把同一单位的 rules/art 混进同一文件。不存在 `Files=` 全局列表。
每个目标只扫描本层 `*.ini`，不递归扫描子目录；按文件名不区分大小写排序。
目录不存在或为空是合法无操作。建议文件名前加 `10-`、`20-` 等前缀。
Actions 成品自动生成六个目标目录和 mix，每个只含零字节 `.gitkeep`，不附带生效规则。

成品配置默认全部关闭：Dump、Inject、Runtime、导出子项、Mix、AutoApply 和日志。
以下是需要使用时手动开启的示例，不是成品默认值：

```ini
[Inject]
Enabled=yes
Mix=yes

[Log]
Level=3
```

`Enabled` 名称不变，控制全部启动补丁；`Mix` 控制 MIX 注册。
**修改文件或开关后必须完整重启游戏**，此功能不是 Runtime 热重载。

## 2. 同一文件中的三类操作

例如 `inject/rules/10-htnk.ini`：

```ini
[HTNK]
Strength=800
Cost=1200
-=Prerequisite
Prerequisite=GAWEAP
-=FactoryOwners
-=ForbiddenHouses

[VehicleTypes]
+=MYTANK

[MYTANK]
Strength=900
Cost=1500
```

最终 HTNK 的 `Prerequisite=GAWEAP`，因为它出现在删除之后；FactoryOwners 和
ForbiddenHouses 的显式键则缺失。新类型仍须加入正确的注册表。

| 写法 | 解释 |
|---|---|
| `Key=Value` | 不存在则新增，已存在则覆盖；空值合法，不等于删除 |
| `+=Value` | 追加独立项，为其生成未占用的 `RA2Hook_N` 键；常用于类型列表 |
| `-=Key` | 精确删除当前段中的显式键；只在 rules 目标可用 |
| `[#include]` 中 `+=path` 或 `name=path` | 在此行引用另一份补丁 |

只有等号左侧去除边缘空白后**恰好为 `+` 或 `-`** 才是特殊操作。
`Foo+=bar` 不表示向 Foo 的值追加内容；它是普通键 `Foo+` 的赋值，不建议这样误用。
`+=` 不是算术运算，也不表示给逗号列表追加元素。

重复赋值、重复 `+=` 和重复 `-=` 都保留为独立指令，不经过会折叠同名键的 INI 字典。
`+`/`-` 不作为普通属性写入游戏。生成的 `RA2Hook_N` 与 Ares/Phobos 的 `var_N`
命名空间隔离；如果你显式操作生成键，仍按指令顺序生效。

普通赋值延续既有规则：修剪值的边缘空白，保留内部文本、引号、额外 `=` 和行尾文本；
不由补丁层解释字符串含义。建议普通属性的说明放在独立注释行。
追加项、删除键名及未加引号的 include 路径会去除行尾注释。

## 3. 顺序：后出现者优先

顺序由三部分组成：

1. 根文件按文件名排序。
2. 每个文件自上而下逐行处理，普通赋值、追加、删除交错执行。
3. 遇到 include 行时深度优先展开子文件，完成后继续父文件后面的行。

例如入口 `inject/rules/index.ini`：

```ini
[HTNK]
Strength=500

[#include]
+=../fragments/htnk.ini

[HTNK]
Strength=900
```

`inject/fragments/htnk.ini`：

```ini
[HTNK]
-=Strength
Strength=700
```

执行顺序是 `500 -> 删除 -> 700 -> 900`，最终为 900。
**不再是“父文件全部正文先执行，最后展开 include”，也不再有全局删除优先阶段。**
子文件的段状态独立，不继承父文件当前段，也不改变父文件的段状态。
从 `[#include]` 返回后如需继续写单位属性，应重新写 `[单位ID]` 段头。

include 路径先相对当前文件查找，再尝试游戏 EXE 目录和引擎/MIX 文件系统。
带引号路径允许包含空格或注释字符：

```ini
[#include]
+=../fragments/vehicles.ini
1=unique_mix_rules.ini
extra="../fragments/name ; with # characters.ini"
```

`[#include]` 中 `-=path` 非法。MIX 需先通过既有注册流程加载。
片段应放在目标入口目录外，或只作为独立根文件，避免既扫描又 include。
**不去重加载**：同一文件被引用两次就执行两次，`+=` 因而会追加两项。

## 4. 删除边界与预校验

删除只影响 `INI_Rules` 当前明确存储的 entry：

- 段名和键名不区分大小写、精确匹配；缺失项跳过，不创建段。
- 不处理默认值、父段、已缓存 TypeClass 或扩展内部字段。
- 不写 no/空字符串，不恢复旧覆盖层值；后续显式赋值可重新建立该键。
- 不支持通配符、逗号清单、整段删除或单位注销。
- 注册表/列表段允许赋值和追加，但禁止删除成员；例如 `[VehicleTypes]` 中
  `+=MYTANK` 合法，`-=0` 非法。
- art/ra2md/ai/uimd/sound 出现 `-=` 会拒绝该目标计划，不会静默忽略。

**每个目标必须先读取、解析全部根文件及 include，成功后才执行第一条变更。**
缺失文件、语法错误、循环、深度/资源超限时，该目标本次的赋值/追加/删除全部不执行；
其他目标独立处理。这不同于旧版“保留 set，只跳过 remove”的错误策略。
已经开始执行后若原生 WriteString/Clear 失败，停止剩余指令并记录已执行计数，
**不承诺回滚**；预校验成功也不等于完整事务。

限制：路径短于 260 字节，段/键名短于 512 字节；根文件最多 256、读取文件最多 4096、
递归深度最多 32、单文件最多 8 MiB、总输入最多 64 MiB、全部操作合计最多 65536 条。
每文件 include 项上限 4096，实际原地加载通常先触及总文件数上限。超限不截断执行。
支持 UTF-8（可带 BOM）及 ANSI/GBK，支持 ASCII/全角空白和注释；拒绝 UTF-16/NUL。

原生 Clear 的单键分支、禁止列表和日志说明见 [REMOVE_INI.md](./REMOVE_INI.md)。

## 5. 从旧 enabled/set/remove 迁移

旧目录不会自动加载，发现时日志提示 `legacy directory ... is NOT loaded`。

1. 将旧 `inject/set/<target>`（更旧版本为 enabled）中的内容移到 `inject/<target>`。
2. 把旧删除清单中的 `-=` 行合并到相应单位文件，放到希望执行的位置。
3. 复核移动后所有相对 include 路径。
4. 复核 include 前后和跨文件顺序，尤其是“先删后写”的情况。

如暂时要保留旧版“全部写完再删”的最终效果，可保留普通写入根文件，并增加一个
**实际排序最后**的根文件（例如 `zz-remove.ini`）来 include 已迁移的删除片段。
片段放在目标扫描目录之外，避免重复入口。文件名不是保留字，需要自己确认排序。
旧版 include 是正文先于子文件；单纯移动目录不保证其原有覆盖顺序不变，必要时重排
include 或将最终覆盖行移到引用之后。

## 6. 引擎时机与隔离

| 目标 | Hook/对象 | 边界 |
|---|---|---|
| rules | `0x679A1B`, INI_Rules | 在原生类型定义读取前完成有序补丁 |
| art | `0x679A1B`, INI_Art | 后续原生读取可见；Phobos 更早缓存的字段除外 |
| ra2md | `0x679A1B`, INI_RA2MD | 不覆盖更早读取的扩展启动配置 |
| ai | `0x52D37D`, INI_AI | 装载后、AI 定义读取前 |
| uimd | `0x53531A`, INI_UIMD | 扩展可能另外使用局部对象 |
| sound | `0x52C6C4` 预解析计划，`0x7510F6` 内存应用 | 后一挂点不再读取补丁文件或展开 include |

主 Hook 按 rules/ra2md/art 依次处理目标，再 `RegisterInjectedTypes`、
`ReloadInjectedGlobalRules`，继续原生读取。最终规则状态用于类型注册及全局变化判断。

私有补丁使用 `CCFileClass` 读原始字节，由 `IniPatch` 规划、`StartupPatch` 执行，
不调用可能已被 Ares Hook 的 `CCINIClass::ReadCCFile`，不改原游戏 `[#include]`。
`$Inherits` 等扩展语义仍不由本项目模拟，是否生效取决于扩展后续是否消费该字段。

类型补注册包含 Countries、OverlayTypes、SuperWeaponTypes、Warheads、SmudgeTypes、
TerrainTypes、BuildingTypes、VehicleTypes、AircraftTypes、InfantryTypes、Animations、
VoxelAnims、Particles、ParticleSystems；WeaponTypes/Projectiles 使用 FindOrAllocate，
兼容单数 `[Projectile]`。已缓存全局段 Maximums、JumpjetControls、
MultiplayerDialogSettings、AI、Powerups、LandCharacteristics、IQ、General 按依赖重读。
不强制重放 Sides、Colors、ColorAdd 等结构段。

Runtime 仍使用 `IniOverlay` 和自己的基线恢复机制，不消费 `-=`；其原有正文先于
include、容错及热重载行为不因这次启动补丁调整而改变。见 [RUNTIME_INI.md](./RUNTIME_INI.md)。

## 7. 验证与历史记录

目标 gamemd.exe：

```text
MD5: 56D582A1D6F3C144D3ADC867D7A4D91B
SHA-256: 7CD005D263FDE203D9C84548200A057A8DF61D724DA3C6BD1E521EEB61CD0747
```

2026-08-13 的旧覆盖版本实机记录（不是本次有序流程的验证）：

- DLL SHA-256：FADCA627005A61F6C4467352C210EC30530F60085A363C74926A87E5E32E737A。
- rules：40 个 include、11320 个键、382 个追加项，段数 8779 -> 9092。
- art：26 个 include、1257 个键，段数 6534 -> 6536。
- 补注册 7 个规则列表；新增 WeaponTypes 82、Projectiles/Projectile 777。
- 全局重读 IQ、General，失败 0；类型诊断候选 1097、跟踪 1024、找到 1024、缺失 0，
  未跟踪 73 为诊断容量限制，不表示注册失败；用户确认目标新增内容能在游戏内生效。
- 当时缺失的 SUYASPATS.ini/SUYASPATS-art.ini、Mindmaster.ini 段外文本被容错跳过；
  **当前严格预校验会拒绝相应目标，必须先修复这些输入**。

新版本自动测试见 [tests/README.md](./tests/README.md)，本轮未本地运行。
提交后先检查 Actions 的测试与 DLL 构建，再完整重启游戏验证：

1. 混合配置中先删后设得到后值，先设后删后 Dump 不含该键。
2. include 子文件与父文件尾部按预期覆盖；多根文件排序稳定。
3. 最后一个 include 故意出错时，该目标一个键也不改变。
4. 新单位能生产，新增武器/弹体能真实开火，原有 Ares/Phobos 功能正常。
5. art/ai/uimd/sound 有内容时分别专项验证，不以 rules 成功替代其他目标验收。

正常日志包含 `patch: prepared`、`patch: applied` 及已有注册/全局重读日志。
`patch: rejected entire target ... file:line` 表示预校验失败；`native instruction failed`
表示执行阶段失败。Dump 只证明显式 INI 状态，不证明默认值或玩法条件已解除。

底层地址和被否决的 Hook 方案见 [INJECT_HOOK_ANALYSIS.md](./INJECT_HOOK_ANALYSIS.md)。
