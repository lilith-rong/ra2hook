# TODO — deferred / planned work

按讨论记录。状态：`[ ]` 未做，`[x]` 已完成，`[?]` 待先决条件。

## 注入（inject）

- [x] 注入目录按目标拆分子目录：`ra2hook/inject/enabled/<rules|ra2md|art|ai|uimd|sound>/*.ini`
- [x] 自动模式支持每个目标目录多个 INI：按文件名排序，后写覆盖前写；`Files=` 也支持多个逗号分隔文件。
- [x] inject 文件内独立展开 `[#include]`：可用 `enabled/<target>/index.ini`
      控制加载散装或 mix 内的规则 ini，不写入/干扰 Ares/Phobos 原 include 链。
- [x] 私有 include 解析已绕过 `CCINIClass::ReadCCFile`，避免 Ares 自动展开造成重复或顺序不确定。
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
- [ ] 实测：往 `enabled/art、enabled/ai、enabled/uimd、enabled/sound` 放入测试 ini，
      确认游戏内真实生效，并分别验证 Ares/Phobos 的同名配置不会被错误覆盖
      （当前注入目录仅空 .gitkeep）。
- [ ] sound 专项实测：空目录、`[Defaults]` 覆盖、新 `[SoundList]` 条目、多个入口
      INI、重复 `+=` include、MIX 内 INI，以及 Ares + Phobos 下连续多次启动。
- [ ] 实测：Ares/Phobos 共存时确认 `0x679A1B` 能到达，且私有 include 只展开一次。

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
- [x] `ui/` WPF 控制面板 + `\\.\pipe\ra2hook-runtime-v1`：编辑、原子保存、应用、
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
