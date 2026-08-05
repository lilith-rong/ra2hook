// ArtMap.h — 建立「单位 → 素材文件名」映射，并导出 VXL/SHP。
//
// 映射链路（已用真实 dump 数据验证）：
//   rulesmd.ini [E1]     Image=GI          ← 必须读 Image，不能用 ID 猜
//   artmd.ini   [GI]     CameoPCX=giicon.pcx / Sequence=GISequence / ...
//
// 关键事实（来自实测，不是推断）：
//   - E1.SHP 不存在而 HTNK.VXL 存在 —— MO 重命名了素材，ID≠文件名
//   - [HTNK] 里没有 TurretAnim/BarrelAnim 键，只有 Voxel=yes：
//     炮塔炮管是**文件名约定**派生的（HTNKTUR.VXL / HTNKBARL.VXL），不是读键
//   - Voxel=yes 的段有 952 个；缺省即为 SHP
//   - NewTheater=yes 有 2315 处：文件名首字母会按地图类型替换（G/T/U/L/N/D/A）
#pragma once

namespace ArtMap {

    // 遍历 rules 的全部类型列表，解析 art 引用，导出 VXL/SHP/HVA。
    // 受 Config 的 dump.vxl / dump.shp / dump.sortByOwner 控制。
    void Run();

}  // namespace ArtMap
