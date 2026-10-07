# 启动 remove 层

set/remove 已实现：独立解析器和生产适配器（使用模拟引擎接口）共 27 组测试通过，
并通过 AddressSanitizer / UndefinedBehaviorSanitizer 检查。原生 `Clear` 已在目标
二进制中通过反汇编及 IDA 反编译核实。完整 Win32 DLL 编译和游戏内验证仍待执行。

## 目录、开关与时序

- 所有既有覆盖目标从 `inject/enabled/<target>` 改为 `inject/set/<target>`。
  请手动移动/改名旧 `enabled` 目录并保留子目录；新方案不自动加载旧路径。
- `[Inject] Enabled` 保留名称，统一控制 set 和 remove；`Mix` 与 `inject/mix` 不变。
- 首版仅扫描 `<game>/ra2hook/inject/remove/rules/*.ini`，按不区分大小写的文件名排序。
  只操作 `INI_Rules`，其他目标的 remove 目录不受支持；目录不存在是合法无操作。
- set/remove 只在启动时执行，编辑、停用或删除文件后都须完整重启游戏。

主 Hook `0x679A1B` 内的顺序：

```text
全部 set rules / ra2md / art 及各自私有 include 合并完成
  -> 读取并严格解析所有 remove/rules 入口及其 include，收集完整命令列表
  -> 全部成功才删除 INI_Rules 的显式键；失败则整个 remove 层不执行
  -> RegisterInjectedTypes
  -> ReloadInjectedGlobalRules
  -> 继续原程序的 TypeClass / art 等读取
```

ai/uimd/sound 的既有 set 挂点不变。remove 校验失败不会撤销先前 set 写入，也不应阻止
原有注册、重读和启动流程。这里保证的是**全层读入/解析成功前零删除**，不是引擎调用
失败时的事务回滚承诺。

## 严格命令语法

remove 使用独立命令解析器，**绝不作为普通游戏 INI 解析或复制**；重复 `-=` 行按
命令列表保留，不能用只保留最后一个 `-` 键的普通 INI 字典表示。

以下是手动配置示例，不会在仓库创建生效的删除文件。假设入口为
`ra2hook/inject/remove/rules/10-unit.ini`，片段放在入口目录之外：

```ini
[UNIT_ID]
-=Prerequisite
-=FactoryOwners
-=ForbiddenHouses
-=Prerequisite

[#include]
+=../fragments/owners.ini
1=../fragments/houses.ini
extra=unique_remove_fragment.ini
```

`UNIT_ID` 应替换成目标定义段的真实 ID。片段也使用同一命令语法，例如
`ra2hook/inject/remove/fragments/owners.ini`：

```ini
[UNIT_ID]
-=RequiredHouses
```

规则：

- 普通定义段内只允许 `-=非空键名`；`+=Key`、`Foo=no`、空 `-=` 都是错误。
- `[#include]` 只允许 `+=路径` 或普通命名/编号 `key=路径`；路径不能为空，
  `-` 不能作为 include 键（`-=path` 非法）。
- 先收集当前文件正文删除命令，再按 include 出现顺序递归处理子文件。
- 子路径先相对当前文件查找，再走游戏/MIX 文件系统；MIX 必须按既有规则注册。
- included 片段应放在入口 `remove/rules` 文件夹之外，否则也会被扫描为独立入口。
  重复删除是幂等的，但重复加载仍消耗资源限额。
- 段名和键名不区分大小写、精确匹配；不存在的段/键跳过，重复删除无害。
- 不支持通配符或整段删除。注册表/列表段会被拒绝，包括类型列表（如
  `[VehicleTypes]`、`[WeaponTypes]`、`[Projectiles]` 及兼容别名 `[Projectile]`）等；
  remove 不能用于注销单位或其他类型。

## 全层失败规则

**全部根 `*.ini` 和递归 include 必须都读取、解析并通过校验，才可执行第一条删除。**
即使错误发生在最后一个根文件，也不能先删除前面根文件的键。以下任一问题拒绝整个层：

- 文件不可读取、子文件缺失；
- 坏语法（包括正文 `+=`、普通赋值 `Foo=no`、空 `-=`、include 中 `-=`）；
- 非法注册表/列表目标、通配符或整段删除请求；
- include 循环、深度超过 32，或超出资源限制。

路径必须短于 260 字节，深度上限为 32。实现还限制：标识符短于 512 字节、
单文件不超过 8 MiB、总输入不超过 64 MiB、入口不超过 256、加载文件不超过 4096、
每文件 include 项不超过 4096、删除命令不超过 65536。不得截断后继续执行。
支持 UTF-8（可带 BOM）及 ANSI/GBK；UTF-16、NUL 字节和非法标识符会被拒绝。
正文 `-=Key ; 注释` 和带引号的 include 路径可用；命令不能混入 set 或 runtime。

## 删除的含义与边界

remove 只删除 `INI_Rules` 中当前**显式存储**的 entry：

- 不是写 `no`，也不是写空字符串；
- 不恢复原始 INI、Ares/Phobos 或先前 set 覆盖层的旧值；
- 不修改读取器默认值、已缓存的 TypeClass 字段或扩展内部缓存；
- 不额外强制重读、清空类型数组或移除单位注册；既有后续读取仍按原流程执行。

因此，删除 `Prerequisite`、`FactoryOwners` 或 `ForbiddenHouses` 不承诺解除生产条件。
缺省处理、继承、其他条件以及更早的缓存仍可能影响行为。Dump 中键缺失只能证明
显式 entry 不再存在，不能证明默认效果或游戏玩法。

这与运行时补丁移除键/文件后恢复本局基线不同；运行时目录和 UI 不消费 remove 命令。
详见 [RUNTIME_INI.md](./RUNTIME_INI.md)。

## 原生适配与验证状态

目标 `gamemd.exe` MD5 `56d582a1d6f3c144d3adc867d7a4d91b` 的
`INIClass::Clear @0x5257C0` 已反汇编确认：

- 清理 `CurrentSection` 相关缓存（对象偏移 `+0x4` / `+0x8`）；
- section 为 null 会整体重置；key 为 null 会删除整个 section；
- section/key 都非 null 时，通过 `EntryIndex` 删除并调用 entry 的虚析构。

适配器先遍历 section/entry 链表做不区分大小写的精确预查找，使用实际存储的
段名/键名大小写，只用**两者非 null** 的名称调用 Clear，再复查键已不存在。
缺失项在调用前跳过；绝不以 null key 表达删除，也不手工摘链绕过原生索引/缓存维护。

本地测试覆盖重复命令、大小写、不存在项、正文先于子 include、相对/游戏/MIX 查找、
跨根或 include 错误零删除、语法拒绝、循环及深度/资源边界、set 写入保留。
适配器测试编译实际 `RulesRemoval.cpp`，以模拟文件/引擎接口验证调用流程和删除后检查；
它不能证明真实游戏 ABI 或 Ares/Phobos 共存行为。运行方法见 [tests/README.md](./tests/README.md)。
本机没有 MSVC，CI 已加入 Win32 测试及 DLL 构建；本轮未运行真实游戏。
后续应完整重启目标游戏，用日志与 Dump 核验显式键缺失；默认值是否仍生效不属于删除失败。

相关文档：[INJECT_INI.md](./INJECT_INI.md)、
[INJECT_HOOK_ANALYSIS.md](./INJECT_HOOK_ANALYSIS.md)。
