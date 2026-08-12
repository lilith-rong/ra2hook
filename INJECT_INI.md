# ra2hook INI 注入说明

最后更新：2026-08-13  
状态：rules/art 私有 INI 注入已通过实机验证，游戏内目标内容可以正常生效。

## 1. 功能目标

ra2hook 在 Ares/Phobos 完成原有 INI 处理后，再把自己的 INI 覆盖层合并到对应的
游戏 INI 对象。它解决以下问题：

- ra2hook、Ares 和 Phobos 不再竞争同一个注入地址；
- ra2hook 的 `[#include]` 独立展开，不修改或干扰 Ares/Phobos 原 include 链；
- 一个目标目录可以有多个入口 INI；
- include 可以引用散装文件或已注册 MIX 中的 INI；
- 缺失的 include 文件只产生警告，不中止其他兄弟文件；
- rules 中新增的单位、建筑、武器、弹体等类型会补注册到引擎数组，而不只是写进
  `CCINIClass` 内存对象。

## 2. 已验证环境

本次实机验证使用：

```text
gamemd.exe SHA-256:
7CD005D263FDE203D9C84548200A057A8DF61D724DA3C6BD1E521EEB61CD0747

ra2hook.dll SHA-256:
FADCA627005A61F6C4467352C210EC30530F60085A363C74926A87E5E32E737A
```

注入地址与该 `gamemd.exe` 版本绑定。更换可执行文件后，必须重新确认指令地址和
Hook 范围，不能直接假设兼容。

## 3. 目录结构

目标 INI 对象由 `enabled` 下的子目录决定，不在 `ra2hook.ini` 中配置文件列表：

```text
<game>/
|-- gamemd.exe
|-- ra2hook.dll
`-- ra2hook/
    |-- ra2hook.ini
    |-- ra2hook.log
    `-- inject/
        |-- enabled/
        |   |-- rules/   -> rulesmd.ini
        |   |-- ra2md/   -> ra2md.ini
        |   |-- art/     -> artmd.ini
        |   |-- ai/      -> aimd.ini
        |   |-- uimd/    -> uimd.ini
        |   `-- sound/   -> soundmd.ini
        `-- mix/          -> 自动注册目录中的全部 .mix
```

每个目标目录都会扫描全部 `*.ini`，入口文件名没有特殊要求。推荐统一命名为
`index.ini` 或 `include.ini`，避免维护时混淆。

## 4. 配置

在 `<game>\ra2hook\ra2hook.ini` 中启用：

```ini
[Inject]
Enabled=yes
Mix=yes

[Log]
Level=3
```

- `Enabled` 是 INI 注入总开关。
- `Mix` 控制是否注册 `ra2hook\inject\mix\*.mix`。
- 不存在 `Files=` 配置。文件属于哪个目标，只由所在子目录决定。

## 5. 私有 include 用法

例如 `ra2hook\inject\enabled\rules\index.ini`：

```ini
[#include]
+=units.ini
+=weapons.ini
+=balance.ini
```

也支持普通键名：

```ini
[#include]
1=units.ini
2=weapons.ini
```

解析规则：

1. 先合并入口文件自身的普通段；
2. 再按 `[#include]` 中的出现顺序递归合并引用文件；
3. 后合并的同名键覆盖先前值；
4. include 路径先相对当前散装 INI 解析，再通过游戏/MIX 文件系统查找；
5. 循环引用和超过 32 层的 include 会被拒绝；
6. 缺失文件、可恢复的无效文本会写入日志并跳过，不影响其他 include；
7. 私有 `[#include]` 段本身不会写入目标 INI。

入口目录中的所有 INI 都会被扫描。如果某个文件既位于入口目录，又被另一个入口
文件 include，它可能被合并两次。因此推荐入口目录只放少量索引文件，实际规则放在
其他目录或 MIX 中。

## 6. 与 Ares/Phobos 的隔离

ra2hook 使用 `CCFileClass` 读取文件原始内容，再由自己的解析器展开私有 include，
不调用可能已被 Ares Hook 的 `CCINIClass::ReadCCFile`。因此：

- 不改写原 rules/art 中的 `[#include]`；
- 不复用 Ares/Phobos 的 `var_N` 追加键命名空间；
- 多个 `+=TypeName` 会转换为 ra2hook 自己的 `RA2Hook_N=TypeName`，不会只保留
  最后一项；
- Ares/Phobos 原 include 先完成，ra2hook 再叠加自己的覆盖层；
- `$Inherits` 等扩展专用语义不会由 ra2hook 私有解析器自行模拟，是否生效取决于
  对应扩展在后续阶段是否还会消费该键。

## 7. rules 类型补注册

主 rules Hook 位于 `0x679A1B`。它在 Ares/Phobos 使用的 `0x679A15` 之后，且在
原生 `LoadTypesFromINI` 类型定义加载之前。

合并 rules 覆盖层后，ra2hook 只对发生变化的列表补跑原生注册逻辑：

```text
Countries             OverlayTypes       SuperWeaponTypes
Warheads              SmudgeTypes        TerrainTypes
BuildingTypes         VehicleTypes       AircraftTypes
InfantryTypes         Animations         VoxelAnims
Particles             ParticleSystems    WeaponTypes
Projectiles
```

`WeaponTypes` 和 `Projectiles` 没有对应的 `RulesClass::Read_*` 包装函数，因此逐项
调用原生 `FindOrAllocate`。社区规则中出现的单数 `[Projectile]` 也作为 BulletType
列表兼容别名处理，但不会改名或写回 `[Projectiles]`。

类型注册完成后，原程序继续执行 `LoadTypesFromINI`，读取新增类型的定义字段。
`0x668EF5` Hook 在该调用返回后只做数组核验，不修改游戏状态。

## 8. 已缓存全局段

以下 rules 全局段在 `0x679A1B` 前已经被原程序读取。如果覆盖层修改了它们，
ra2hook 会按依赖顺序主动重读：

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

难度、伤害、视听、箱子、辐射、地形高差、墙体、超级武器和命令栏等段本来就在
`LoadTypesFromINI` 后由原流程读取，不需要提前重复调用。`Sides`、`Colors` 和
`ColorAdd` 等会改变全局结构的段不会被主动重放。

## 9. 各目标的注入时机

| 目录 | Hook/对象 | 当前结论 |
|---|---|---|
| `rules` | `0x679A1B`, `INI_Rules` | 已实机验证，私有 include、字段覆盖和新增类型生效 |
| `art` | `0x679A1B`, `INI_Art` | 本次日志确认合并；原生后续 art 读取可见 |
| `ra2md` | `0x679A1B`, `INI_RA2MD` | 可合并；不能用于覆盖更早读取的 Phobos 启动配置 |
| `ai` | `0x52D37D`, `INI_AI` | 代码已接入，本次目录为空，功能内容未复测 |
| `uimd` | `0x53531A`, `INI_UIMD` | 代码已接入，本次目录为空；扩展可能使用自己的局部对象 |
| `sound` | `0x52C6C4` 预备，`0x7510F6` 应用 | 两阶段方案；本次目录为空，实际声音键仍需专项测试 |

art 中由 Phobos 在 `0x679A15` 提前缓存的扩展字段看不到之后写入的值。类似限制也
适用于其他在 ra2hook Hook 之前已经被扩展消费的配置。

## 10. 本次实机结果

2026-08-13 的日志记录：

```text
rules: 40 个 include，11320 个键，382 个 += 追加项
rules 段数: 8779 -> 9092

art: 26 个 include，1257 个键
art 段数: 6534 -> 6536

补注册原生 rules 类型列表: 7 个
新增 WeaponTypes: 82
新增 Projectiles/Projectile: 777
重读全局段: IQ、General，失败 0
```

类型加载后核验：

```text
SuperWeaponTypes  38/38
Warheads          41/41
BuildingTypes     28/28
VehicleTypes      34/34
AircraftTypes      5/5
InfantryTypes     11/11
Animations        75/75
WeaponTypes       82/82
```

汇总为：

```text
新增候选 1097
已跟踪 1024
找到 1024
缺失 0
未跟踪 73
```

`未跟踪 73` 表示诊断缓冲区最多保存 1024 个新增 ID，而本次候选数为 1097；它不
表示这 73 项注册失败。日志显示已跟踪项全部找到，`Projectiles` 数组也由 0 增长到
1047。用户随后在游戏中确认目标注入内容已经正常生效，因此本次 rules 注入按实际
功能验收通过。不过，日志没有逐项核验最后 73 个 ID；若未来要证明每一个新增弹体，
仍应扩大诊断容量或对指定 ID 增加专项探针。

以下警告属于预期的可恢复行为：

```text
SUYASPATS.ini / SUYASPATS-art.ini 不存在 -> 跳过，不影响其他 include
Mindmaster.ini:3 存在段外文本 -> 忽略该行，继续读取有效段
```

探针日志中 `FromAresInclude=[]` 表示本次没有在 Ares 原 include 链配置探针键，不能
据此认定加载顺序失败。

## 11. 日志判定

一次正常 rules 注入至少应出现：

```text
inject @0x679A1B: pINI=...
inject: ... expanded ... include(s), ... key(s)
inject: rulesmd.ini 注入后段数 ...->...
inject: 已补注册 ...
inject: rules 全局段重读完成（变化 ...，失败 0）
inject post @0x668EF5: ... missing=0
```

判定层次不同：

- include/键数日志证明文件被解析；
- dump 中出现定义段证明内容已经合并到 INI 对象；
- `0x668EF5 missing=0` 证明已跟踪的新 ID 进入了类型数组；
- 游戏内能够生产单位、使用武器并正常开火，才证明目标功能真正可用。

## 12. 常见问题

| 现象 | 检查项 |
|---|---|
| 没有任何 inject 日志 | `ra2hook\ra2hook.ini` 中 `[Inject] Enabled=yes`；DLL 是否加载；查看 `ra2hook\ra2hook.log` |
| 某个目录没有内容 | 文件是否位于正确的 `enabled/<target>` 子目录 |
| include 文件未找到 | 相对路径、MIX 是否注册、文件名大小写和拼写 |
| 一个缺失文件导致担忧 | 缺失项只会被跳过；检查后续兄弟 include 是否仍有 merged 日志 |
| dump 有定义但单位不存在 | ID 是否加入正确类型列表；检查 `missing`、生产条件和科技条件 |
| 单位不能生产 | `Owner`、`RequiredHouses`、`ForbiddenHouses`、`Prerequisite`、`TechLevel` |
| 武器存在但不能开火 | Weapon、Projectile、Warhead 是否都注册；资源名和目标过滤是否正确 |
| include 内容执行两次 | 文件是否既被入口目录扫描，又被另一个入口文件 include |
| Ares/Phobos 字段无效 | 该字段是否已在 `0x679A1B` 前被扩展缓存，或需要扩展自己的解析器 |

## 13. 回归测试建议

更新 Hook、INI 解析器或类型注册逻辑后，至少执行：

1. 连续启动游戏多次，确认无启动崩溃；
2. 检查 rules/art include 数量和键数没有异常下降；
3. 检查 `missing=0`，并关注 `untracked` 是否因候选过多而非零；
4. 实际生产一个新增 Infantry、Vehicle 或 Building；
5. 让使用新增 Weapon/Projectile/Warhead 的单位真实开火；
6. 验证 Ares/Phobos 原 include 和主要功能仍正常；
7. sound、ai、uimd 有内容时分别做独立测试，不用 rules 成功代替它们的验收。

更底层的地址分析、反汇编依据和否决过的 Hook 点见
`INJECT_HOOK_ANALYSIS.md`。
