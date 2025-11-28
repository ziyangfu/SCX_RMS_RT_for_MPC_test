#pragma once
#include <chrono>

// 模拟 MPC 控制器的计算负载
// target_runtime: 期望该任务占用 CPU 的时间（预算）
void run_mpc_step(std::chrono::microseconds target_runtime);
