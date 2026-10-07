# TODO — deferred / planned work

按讨论记录。状态：`[ ]` 未做，`[x]` 已完成，`[?]` 待先决条件。

## 注入（inject）

- [x] 既有注入按六目标拆分：`ra2hook/inject/<rules|ra2md|art|ai|uimd|sound>/*.ini`。
- [x] 每个目标根文件按文件名排序，普通赋值/追加/删除按行交错执行，后出现者优先。
- [x] `[#include]` 在出现行原地展开子文件，然后继续父文件，不写入/干扰扩展原链。
- [x] 私有 include 解析已绕过 `CCINIClass::ReadCCFile`，避免 Ares 自动展开造成重复或顺序不确定。
- [x] rules 私有覆盖在 `0x679A1B` 合并后，仅对变化的原生类型列表补跑注册；
      `WeaponTypes`/`Projectiles` 逐项 `FindOrAllocate`；现有规则中的单数
      `[Projectile]` 也作为兼容别名处理，随后由原流程
      `LoadTypesFromINI` 加载定义。
- [x] `0x668EF5` 在 `LoadTypesFromINI` 返回后核验新增 ID 是否进入类型数组；与
      项目自身 `0x668F6A` dump hook 无重叠，目标 Ares/Phobos hook 表也未覆盖。
- [x] **art 注入挂点** — `INI_Art`（&CCINIClass::INI_Art，0x887180）：IDA 定位
      ARTMD.INI 在 `sub_52CD70` 内 0x52d053 读进 INI_Art，早于含注入点的
      `sub_668BF0`（0x52d317 调用）；art 读取循环（0x679a66）在注入点后，
      故复用后置主挂点 **0x679A1B** 注入（kTargets[i].mapped=true）。原生 art
      字段可在类型读取前生效，但 Phobos 在 0x679A15 早期缓存的 LaserTrail 等扩展
      字段不会看到后写内容。
- [x] **ai 注入挂点** — `INI_AI`（0x887128）：AIMD.INI 在 `sub_52CD70` 0x52d378
      读进 INI_AI（晚于 0x679A15），独立挂点 **0x52D37D**（7 字节 lea，装载完成
      后、AI 读取前注入）。
- [x] **uimd 注入挂点** — `INI_UIMD`（0x887208）：UIMD.INI 在 `sub_534FA0`
      0x535311 读进 INI_UIMD，独立挂点 **0x53531A**（5 字节 mov，装载完成后、
      sub_674650 读取 0x53533d 之前注入）。
- [x] **sound 两阶段注入已实现** — `0x52C796`（relative call）、`0x52C78F`
      （ESP-relative lea）和 `0x7510D0`（修改 ESP）均已被实机崩溃否决。改用
      **0x52C6C4** 在打开 SOUNDMD 前加载配置/MIX/补丁计划，再在
      **0x7510F6** 通过 `ECX` 取得 SOUNDMD 对象并仅执行内存命令。历史空目录探针
      已运行 60 秒；当前有序计划的正式构建和实际声音键仍待验证。
- [ ] 实测：往 `inject/art、inject/ai、inject/uimd、inject/sound` 放入测试 ini，
      确认游戏内真实生效，并分别验证 Ares/Phobos 的同名配置不会被错误覆盖
      （当前注入目录仅空 .gitkeep）。
- [ ] sound 专项实测：空目录、`[Defaults]` 覆盖、新 `[SoundList]` 条目、多个入口
      INI、重复 `+=` include、MIX 内 INI，以及 Ares + Phobos 下连续多次启动。
- [ ] 实测：Ares/Phobos 共存时确认 `0x679A1B` 能到达，且私有 include 只展开一次。
- [ ] 实测：确认 `0x668EF5` 汇总为 `missing=0, untracked=0`，并实际生产新增
      Infantry/Vehicle/Building；新增 Weapon/Projectile 还必须完成一次真实开火。

## 统一有序启动补丁（代码已实现，CI/实机待验证）

- [x] 新目标目录直接位于 inject 下，旧 enabled/set/remove 不加载并提示迁移。
- [x] IniPatch 规划每一行 set/append/remove，include 原地展开，重复指令不折叠。
- [x] StartupPatch 按顺序执行：先删后设保留后值，先设后删使显式键消失。
- [x] 每目标所有根/include 校验成功前零变更；失败不保留该目标的部分 set。
      原生执行失败停止但不保证回滚，其他目标独立处理。
- [x] 只有 rules 支持 -=；列表段可写入/追加但不可删除成员；不注销单位、删除整段或
      改默认值。原生 Clear 用存储大小写、非 null 段/键名，调用前后精确核验。
- [x] sound 保持既有两 Hook 安全边界，早期预读计划、后期仅内存执行。
- [x] Runtime 仍用 IniOverlay，不消费 -=，既有基线恢复与解析策略不变。
- [x] 重写规划器 16 组、模拟适配器 12 组测试，覆盖混合顺序、include/根顺序、
      失败零修改、追加身份、存储大小写、资源边界、source-free sound 应用和原生失败。
- [x] CI 接入新测试源，打包六目标+mix 空目录和 INJECT_INI.md/REMOVE_INI.md。
- [x] 成品 ra2hook.ini 所有功能/子项/自动应用默认 no、日志 Level=0；Actions 校验打包后的安全默认值。
- [ ] 运行 GitHub Actions 测试及完整 Win32 DLL 构建（本轮未本地编译或运行测试）。
- [ ] 游戏完整重启验证混合顺序、include 覆盖、坏输入零修改，以及真实单位/武器功能。

历史：旧独立删除实现曾通过 20+7 组测试和 ASan/UBSan；目标 MD5 对应 Clear 分支也已
由 IDA 核实。这些证据不能代替当前有序实现的 CI、ABI 和 Ares/Phobos 游戏验证。
迁移时须同时复核路径、include 顺序及删除不再最终优先的变化。

## dump

- [x] 目录已含 rules/art/ai/uimd/ra2md 五个对象（uimd 为空对象时回退拷贝散装文件）
- [?] uimd 内存对象为空的原因 —— 确认引擎到底从哪个对象读 UI 配置

## 运行时（runtime）

- [x] IDA 确认主循环外层回边；选择正常帧路径 `0x55DE3A`（6 字节完整指令）执行 `RuntimeTick`。
- [x] `ReadDirectoryChangesW` + 500ms 默认 debounce + 写入稳定检查；worker 只投递命令。
- [x] 战役/遭遇战硬门禁；LAN、Internet、录像/回放拒绝写入；离局自动回滚。
- [x] 完整目标状态重建、语法验证、上一个有效状态恢复和删除键回滚。
- [x] `RulesClass::Read_*` 与 `AbstractTypeClass::LoadFromINI` 路由；首次修改前用
      `SaveToINI` 保存本局实际类型基线。
- [x] `Immediate/FutureObjects/ControlledReload/RestartRequired` 分类；资源、布局、
      类型注册和结构型字段拒绝强写。
- [x] `ui/` WPF 控制面板 + 游戏目录专用命名管道：编辑、原子保存、应用、
      暂停自动应用、回滚、旧/新值、安全等级与筛选。
- [x] UI 本地 .NET 8 Release 编译及 win-x64 自包含单文件发布。
- [?] DLL Action 编译：本机无 MSVC，需 CI 验证新增 C++ 文件。
- [ ] 实机：验证 tick、watcher、类型 baseline、失败保留、删除回滚、离局回滚。
- [ ] 实机：Ares / Phobos / 两者共存下验证原生和扩展字段边界。

## mix

- [x] `ra2hook/inject/mix/*.mix` 全部注册进引擎 MixFileClass（`new MixFileClass`）
- [ ] 验证：把一个自制 mix（含 SHP）放进去，游戏内确认真实读到资源

## 其他

- [ ] 与 Ares / Phobos 同时加载的共存测试（依赖本机/游戏环境）
- [ ] hooks.json 与 ra2hook.ini 的清理/合并（见 DEVELOPMENT.md §5.1）
