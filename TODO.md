# TODO — deferred / planned work

按讨论记录。状态：`[ ]` 未做，`[x]` 已完成，`[?]` 待先决条件。

## 注入（inject）

- [x] 注入目录按目标拆分子目录：`ra2hook/inject/enabled/<rules|ra2md|art|ai|uimd>/*.ini`
- [ ] **art 注入挂点** — 引擎 `INI_Art`（&CCINIClass::INI_Art，0x887180）"类型解析前"挂点未定：
      对象在 rules 窗口可能尚未装载，需先找 `artmd` 独立的加载/消费点，才能把
      `enabled/art/*.ini` 真正并进去。需要 IDA 找 xref。
- [ ] **ai 注入挂点** — `INI_AI`（0x887128），同上。
- [ ] **uimd 注入挂点** — `INI_UIMD`（0x887208）在 load-all 时对象段数为 0，
      需要找它真正的装载时机（UI 初始化段），再决定注入点。
- [ ] **sound 注入** — `soundmd` 不走 CCINIClass（引擎自解析），本套注入机制
      不适用；需独立方案（钩 sound 载入函数或直接改文件）。暂缓。

## dump

- [x] 目录已含 rules/art/ai/uimd/ra2md 五个对象（uimd 为空对象时回退拷贝散装文件）
- [?] uimd 内存对象为空的原因 —— 确认引擎到底从哪个对象读 UI 配置

## 运行时（runtime）

- [ ] 主循环 tick（取循环顶部，不取逻辑帧内部）
- [ ] 热键轮询（GetAsyncKeyState 即可，不装键盘钩子）
- [ ] 现读型属性改值（直接改 WeaponTypeClass 等字段）
- [ ] 单位创建点：拷贝型属性对新单位生效
  讨论结论：运行时能稳定改的偏 rules 里的现读字段，且不需要"那么多 inject 目标"，
  所以运行时做成独立子系统，不复用 INI 注入框架。

## mix

- [x] `ra2hook/inject/mix/*.mix` 全部注册进引擎 MixFileClass（`new MixFileClass`）
- [ ] 验证：把一个自制 mix（含 SHP）放进去，游戏内确认真实读到资源

## 其他

- [ ] 与 Ares / Phobos 同时加载的共存测试（依赖本机/游戏环境）
- [ ] hooks.json 与 ra2hook.ini 的清理/合并（见 DEVELOPMENT.md §5.1）