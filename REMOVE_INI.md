# 同一启动补丁中的显式属性删除

当前不再使用独立的 `inject/remove` 目录。删除与普通赋值放在同一份
`ra2hook/inject/rules/*.ini` 中，按文件名排序、文件内从上到下交错执行。
完整目录、include 和迁移说明见 [INJECT_INI.md](./INJECT_INI.md)。

## 语法和顺序

```ini
[HTNK]
Strength=800
-=Prerequisite
Prerequisite=GAWEAP
-=FactoryOwners
-=ForbiddenHouses
```

最终 Prerequisite 为 GAWEAP，FactoryOwners/ForbiddenHouses 的显式键缺失。
删除没有最终优先权：后面任何行、include 或根文件的赋值都可以重新建立同一个键。
只删除时保留 `-=属性名` 即可；不需要写原值。

- `Key=Value`：普通新增/覆盖，空值合法但不是删除。
- `+=Value`：独立追加项，生成不占用原键的 `RA2Hook_N`；不是算术或属性列表拼接。
- `-=Key`：只删除当前段中的一个确切属性。
- 特殊 `+`/`-` 指令从原始行解析，不能作为普通 INI 同名键存储，不能折叠重复行。

include 在出现的那一行立即展开：

```ini
[#include]
+=../fragments/vehicles.ini
extra="../fragments/extra properties.ini"

[HTNK]
Prerequisite=GAWEAP
```

如果片段里删除 Prerequisite，上面父文件末尾的赋值会恢复它。
`[#include]` 中允许 `+=路径` 或命名/编号 `key=路径`，不允许 `-=路径`。
片段应放在入口目标目录之外，避免同时扫描和引用导致重复执行。

## 安全边界

- 仅 rules 目标允许删除；其他目标出现 `-=` 会拒绝整个目标计划。
- 段名/键名大小写不敏感、精确匹配。`Prerequisite` 不匹配 `Prerequisite.X`。
- 缺失段/键跳过；重复删除幂等。删除最后一个属性也不额外删除整个段。
- 每行只有一个键名，不允许空键、通配符、逗号清单、整段删除。
- 类型注册表和结构列表禁止删除成员，但仍允许正常赋值/追加。
  包括 InfantryTypes、VehicleTypes、AircraftTypes、BuildingTypes、TerrainTypes、
  SmudgeTypes、OverlayTypes、Animations、VoxelAnims、Warheads、Particles、
  ParticleSystems、WeaponTypes、Projectiles、Projectile、SuperWeaponTypes、Countries、
  Sides、AITriggerTypes、AITriggerTypesEnable、TeamTypes、TaskForces、ScriptTypes、
  TriggerTypes、Triggers、Tags、Colors、ColorAdd。
- 不注销单位，不写 no/空值，不自动恢复旧覆盖层值，不改默认兜底配置、继承、
  已缓存 TypeClass 或扩展内部缓存。后续游戏读取仍沿原流程运行。

这里的“删除”只意味着 INI_Rules 当前明确存储的 entry 不再存在。Dump 键缺失不保证
解除建造/渗透/阵营条件；默认值如何处理不属于此功能。

## 整目标预校验与失败

每个目标所有根文件、所有递归 include 都读取并严格解析成功后，才执行任何
赋值、追加或删除。缺失文件、语法错误、循环、资源超限导致**该目标本次零修改**，
其他目标独立处理；不是旧版“set 保留、remove 取消”。

源文件名与行号记录在每条指令中。若执行时原生 WriteString/Clear 失败，停止剩余
操作并报告已执行计数，不承诺事务回滚。完整读取预校验与执行失败回滚不是一回事。
限制及编码规则见 INJECT_INI.md；UTF-16、NUL、空删除键均拒绝。

## 原生删键适配

目标 gamemd.exe MD5 `56d582a1d6f3c144d3adc867d7a4d91b` 的
`INIClass::Clear @0x5257C0` 曾经反汇编及 IDA 核实：

- 清理 CurrentSection 相关缓存（偏移 +0x4/+0x8）。
- section 为 null 会整体重置；key 为 null 会删除整个 section。
- 两者都非 null 时，删除对应 EntryIndex 项并调用该 entry 的虚析构。

`src/StartupPatch.cpp` 先遍历段/键链表做精确预查找，使用实际存储的大小写，
只以非空段名和非空键名调用 Clear，再复查缺失。不存在项不调用 Clear，
不以 ReadString/GetKeyCount 的缺省或缓存回落判断存在性，也不手工摘链绕过原生维护。
普通覆盖同样使用已存在的段/键大小写，防止生成重复拼写。

## 迁移与验证状态

旧 `enabled/set/remove` 不会自动加载；不要简单把旧 remove 与 set 同时放进新入口
并假设删除仍最终优先。根据希望的执行顺序合并，或把兼容删除片段放入口目录外，
由实际排序最后的根文件引用。旧 include“正文先于子文件”的顺序也需复核。

旧独立删除实现的 27 组测试、ASan/UBSan 及 Clear 静态核实属于历史结果。
**当前有序实现新增 16 组规划器、12 组模拟适配器测试；本轮未本地编译或执行，
由 GitHub Actions 验证。完整 Win32 DLL 和真实游戏验证仍待执行。**
测试使用模拟引擎，不证明真实 ABI/Ares/Phobos 共存行为。

修改后完整重启游戏，分别测试先删后设、先设后删、include 覆盖顺序和错误输入时
整目标零修改。Runtime/UI 不消费此命令，仍按基线恢复，见 [RUNTIME_INI.md](./RUNTIME_INI.md)。
