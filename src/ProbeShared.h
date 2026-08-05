// ProbeShared.h — 探针共享常量（验证完可随探针一起删）。
#pragma once

namespace Probe {
    // 注入测试值。E1（美国大兵）vanilla Strength = 100，用一个明显不同的值，
    // 便于在日志里一眼区分"注入生效"与"读到原值"。
    inline constexpr int kTestStrength = 543;
}
