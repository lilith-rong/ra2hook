# TODO — deferred / planned work

按讨论记录。状态：`[ ]` 未做，`[x]` 已完成，`[?]` 待先决条件。

## 注入（inject）

- [x] 既有注入按六目标拆分子目录；新路径约定为
      `ra2hook/inject/set/<rules|ra2md|art|ai|uimd|sound>/*.ini`，迁移实现见下节。
- [x] 每个目标目录支持多个 INI：按文件名排序，后写覆盖前写；目标只由目录决定。
- [x] set 文件内独立展开 `[#include]`：新路径为 `set/<target>/index.ini`
      控制加载散装或 mix 内的规则 ini，不写入/干扰 Ares/Phobos 原 include 链。
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
      **0x52C6C4** 在打开 SOUNDMD 前加载配置/MIX/INI 到持久覆盖层，再在
      **0x7510F6** 通过 `ECX` 取得 SOUNDMD 对象并只做内存复制。空目录二进制探针
      已运行 60 秒；正式源码构建和实际声音键仍待验证。
- [ ] 实测：往 `set/art、set/ai、set/uimd、set/sound` 放入测试 ini，
      确认游戏内真实生效，并分别验证 Ares/Phobos 的同名配置不会被错误覆盖
      （当前注入目录仅空 .gitkeep）。
- [ ] sound 专项实测：空目录、`[Defaults]` 覆盖、新 `[SoundList]` 条目、多个入口
      INI、重复 `+=` include、MIX 内 INI，以及 Ares + Phobos 下连续多次启动。
- [ ] 实测：Ares/Phobos 共存时确认 `0x679A1B` 能到达，且私有 include 只展开一次。
- [ ] 实测：确认 `0x668EF5` 汇总为 `missing=0, untracked=0`，并实际生产新增
      Infantry/Vehicle/Building；新增 Weapon/Projectile 还必须完成一次真实开火。

## set/remove（已实现，完整 Win32 构建/实机待验证）

源码、目录、测试和打包配置已更新，不创建生效删除文件。
历史 rules/art 实机结果不代表 remove 验收。

- [x] 记录目录迁移与严格删除约定，详见 `REMOVE_INI.md`。
- [x] 目标 MD5 `56d582a1d6f3c144d3adc867d7a4d91b` 静态反汇编确认
      `Clear @0x5257C0`：清理 `+0x4/+0x8` 缓存；null section 全重置，null key
      整段删除；两者非 null 才经 `EntryIndex` 删除及 entry 虚析构。这不是游戏测试。
- [x] 将六个既有目标的代码路径从 `inject/enabled` 改为 `inject/set`，不自动加载旧目录；
      用户手动移动/改名 enabled 为 set。`[Inject] Enabled` 名称保留并控制两阶段，
      `Mix` 不变。
- [x] 仅 `inject/remove/rules/*.ini`：全部主 set rules/ra2md/art 和私有 include 后、
      `RegisterInjectedTypes` / `ReloadInjectedGlobalRules` / 原生类型读取前删除显式键。
- [x] 独立严格命令解析器保留重复 `-=Key`；正文先于 include，子路径先当前文件再
      游戏/MIX；include 接受 `+=path` 和普通命名/编号 `key=path`，禁止 `-`。
- [x] 所有根文件及 include 全层读取/解析成功前零删除；缺失子文件、坏语法（正文
      `+=`、`Foo=no`、空 `-=` 等）、循环、深度 >32 或资源超限拒绝整个删除层，
      保留 set；缺失目录无操作。片段放入口目录外，避免重复根扫描。
- [x] 适配器用链表精确预查找、存储大小写和非 null 段/键名调用 Clear，再复查缺失；
      不存在跳过、重复无害。拒绝通配符、整段删除和注册表/列表段，不注销类型；
      不写 no/空值、不恢复旧层值、不修改默认值或已缓存 TypeClass。
- [x] 20 组解析器和 7 组生产适配器模拟测试通过，覆盖重复命令、大小写、缺失项、
      include 顺序/查找、跨根原子校验、set 保留、拒绝语法、循环及资源边界；
      ASan/UBSan 通过，生产解析器和模拟接口下的适配器以禁用异常/RTTI 选项编译通过。
- [x] IDA 再次核对当前 gamemd.exe MD5/SHA-256 和 Clear 反编译，确认只走单键分支。
- [x] CI 加入 Win32 CTest，打包 set/remove 空目录和 REMOVE_INI.md。
- [ ] 本机无 MSVC，需实际运行 CI 验证新增源码的完整 Win32 DLL 构建。
- [ ] 目标游戏验证：修改后完整重启，Dump 键缺失仅证明显式删除；实际生产限制、默认
      处理及 Ares/Phobos 缓存另验，不用静态 Clear 语义或历史实机结果替代。

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
