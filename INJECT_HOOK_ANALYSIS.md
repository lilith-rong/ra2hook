# ra2hook 后置 INI 注入点分析与实机测试说明

记录日期：2026-08-11

本文记录 rules 注入点从 `0x679A15` 后移到 `0x679A1B` 的分析依据、预期时序和实机测试方法，并补充各个 INI 目标与 Ares/Phobos 的兼容边界。

## 1. 当前结论

- 当前源码已切换到 **`0x679A1B`，补丁长度 `0x5`**；这是静态分析通过、实机待验证的候选点。
- IDA 静态分析确认 `0x679A1B` 位于 Ares/Phobos 使用的 `0x679A15` 之后。
- 到达 `0x679A1B` 时，`ESI` 已由原程序设置为 `CCINIClass*`，可以直接作为 rules 注入目标。
- `0x679A1B` 仍在第一段 TypeClass 读取之前，写入 INI 的值应能被后续类型解析采纳。
- 该方案避免 ra2hook 与 Ares/Phobos 在 `0x679A15` 上依赖同址 hook 的执行顺序。
- inject 文件现在通过 `CCFileClass` 读取原始字节并由 ra2hook 自己解析，不再调用
  可能被 Ares hook 的 `CCINIClass::ReadCCFile`；私有 `[#include]` 只展开一次。
- **`0x679A1B` 目前只是经过 IDA 确认的候选点，源码已切换，但尚未实机验证。**

已有实测事实仍然有效：旧点 `0x679A15` 收到的 `pINI` 是 `INI_Rules`，且写入 `[E1]Strength=543` 后，类型解析结果确实变为 543。这证明该控制流区域处于 INI 装载完成之后、类型消费之前。新点只比它后移 6 字节。

## 2. 适用的 gamemd.exe

本次 IDA 分析针对：

```text
文件：D:\Program\Game\ra2hook\gamemd.exe
映像基址：0x400000
MD5：56d582a1d6f3c144d3adc867d7a4d91b
SHA-256：7cd005d263fde203d9c84548200a057a8df61d724da3c6bd1e521eeb61cd0747
```

这些地址不能直接套用到其他版本的 `gamemd.exe`。如果可执行文件散列不同，需要重新确认函数和指令边界。

## 3. IDA 定位结果

目标函数为 `sub_679A10`，函数大小 `0x2A7`。入口处的关键指令如下：

```asm
00679A10  mov  eax, dword_A83CA8
00679A15  push ebx
00679A16  push esi
00679A17  mov  esi, [esp+0Ch]   ; CCINIClass* 参数
00679A1B  push edi
00679A1C  xor  edi, edi
00679A1E  test eax, eax
00679A20  jle  ...
```

对应原始字节：

```text
0x679A10: A1 A8 3C A8 00 53 56 8B 74 24 0C 57 33 FF 85 C0 7E 18
0x679A15: 53 56 8B 74 24 0C             ; 6 字节
0x679A1B: 57 33 FF 85 C0                ; 5 字节
```

`0x679A15` 的 6 字节正好执行完：

1. 保存 `EBX`；
2. 保存 `ESI`；
3. 从栈参数取出 `CCINIClass*` 放入 `ESI`。

因此，正常控制流到达 `0x679A1B` 时，INI 指针已经稳定地位于 `ESI`。候选点覆盖的 5 字节是三条完整指令，按 Syringe/YRpp 的标准 trampoline 重放后，会在 `0x679A20` 恢复原程序控制流和 `test eax,eax` 产生的标志位。

IDA 中已经在 `0x679A1B` 留下注释，说明该地址是 ra2hook 的 post-Ares/Phobos 候选注入点。

## 4. 预期执行时序

```text
原程序先执行 0x679A10
        |
        v
Ares/Phobos 在 0x679A15 的 handler 完成 INI/include 相关处理
        |
        v
Syringe 重放 0x679A15 的 6 字节，ESI = CCINIClass*
        |
        v
ra2hook 在 0x679A1B 合并私有 inject INI
        |
        v
Syringe 重放 0x679A1B 的 5 字节
        |
        v
0x679A20 开始原类型读取流程
```

这个顺序不再取决于多个 DLL 在同一个地址上的 handler 排列。只要 Ares/Phobos 的 `0x679A15` hook 最终以正常路径继续执行，控制流就会到达 `0x679A1B`，此时 ra2hook 才开始写入。

这里仍有一个必须实测的依赖：如果某个扩展从 `0x679A15` 返回了非零替代地址并跳过正常续行，`0x679A1B` 就不会执行。现有 Ares/Phobos 用法预期会继续原流程，但要用日志确认。

## 5. 建议的代码修改

当前代码位于 `src/Hooks.RulesInject.cpp`：

```cpp
DEFINE_HOOK(0x679A1B, RA2Hook_RulesInject_PostAresPhobos, 0x5)
{
    GET(CCINIClass*, pINI, ESI);
    RulesInject::Apply(pINI);
    return 0;
}
```

切换后需要同步确认以下文字和日志：

- 文件头部的注入点、寄存器约定和补丁长度；
- `s_done`、`kReadyAtMain`、`Apply` 附近注释中的地址；
- `Log::Info("inject @0x679A1B: ...")` 中的地址；
- `README.md`、`DEVELOPMENT.md` 和 `TODO.md` 中关于主注入点的状态。

不要在新点继续使用 `GET_STACK(..., 0x4)`。它是 `0x679A15` handler 入口时的栈布局；到 `0x679A1B` 应使用已经准备好的 `ESI`。

## 6. 最小实机测试

### 6.1 准备原 Ares include 探针

在一个确定由 Ares 原 `[#include]` 链加载的 INI 中加入：

```ini
[RA2HookOrderProbe]
Stage=AresInclude

[E1]
Strength=321
```

`E1` 只是容易观察的示例。正式测试前应确认当前模组没有脚本或其他扩展在类型解析后再次修改这个值。

### 6.2 准备 ra2hook 私有 include

创建 `ra2hook\inject\enabled\rules\index.ini`：

```ini
[#include]
1=ra2hook_after.ini
```

在同目录创建 `ra2hook_after.ini`：

```ini
[RA2HookOrderProbe]
Stage=RA2HookInject

[E1]
Strength=543
```

启用配置：

```ini
[Inject]
Enabled=yes
Mix=yes

[Log]
Level=4
```

目标由 `enabled/<target>/` 子目录决定；此处不再提供无法表达目标类型的全局文件列表。

### 6.3 内置探针日志

源码现在会在调用注入前自动读取这个探针；不需要再临时改 DLL：

```cpp
char stage[32] = {};
pINI->ReadString("RA2HookOrderProbe", "Stage", "", stage, sizeof(stage));
    Log::Info("probe @0x679A1B before inject: Stage=[%s]", stage);
```

期望输出是：

```text
probe @0x679A1B before inject: Stage=[AresInclude]
```

随后应看到 rules 目录扫描、`index.ini` 的 `[#include]` 展开以及键合并完成的日志。游戏内最终值应是 543，说明 ra2hook 的覆盖发生在 Ares include 之后、类型解析之前。

## 7. 共存测试矩阵

| 环境 | 需要确认的结果 |
|---|---|
| 仅 ra2hook | 能进入游戏；`0x679A1B` 日志出现；注入值生效 |
| Ares + ra2hook | 探针先读到 `AresInclude`；最终值为 ra2hook 的 543；Ares 功能正常 |
| Phobos + ra2hook | `0x679A1B` 日志出现；最终值为 543；Phobos 功能正常 |
| Ares/Phobos + ra2hook 私有 include | Ares 原 include 和 ra2hook 私有 include 都能加载，最终覆盖顺序符合预期 |
| 私有 include 指向 mix 内 INI | mix 注册日志、include 展开日志出现，mix 内 INI 的值生效 |
| 重开一局或读档 | 不崩溃；`s_done` 幂等行为符合当前设计，不发生重复合并副作用 |

每次测试都应保留：

- 使用的 `gamemd.exe` 散列；
- Ares、Phobos、Syringe/SyringeEx 和 ra2hook 版本；
- 完整 `ra2hook.log`；
- 实际使用的两个探针 INI；
- 是否进主菜单、是否成功开始遭遇战、游戏内观察到的最终值。

## 8. 结果判定与排查

| 现象 | 优先检查 |
|---|---|
| 启动即崩溃 | `0x679A1B` 是否为当前 exe 的指令边界；hook 长度是否为 `0x5`；是否误用栈参数 |
| 没有 `0x679A1B` 日志 | DLL 是否加载；exe 散列是否一致；上游 hook 是否跳过正常续行 |
| `pINI` 无效或段数异常 | `ESI` 是否被正确读取；Syringe 是否在 handler 前保存了该点寄存器状态 |
| 探针读不到 `AresInclude` | 测试 INI 是否真的位于 Ares 原 include 链；Ares 是否完成加载；候选点时序假设不成立 |
| 探针正确但最终仍是 321 | ra2hook 文件未扫描/未展开，或 543 在更晚阶段被其他逻辑覆盖 |
| 最终值为 543，但 Ares/Phobos 功能异常 | 检查 inject 是否覆盖了扩展自身使用的键；缩小测试 INI，只保留探针键 |
| include 被执行两次 | 当前源码已绕过 `CCINIClass::ReadCCFile`，若仍重复，检查同一个文件是否既被目录扫描又被 `index.ini` 引用 |

## 9. 多 INI 与 Ares/Phobos 兼容矩阵

### 9.1 多文件合并规则

- 始终扫描 `enabled/<target>/*.ini`；目录名决定注入对象，每个目标目录按文件名不区分大小写排序后逐个合并。
- 每个文件先合并自身正文，再按 `[#include]` 段中的出现顺序深度优先合并引用文件；重复键也保留，故 Ares 常用的多行 `+=文件.ini` 可作为私有 include 入口。引用文件后写，因此覆盖当前文件。
- 推荐把 `enabled/<target>` 只作为入口目录，真正的可选规则放在 mix 或其他目录中由 `index.ini` 引用；否则同一个文件既会被目录扫描又会被 include，可能被重复写入。
- include 路径先相对当前散装文件目录解析，再按游戏/MIX 文件系统解析。mix 会在每个目标首次注入前注册，因此较早的 `sound/ai/uimd` 挂点也能引用 mix 内 INI。循环引用和超过 32 层的链会被跳过并写日志。
- 注入文件支持普通 `section/key=value` 语法。Ares/Phobos 的 `$Inherits` 等扩展语义不会在私有链中自动复制。
- 普通段中的 Ares 列表追加语法 `+=TypeName` 也会被保留为独立追加项；写入真实引擎对象时转换为 ra2hook 专用的 `RA2Hook_N=TypeName`，不会像普通 `WriteString("+", ...)` 那样只留下最后一项。ra2hook 不扫描、不复用 Ares/Phobos 使用的 `var_N` 键名空间。

### 9.2 目标和冲突边界

本次对实际游戏目录中的 `Ares.dll`、`Phobos.dll` 和旧版 `ra2hook.dll` 做了地址区间检查：
`Ares.dll` 与 `Phobos.dll` 共同占用 `0x679A15..0x679A1A`（6 字节），也共同占用
`0x5FACDF..0x5FACE3`（5 字节）；没有发现它们占用 `0x679A1B`、`0x52D37D`、
`0x53531A`、`0x52C6C4`、`0x7510F6` 或 `0x668F6A`。Ares、Phobos、IHCore 的
Syringe hook 元数据也没有覆盖新的两个 sound 点。这只能说明没有直接字节区间重叠，
不能单独证明 SyringeIH 重放指令后的寄存器和控制流安全。

2026-08-11 在目标 MD5 `56D582A1D6F3C144D3ADC867D7A4D91B`、Ares 3.0p1 +
Phobos Build #47+6_0 下依次否决了三个点，均会触发 `C0000005`，即使 sound
目录为空也一样：

- `0x52C796`：相对 `call sub_7510D0`，不能依赖 SyringeIH 安全重放；
- `0x52C78F`：`lea ecx,[esp+12Ch]`，被盗指令依赖原始 ESP；
- `0x7510D0`：函数入口 `sub esp,824h`，被盗指令直接修改 ESP。

当前方案改为两阶段：

1. `0x52C6C4` 覆盖完整 5 字节 `mov eax,dword ptr [88730Ch]`。此时尚未打开
   SOUNDMD.INI，handler 在这里执行 `Config::Load()`、注册 MIX，并把
   `enabled/sound/*.ini` 及私有 include 合并到引擎分配的持久 `CCINIClass`。
2. `0x7510F6` 覆盖完整 5 字节 `mov dword ptr [B1D3A4h],eax`。前一条
   `0x7510F4 mov ecx,edi` 已令 `ECX` 指向 SOUNDMD 对象；handler 只校验 vtable
   `0x7E1AF4` 并复制预备覆盖层，不再读取配置或文件。第一处 `[Defaults]` 查询在
   `0x751114`，所以复制仍发生在声音配置消费之前。

二进制探针已把预加载 handler 临时放到 `0x52C6C4`，把声音 handler 放到
`0x7510F6`。游戏启动 60 秒后 launcher 和 game 进程都存活，日志取得 SOUNDMD
对象 `001AA660`。这只验证了空目录下两个 hook 的控制流和对象寄存器；当次异常日志的
时间存在旧失败探针延迟写入的歧义，且实际 sound 键尚未验证。因此仍要用正式 Action
产物重复干净启动和功能测试。源码使用新导出名，游戏目录 `Syringe.json` 中遗留的
`RA2Hook_SoundInject` 禁用项不会禁用新 hook。

| 目标目录 | 当前注入对象/时机 | 原生引擎 | Ares/Phobos 影响与限制 |
|---|---|---|---|
| `rules` | `0x679A1B`，`ESI=INI_Rules` | 原生 `[General]`、类型读取前字段可生效 | 不覆盖它们在 `0x679A15` 已提前读取并缓存的扩展字段；不会改它们的原 include 链 |
| `art` | 同 `0x679A1B`，`INI_Art` | 后续 art/类型读取使用的字段可生效 | Phobos 在 `0x679A15` 早期读取的 `LaserTrail` 等列表看不到后写值 |
| `ra2md` | 同 `0x679A1B`，全局 `INI_RA2MD` | 后续引擎读取可能生效 | Phobos 在 `0x5FACDF` 的启动配置读取早于此点；不能用该目录可靠覆盖 `[Phobos]` 启动配置 |
| `ai` | `0x52D37D`，AIMD 读入 `INI_AI` 后 | AI 数据消费前可生效 | 当前未发现 Ares/Phobos 对该全局对象的直接读取；仍需实机验证 |
| `uimd` | `0x53531A`，写入全局 `INI_UIMD` | 可能影响原生后续 UI 读取 | Ares/Phobos 会重新打开 `uimd.ini` 到局部对象；该目录不能可靠覆盖它们自己的 UISettings |
| `sound` | `0x52C6C4` 预备覆盖层，`0x7510F6` 由 `ECX` 写入 SOUNDMD 局部对象 | `[Defaults]`/`[SoundList]` 原生解析前可生效 | 空目录二进制探针已存活 60 秒；正式构建、实际键和连续启动仍待验证 |
| `eva` / `theme` | 当前没有目标目录或 hook | 不支持 | 当前不注入 `evamd.ini`、`thememd.ini`；需要另找装载完成后的对象/消费点 |

### 9.3 MIX 资源冲突

`inject/mix/*.mix` 只注册文件系统资源，不会直接合并 INI。若 Ares/Phobos 或其他 MIX
包含同名的 INI/SHP/VXL/PCX，最终取哪个文件取决于引擎 MIX 查找链顺序，不能把同名资源
当作确定的覆盖机制。建议私有资源使用唯一文件名；规则覆盖使用 `WriteString` 注入。

## 10. 验收标准

只有同时满足以下条件，才能把主注入点正式认定为已迁移：

1. 目标 `gamemd.exe` 下连续多次启动无崩溃；
2. Ares include 探针在 ra2hook 注入前可见；
3. ra2hook 私有 include 的同键覆盖最终胜出；
4. 注入结果被 TypeClass 实际采纳，而不只是存在于 `CCINIClass`；
5. Ares/Phobos 原 include 和主要功能未受影响；
6. 散装 INI 与 mix 内 INI 两种来源都通过；
7. 完成测试后再把 `0x679A1B` 从“静态分析通过、实机待验证”改为“实机已验证”。

sound 还需单独确认：日志先出现 `inject prepare @0x52C6C4`，再出现
`inject apply @0x7510F6`；连续启动无新异常；测试声音能播放；`[Defaults]` 覆盖和
新增 `[SoundList]` 条目都被 `sub_7510D0` 采纳。还需分别测试多个入口 INI、重复
`+=` include，以及引用 MIX 内唯一命名 INI 的情况。

在这些测试完成前，应把 `0x679A1B` 标记为“静态分析通过、实机待验证”，不要写成已验证挂点。
