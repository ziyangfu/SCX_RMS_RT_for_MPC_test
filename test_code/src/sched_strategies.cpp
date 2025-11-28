#include "sched_strategies.hpp"
#include "librms.h" // 引用你的本地头文件

#include <sched.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <linux/sched.h> // For SCHED_DEADLINE definitions
#include <linux/types.h>
#include <iostream>
#include <cstring>
#include <thread>
#include <vector>

// Deadline 调度所需的结构体定义（内核态接口）
struct sched_attr02 {
    __u32 size;
    __u32 sched_policy;
    __u64 sched_flags;
    __s32 sched_nice;
    __u32 sched_priority;
    __u64 sched_runtime;
    __u64 sched_deadline;
    __u64 sched_period;
};

// 辅助函数：绑定 CPU
static int pin_to_cpu(int cpu_id) {
    if (cpu_id < 0) return 0; // -1 代表不绑定
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu_id, &cpuset);
    return sched_setaffinity(0, sizeof(cpuset), &cpuset);
}

int setup_scheduler(SchedType type, const TaskConfig& config) {
    // 1. 先绑定 CPU (对于 RT 很重要)
    // RMS 内部实现了绑定
    // DL (SCHED_DEADLINE) 不允许设置亲和性 (EPERM)，它由内核全局调度
    if (type != SchedType::RMS && type != SchedType::DL) {
        if (pin_to_cpu(config.cpu_id) != 0) {
            perror("sched_setaffinity failed");
            return -1;
        }
    }

    switch (type) {
        case SchedType::RMS: {
            std::cout << "[Mode] RMS (via librms)" << std::endl;
            // RMS 内部会处理 setaffinity 和 BPF 注册
            return sched_rms(config.cpu_id, config.runtime_ns, config.period_ns);
        }

        case SchedType::RT: {
            std::cout << "[Mode] RT (SCHED_FIFO)" << std::endl;
            struct sched_param param;
            param.sched_priority = 90; // 设置一个较高的实时优先级
            if (sched_setscheduler(0, SCHED_FIFO, &param) != 0) {
                perror("sched_setscheduler RT failed");
                return -1;
            }
            return 0;
        }

        case SchedType::CFS: {
            std::cout << "[Mode] CFS (SCHED_OTHER)" << std::endl;
            struct sched_param param;
            param.sched_priority = 0;
            if (sched_setscheduler(0, SCHED_OTHER, &param) != 0) {
                perror("sched_setscheduler CFS failed");
                return -1;
            }
            // CFS 可以设置 nice 值，这里省略
            return 0;
        }

        case SchedType::DL: {
            std::cout << "[Mode] DL (SCHED_DEADLINE)" << std::endl;
            struct sched_attr02 attr;
            memset(&attr, 0, sizeof(attr));
            attr.size = sizeof(attr);
            attr.sched_policy = SCHED_DEADLINE;
            attr.sched_runtime = config.runtime_ns;
            attr.sched_deadline = config.period_ns; // 通常 deadline = period
            attr.sched_period = config.period_ns;

            // 使用 syscall 直接调用，因为 glibc 可能未封装
            if (syscall(__NR_sched_setattr, 0, &attr, 0) != 0) {
                perror("sched_setattr DL failed");
                return -1;
            }
            return 0;
        }
    }
    return -1;
}

void cleanup_scheduler(SchedType type, const TaskConfig& config) {
    if (type == SchedType::RMS) {
        drain_rms_exit_queue(config.cpu_id, config.runtime_ns, config.period_ns);
        std::cout << "RMS resources cleaned up." << std::endl;
    }
}
