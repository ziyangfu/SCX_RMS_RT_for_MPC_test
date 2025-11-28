#pragma once
#include <string>
#include <cstdint>

enum class SchedType {
    RMS,
    RT,     // SCHED_FIFO
    CFS,    // SCHED_OTHER (Normal)
    DL      // SCHED_DEADLINE
};

struct TaskConfig {
    int cpu_id;
    uint64_t period_ns;
    uint64_t runtime_ns; // Budget
};

// 初始化调度策略
// 返回 0 成功，-1 失败
int setup_scheduler(SchedType type, const TaskConfig& config);

// 清理资源 (主要用于 RMS)
void cleanup_scheduler(SchedType type, const TaskConfig& config);
