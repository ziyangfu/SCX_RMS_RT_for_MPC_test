#include <iostream>
#include <unistd.h>
#include <csignal>
#include <cstring>
#include <chrono>
#include <iomanip>
#include <thread>
#include <vector>
#include <algorithm>
#include "sched_strategies.hpp"
#include "mpc_task.hpp"

// 全局退出标志
static volatile std::sig_atomic_t g_exit_req = 0;

void sig_handler(int) {
    g_exit_req = 1;
}

void print_usage(const char* prog_name) {
    std::cout << "Usage: " << prog_name << " -m <mode> [options]\n"
              << "Modes:\n"
              << "  rms   : Rate-Monotonic (via sched_ext)\n"
              << "  rt    : Real-Time (SCHED_FIFO)\n"
              << "  cfs   : Normal (SCHED_OTHER)\n"
              << "  dl    : Deadline (SCHED_DEADLINE)\n"
              << "Options:\n"
              << "  -c <cpu_id> : Bind to CPU (default 3)\n"
              << "  -p <ms>     : Period in ms (default 10)\n"
              << "  -b <ms>     : Budget in ms (default 5)\n"
              << "  -n <count>  : Number of cycles to run\n"
              << "  -t <min>    : Duration in minutes to run\n"
              << "Note: -n and -t are mutually exclusive.\n"
              << std::endl;
}

// 纳秒级睡眠辅助函数 (绝对时间)
void sleep_until(std::chrono::steady_clock::time_point target_time) {
    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, 
        reinterpret_cast<const struct timespec*>(&target_time), NULL);
}

int main(int argc, char* argv[]) {
    int opt;
    SchedType mode = SchedType::CFS;
    bool mode_set = false;
    int cpu_id = 3; // 默认 CPU
    
    // 默认参数
    uint64_t period_ms = 10;
    uint64_t budget_ms = 5;
    long run_cycles = -1; // -1 表示无限或未设置
    double run_duration_min = -1.0; // -1.0 表示无限或未设置

    while ((opt = getopt(argc, argv, "m:c:p:b:n:t:")) != -1) {
        switch (opt) {
            case 'm':
                if (strcmp(optarg, "rms") == 0) mode = SchedType::RMS;
                else if (strcmp(optarg, "rt") == 0) mode = SchedType::RT;
                else if (strcmp(optarg, "cfs") == 0) mode = SchedType::CFS;
                else if (strcmp(optarg, "dl") == 0) mode = SchedType::DL;
                else {
                    std::cerr << "Unknown mode: " << optarg << std::endl;
                    return 1;
                }
                mode_set = true;
                break;
            case 'c':
                cpu_id = std::atoi(optarg);
                break;
            case 'p':
                period_ms = std::stoull(optarg);
                break;
            case 'b':
                budget_ms = std::stoull(optarg);
                break;
            case 'n':
                run_cycles = std::atol(optarg);
                break;
            case 't':
                run_duration_min = std::atof(optarg);
                break;
            default:
                print_usage(argv[0]);
                return 1;
        }
    }

    if (!mode_set) {
        print_usage(argv[0]);
        return 1;
    }

    if (run_cycles > 0 && run_duration_min > 0) {
        std::cerr << "Error: Options -n and -t are mutually exclusive." << std::endl;
        return 1;
    }

    uint64_t period_ns = period_ms * 1000000UL;
    uint64_t budget_ns = budget_ms * 1000000UL;

    // 设置信号捕获
    std::signal(SIGINT, sig_handler);
    std::signal(SIGTERM, sig_handler);

    TaskConfig config{cpu_id, period_ns, budget_ns};

    // 1. 配置调度器
    if (setup_scheduler(mode, config) != 0) {
        std::cerr << "Failed to setup scheduler." << std::endl;
        return 1;
    }

    std::cout << "Starting task loop on CPU " << cpu_id 
              << " Period: " << period_ms << "ms"
              << " Budget: " << budget_ms << "ms";
    if (run_cycles > 0) std::cout << " for " << run_cycles << " cycles";
    if (run_duration_min > 0) std::cout << " for " << run_duration_min << " minutes";
    std::cout << std::endl;

    // 统计变量
    long max_positive_jitter_ns = 0;    // 最大正向抖动
    long max_negative_jitter_ns = 0;    // 最大负向抖动
    double sum_abs_jitter_ns = 0.0;     // 绝对值抖动总和
    long count_over_80us = 0;           // 超过80us的抖动次数
    long current_cycle = 0;             // 当前循环

    auto period_duration = std::chrono::nanoseconds(period_ns);
    auto budget_duration = std::chrono::microseconds(budget_ns / 1000);
    auto last_activation = std::chrono::steady_clock::now();  // 上次激活时间
    auto start_time = last_activation;
    
    // 统计数据存储
    std::vector<long> jitters;
    jitters.reserve(1000); // 预分配空间

    // 2. 主循环
    while (!g_exit_req) {
        // 预算负载 ---------------------------------------------------------
        // 预算负载包括：退出条件检查 + 业务负载 + 抖动统计

        // 周期开始点记录
        auto now = std::chrono::steady_clock::now();
        // 检查退出条件
        if (run_cycles > 0 && current_cycle >= run_cycles) break;
        if (run_duration_min > 0) {
            auto elapsed = std::chrono::steady_clock::now() - start_time;
            if (elapsed >= std::chrono::duration<double, std::ratio<60>>(run_duration_min)) break;
        }

        // 第一次循环不计算抖动,current_cycle == 0 为第一次循环。只有从第二次循环开始才能计算周期抖动
        if (current_cycle > 0) { 
            // 实际周期时长
            auto actual_period = now - last_activation;
            long actual_period_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(actual_period).count();
            
            // 抖动 = 实际周期 - 理论周期
            long jitter_ns = actual_period_ns - period_ns;
            
            // 存储抖动数据
            jitters.push_back(jitter_ns);
            
            // 更新统计
            sum_abs_jitter_ns += std::abs(jitter_ns);
            
            if (jitter_ns > max_positive_jitter_ns) max_positive_jitter_ns = jitter_ns;
            // 负向抖动通常是负数，我们要找最小的负数（绝对值最大的负向偏差）
            if (jitter_ns < max_negative_jitter_ns) max_negative_jitter_ns = jitter_ns;
            
            if (std::abs(jitter_ns) > 80000) count_over_80us++;

            std::cout << "current Cycle " << current_cycle << ": Jitter = " << jitter_ns / 1000.0 << " us" << std::endl;
        }
        current_cycle++;
        last_activation = now;
        // 业务负载
        run_mpc_step(budget_duration);
        // 预算负载结束 ---------------------------------------------------------

        // --- 周期控制 ---
        if (mode == SchedType::RMS || mode == SchedType::DL) {
            sched_yield();
        } 
        else {
            // RT/CFS: 计算下一个理论时刻并睡眠
            auto next_target = last_activation + period_duration;
            std::this_thread::sleep_until(next_target);
        }
    }

    // 3. 输出最终统计结果
    // 3. 输出最终统计结果
    std::cout << "\n------------------------------------------------------------------" << std::endl;
    std::cout << "Final Statistics:" << std::endl;
    std::cout << "  Total Cycles: " << current_cycle << std::endl;
    if (current_cycle > 1) {
        // 收集所有抖动数据用于排序
        std::vector<long> all_jitters;
        all_jitters.reserve(jitters.size());
        
        double sum_abs_jitter_filtered = 0.0;
        long count_filtered = 0;

        for (long j : jitters) {
            all_jitters.push_back(j);
            
            // 计算去除 >80us 后的平均抖动
            if (std::abs(j) <= 80000) {
                sum_abs_jitter_filtered += std::abs(j);
                count_filtered++;
            }
        }

        // 1. 总体平均绝对抖动
        double avg_abs_jitter = sum_abs_jitter_ns / jitters.size();
        std::cout << "  Avg Abs Jitter (All): " << std::fixed << std::setprecision(3) << avg_abs_jitter / 1000.0 << " us" << std::endl;
        
        // 2. 去除 >80us 后的平均绝对抖动
        if (count_filtered > 0) {
            double avg_abs_jitter_filtered = sum_abs_jitter_filtered / count_filtered;
            std::cout << "  Avg Abs Jitter (<=80us): " << avg_abs_jitter_filtered / 1000.0 << " us" << std::endl;
        } else {
            std::cout << "  Avg Abs Jitter (<=80us): N/A (All > 80us)" << std::endl;
        }

        std::cout << "  >80us Count: " << count_over_80us << std::endl;

        // 3. Top 10 Max +Jitter (最大正向抖动)
        std::sort(all_jitters.begin(), all_jitters.end(), std::greater<long>()); // 降序排列
        std::cout << "  Top 10 Max +Jitter: [";
        for (int i = 0; i < 10 && i < all_jitters.size(); ++i) {
            if (all_jitters[i] < 0) break; // 如果全是负数，就不打印了
            std::cout << all_jitters[i] / 1000.0 << (i == 9 || i == all_jitters.size() - 1 ? "" : ", ");
        }
        std::cout << "] us" << std::endl;

        // 4. Top 10 Max -Jitter (最大负向抖动 - 绝对值最大)
        std::sort(all_jitters.begin(), all_jitters.end()); // 升序排列 (负数在前)
        std::cout << "  Top 10 Max -Jitter: [";
        for (int i = 0; i < 10 && i < all_jitters.size(); ++i) {
            if (all_jitters[i] > 0) break; // 如果全是正数，就不打印了
            std::cout << all_jitters[i] / 1000.0 << (i == 9 || i == all_jitters.size() - 1 ? "" : ", ");
        }
        std::cout << "] us" << std::endl;

    } else {
        std::cout << "  Not enough cycles for statistics." << std::endl;
    }
    std::cout << "------------------------------------------------------------------" << std::endl;

    // 3. 清理工作
    cleanup_scheduler(mode, config);
    std::cout << "\nExiting gracefully." << std::endl;

    return 0;
}
