# RMS 单调速率调度实时性方案实践

## 1. 应用场景

理想汽车在车辆控制任务（如电机扭矩控制、电池管理、制动响应等）中对实时性有着严苛的要求。这类任务通常具有以下特征：

1. 短周期特性：任务执行周期为毫秒级
2. 低延迟需求：响应时间要求达到微秒至毫秒级
3. 确定性要求：必须保证在严格时限内完成

典型应用场景包括：

- 防抱死制动系统（ABS）需要在 10ms 周期内完成轮速信号处理与液压调节
- 自动驾驶的二次规划求解器要求 10ms 内完成确定性响应

传统通用操作系统（如 Linux 默认的 CFS 调度器）的调度策略难以满足这些硬实时需求，因为其设计初衷是优化系统通用性和整体性能，而非保证实时性。

单调速率调度（Rate-Monotonic Scheduling, RMS）作为一种静态优先级调度算法，通过以下机制为周期性任务提供确定性保障：

1. 优先级分配原则：任务优先级与其周期呈反比，周期越短的任务优先级越高
2. 调度可行性：只要存在可行的静态优先级调度顺序，RMS 算法就能确保所有任务在截止时间内完成

该算法的理论依据来自 Liu 和 Layland 的[证明](https://igm.univ-mlv.fr/~masson/pdfANDps/liulayland73.pdf)：

- CPU 利用率为所有任务的预算/周期之和
- 最坏情况下 CPU 利用率上限为：$$N(2^{1/N}  - 1)$$
- 当 N=1 时，利用率可达100%
- 当 N→∞ 时，利用率趋近于69%
- 只要系统总利用率不超过该上限，RMS 就能保证任务可调度

这种特性使 RMS 特别适合车辆控制系统中对时间确定性要求严格的实时任务调度。

但是，RMS 算法的应用受到若干理论约束条件的限制，必须满足 Liu 和 Layland 提出的硬实时系统基本假设条件。根据经典实时调度理论，该算法仅在满足以下严格前提条件下才能保证任务集的可调度性：

> (A1) 所有有严格截止时间的任务请求都是周期性的，请求之间的间隔是固定的。
>
> (A2) 截止日期仅指时间限制，即每个任务必须在下一次请求到来之前完成。
>
> (A3) 任务是独立的，即某个任务的请求不依赖于其他任务的启动或完成。
>
> (A4) 每个任务的运行时间是恒定的。运行时间指处理器在无中断情况下执行任务所需的时间。
>
> (A5) 系统中的非周期性任务是特殊的；它们是初始化或故障恢复程序，在它们运行时会取代周期性任务，并且本身没有严格的关键截止日期。

## 2. 演示目标

### 2.1. 实时性能对比

- 原生调度器：在加压情况下关键任务(10ms周期)抖动是 ms 级，最坏情况延迟超过 deadline；
- RMS调度器：所有测试任务抖动控制在 μs 级，均满足 deadline 约束；

## 3. 技术方案

![图1：可编程调度框架](../_static/image/rms_practice_scx.png)

本方案基于 Linux 内核可编程调度框架 [sched_ext](https://github.com/sched-ext/scx) 构建，该框架旨在允许开发者动态实现和加载自定义的调度策略，而无需修改内核核心代码或重启系统。它通过提供一组灵活的编程接口（sched_ext_ops），将调度逻辑从内核迁移到用户空间或 eBPF 程序中，从而支持快速实验和定制化调度需求。

![图2：多任务在RMS算法下的运行情况](../_static/image/rms_practice_rms.png)

假设有三个任务 T1、T2、T3，周期分别为 20、40 和 80，则在 RMS 调度策略下，三个任务的优先级为 T1 > T2 > T3，执行情况如图所示。当三个任务同时到达时，根据优先级，T1 会优先执行，其次是 T2，最后是 T3。在 20 时刻 T1 的周期再次到来时，T3 必须让出CPU，优先给 T1 使用。

![图3：技术方案总览](../_static/image/rms_practice_implement.png)

每个 RMS 任务通过`bpf_timer`实现周期控制，在任务注册阶段，为每个周期性任务创建对应的`bpf_timer`实例定时器，设置定时器周期为任务的周期，并通过`bpf_timer_set_callback`绑定到期回调函数。当`bpf_timer`到期时，内核自动调用`rms_callback`，然后通过`scx_bpf_dispatch_vtime`将任务插入 Per-CPU 红黑树，以任务的周期排序，调度器按RMS优先级选择队首任务执行。当任务的预算 timer 到期时，任务会被 yield，确保高优先级的任务不被阻塞。

## 4. 案例实操

### 4.1. 测试环境

#### 4.1.1. 硬件环境

|平台名称|配置说明|
|-|-|
|QEMU|-machine virt -cpu cortex-a72 -m 2G -smp 4|
|A78双核平台|-s 2 -m 4G|

[4.3.2](#432-对比-linux-系统业务通用周期任务调度模型) 节的测试环境为 A78 双核硬件实测平台。需注意 QEMU 作为模拟器存在性能失真问题，若需获取准确的性能基准数据，应当在硬件平台上进行实测验证，采集的数据才具有参考价值，QEMU 结果仅适用于功能验证。

#### 4.1.2. 软件环境

- 操作系统：Linux 6.12
- 实时补丁：PREEMPT_RT
- 测试工具：stress-ng

### 4.2. 操作流程

#### 4.2.1. 环境准备

##### 4.2.1.1. 工具链

- clang >= 16.0
- llvm >= 16.0
- pahole >= 1.25
- libelf-dev, zlib1g-dev, libcap-dev, libbfd-dev

##### 4.2.1.2. 环境需求

- 内核需支持 sched_ext 调度策略
- 建议使用实时内核（如 PREEMPT_RT）以获得更好的实时性保障
- 运行 scx_rms  需要 root 权限

RMS 调度器基于 sched_ext 框架实现，该框架已在 [Linux 6.12](https://kernelnewbies.org/Linux_6.12) 版本合入主线，可直接使用。小于 6.12 版本的内核需要打上 sched_ext 补丁集。

##### 4.2.1.3. 编译带 sched_ext 支持的内核

```sh
cd kernel
export ARCH=arm64
export CROSS_COMPILE=aarch64-linux-gnu-
make qemu_defconfig
```

需要开启的CONFIG

```defconfig
CONFIG_BPF_JIT=y
CONFIG_BPF_SYSCALL=y
# CONFIG_DEBUG_INFO_REDUCED is not set
CONFIG_DEBUG_INFO_BTF=y
CONFIG_SCHED_CLASS_EXT=y
CONFIG_FTRACE=y
CONFIG_BPF_EVENTS=y

# 以下选择开启
CONFIG_BPF_JIT_ALWAYS_ON=y
CONFIG_BPF_JIT_DEFAULT_ON=y
CONFIG_PAHOLE_HAS_SPLIT_BTF=y
CONFIG_PAHOLE_HAS_BTF_TAG=y
```

##### 4.2.1.4. 编译 RMS 调度器

```sh
export ARCH=arm64
export CROSS_COMPILE=aarch64-linux-gnu-
cd tools/sched_ext
make
```

#### 4.2.2. 使用步骤

1. 启动用户态调度器 scx_rms

    在运行需要应用 RMS 调度策略的任务前，必须先启动 scx_rms 程序，以启用 RMS 调度策略的交互逻辑。

    ```sh
    sudo ./scx_rms  #启动 RMS 调度器
    ```

2. 编写 rms 调度应用

    ```c
    /**
    * @brief 配置当前线程/进程的 RMS（Rate-Monotonic Scheduling）调度参数
    * 
    * @param cpu     绑定的目标CPU核心编号（如0、1等），如果为 -1，则随机分配
    * @param budget  单周期内最大执行时间（单位：纳秒），即任务的时间预算（C_i）
    *                - 例如：2ms 应转换为 2 * 1000 * 1000 = 2000000 ns
    *                - 必须满足 budget ≤ period
    * @param period  任务周期（单位：纳秒），即相邻两次激活的时间间隔（T_i）
    *                - 例如：10ms 应转换为 10 * 1000 * 1000 = 10000000 ns
    *                - 周期越短的任务优先级越高（RMS核心规则）
    * 
    * @return int    返回状态码：
    *                - 0: 成功
    *                - -1: 失败
    * 
    * @note 此函数会执行以下原子操作：
    *       1. 参数有效性校验（满足准入规则）
    *       2. 将线程绑定到指定CPU（通过sched_setaffinity）
    *       3. 设置调度策略为SCHED_EXT（需内核配置CONFIG_SCHED_CLASS_EXT）
    *       4. 通过sched_ext的BPF接口注册RMS参数（budget/period）
    *
    * @warning 任务结束前需要调用 drain_rms_exit_queue 清理配置的任务
    * @example 
    *     // 在CPU3上运行周期10ms、预算5ms的任务
    *     sched_rms(3, 5UL * 1000UL * 1000UL, 10UL * 1000UL * 1000UL)；
    */
    int sched_rms(int cpu, uint64_t budget, uint64_t period);
    ```

    引用 librms 库，创建一个周期为 10ms，预算为 5ms，在 cpu3 上运行的 rms 任务：

    ```c
    // demo.c
    #include <stdio.h>
    #include <unistd.h>
    #include <sched.h>
    #include <signal.h>
    #include <stdlib.h>
    #include "librms.h"

    static volatile int exit_req;

    static void sigint_handler(int sig) {
        exit_req = 1;
    }

    int main() {
        int cpu = 3;
        unsigned long budget =  5UL * 1000UL * 1000UL;
        unsigned long period = 10UL * 1000UL * 1000UL;
    
        signal(SIGINT, sigint_handler); 

        // 绑定到CPU3，设置预算5ms，周期10ms
        if (sched_rms(cpu, budget, period) != 0) {
            printf("sched_rms failed\n");
            exit(EXIT_FAILURE);
        }
    
        while(!exit_req) {
            printf("Running task...\n");
            // TASK
            sched_yield();
        }
        
        // 清理RMS任务队列
        drain_rms_exit_queue(cpu, budget, period);
        return 0;
    }
    ```

    ```Makefile
    # Makefile
    .PHONY: all clean

    all: demo

    demo: demo.c
        $(CROSS_COMPILE)gcc -I$(LIBRMSDIR)/include -L$(LIBRMSDIR) $(INCLUDES) $< -o $@ -lrms -lbpf

    install: all
        cp demo $(DESTDIR)

    clean:
        rm -f *.o demo
    ```

### 4.3. 优化效果

**周期抖动**：评估周期性任务的调度器的实时性能，通过计算任务的平均周期抖动和最大周期抖动，可以评估调度性能。计算方法如下所示：

![图4：周期抖动的计算方法](../_static/image/rms_practice_jitter.png)

- 理想周期 T0
- 周期抖动 = (T1-T0,T2-T0,T3-T0,...,Tn-T0)
- 最大周期抖动 = max(T1-T0,T2-T0,T3-T0,...,Tn-T0)

#### 4.3.1. QEMU 参考数据

<table>
    <tr>
        <td colspan="2" > QEMU </td>
        <td align="center" valign="middle" colspan="3">RMS+BPF Timer</td>
    </tr>
    <tr>
        <td rowspan="2">测试条件</td>
        <td>周期</td>
        <td>10ms</td>
        <td>20ms</td>
        <td>100ms</td>
    </tr>
    <tr>
        <td>预算</td>
        <td>3ms</td>
        <td>7ms</td>
        <td>5ms</td>
    </tr>
    <tr>
        <td colspan="2">测试时间</td>
        <td align="center" valign="middle" colspan="3">10min</td>
    </tr>
    <tr>
        <td colspan="2">平均周期抖动（μs）</td>
        <td> 70.223 </td>
        <td> 74.612 </td>
        <td> 72.097 </td>
    </tr>
    <tr>
        <td colspan="2">最大周期抖动（μs）</td>
        <td> 9937.312 </td>
        <td> 11138.976 </td>
        <td> 7839.184</td>
    </tr>
    <tr>
        <td colspan="2">周期抖动超过80μs的次数</td>
        <td> 13774 </td>
        <td> 5545 </td>
        <td> 1808 </td>
    </tr>
</table>

> 备注：因 QEMU 只是模拟硬件行为，对于性能数据，与实际机器测出的数据差异较大，仅供简单参考。

#### 4.3.2 对比 Linux 系统业务通用周期任务调度模型

调度优先级 DL (Deadline) > RT (Real-Time) > RMS > CFS，测试中将三个周期任务绑定到单个CPU核心运行，分别采用以下四种调度策略组合：

- RMS+BPF Timer
- RT+POSIX Timer
- CFS+POSIX Timer
- DL (Deadline Scheduler)

测试任务参数配置：

- 周期：10ms/20ms/100ms
- 预算：3ms/7ms/5ms

调度上限及利用率：

$$ U_{RMS} = 3(2^{1/3}-1) ≈ 0.779  (> 实际测试利用率 = 3/10 + 7/20 + 5/100 = 0.7) $$

##### 4.3.2.1. 未加压数据

<table>
    <tr>
        <td colspan="2" > A78 双核平台 + 未加压</td>
        <td align="center" valign="middle" colspan="3">RMS+BPF Timer</td>
        <td align="center" valign="middle" colspan="3">RT+POSIX Timer</td>
        <td align="center" valign="middle" colspan="3">CFS+POSIX Timer</td>
        <td align="center" valign="middle" colspan="3">DL</td>
    </tr>
    <tr>
        <td rowspan="2">测试条件</td>
        <td>周期</td>
        <td>10ms</td>
        <td>20ms</td>
        <td>100ms</td>
        <td>10ms</td>
        <td>20ms</td>
        <td>100ms</td>
        <td>10ms</td>
        <td>20ms</td>
        <td>100ms</td>
        <td>10ms</td>
        <td>20ms</td>
        <td>100ms</td>
    </tr>
    <tr>
        <td>预算</td>
        <td>3ms</td>
        <td>7ms</td>
        <td>5ms</td>
        <td align="center" valign="middle">-</td>
        <td align="center" valign="middle">-</td>
        <td align="center" valign="middle">-</td>
        <td align="center" valign="middle">-</td>
        <td align="center" valign="middle">-</td>
        <td align="center" valign="middle">-</td>
        <td>3ms</td>
        <td>7ms</td>
        <td>5ms</td>
    </tr>
    <tr>
        <td colspan="2">测试时间</td>
        <td align="center" valign="middle" colspan="3">10min</td>
        <td align="center" valign="middle" colspan="3">10min</td>
        <td align="center" valign="middle" colspan="3">10min</td>
        <td align="center" valign="middle" colspan="3">10min</td>
    </tr>
    <tr>
        <td colspan="2">平均周期抖动（μs）</td>
        <td> 1.248 </td>
        <td> 2.242 </td>
        <td> 0.423 </td>
        <td> 2.999 </td>
        <td> 3.369 </td>
        <td> 9.152 </td>
        <td> 2.920 </td>
        <td> 3.959 </td>
        <td> 9.098 </td>
        <td> 0.600 </td>
        <td> 0.712 </td>
        <td> 0.299 </td>
    </tr>
    <tr>
        <td colspan="2">最大周期抖动（μs）</td>
        <td> 7.611 </td>
        <td> 8.685 </td>
        <td> 8.885 </td>
        <td> 103.405 </td>
        <td> 68.420 </td>
        <td> 126.495 </td>
        <td> 100.860 </td>
        <td> 725.70 </td>
        <td> 109.765 </td>
        <td> 11.285 </td>
        <td> 39.195 </td>
        <td> 17.945 </td>
    </tr>
    <tr>
        <td colspan="2">周期抖动超过80μs的次数</td>
        <td> 0 </td>
        <td> 0 </td>
        <td> 0 </td>
        <td> 1 </td>
        <td> 0 </td>
        <td> 1 </td>
        <td> 1 </td>
        <td> 0 </td>
        <td> 1 </td>
        <td> 0 </td>
        <td> 0 </td>
        <td> 0 </td>
    </tr>
</table>

##### 4.3.2.2. 加压数据

使用 stress-ng 工具进行加压测试

```sh
stress-ng -c 2
```

<table>
    <tr>
        <td colspan="2" > A78 双核平台 + 加压</td>
        <td align="center" valign="middle" colspan="3">RMS+BPF Timer</td>
        <td align="center" valign="middle" colspan="3">RT+POSIX Timer</td>
        <td align="center" valign="middle" colspan="3">CFS+POSIX Timer</td>
        <td align="center" valign="middle" colspan="3">DL</td>
    </tr>
    <tr>
        <td rowspan="2">测试条件</td>
        <td>周期</td>
        <td>10ms</td>
        <td>20ms</td>
        <td>100ms</td>
        <td>10ms</td>
        <td>20ms</td>
        <td>100ms</td>
        <td>10ms</td>
        <td>20ms</td>
        <td>100ms</td>
        <td>10ms</td>
        <td>20ms</td>
        <td>100ms</td>
    </tr>
    <tr>
        <td>预算</td>
        <td>3ms</td>
        <td>7ms</td>
        <td>5ms</td>
        <td align="center" valign="middle">-</td>
        <td align="center" valign="middle">-</td>
        <td align="center" valign="middle">-</td>
        <td align="center" valign="middle">-</td>
        <td align="center" valign="middle">-</td>
        <td align="center" valign="middle">-</td>
        <td>3ms</td>
        <td>7ms</td>
        <td>5ms</td>
    </tr>
    <tr>
        <td colspan="2">测试时间</td>
        <td align="center" valign="middle" colspan="3">10min</td>
        <td align="center" valign="middle" colspan="3">10min</td>
        <td align="center" valign="middle" colspan="3">10min</td>
        <td align="center" valign="middle" colspan="3">10min</td>
    </tr>
    <tr>
        <td colspan="2">平均周期抖动（μs）</td>
        <td> 1.283 </td>
        <td> 1.235 </td>
        <td> 1.483 </td>
        <td> 324.798 </td>
        <td> 1502.696 </td>
        <td> 395.156 </td>
        <td> 3748.837 </td>
        <td> 1415.537 </td>
        <td> 345.635 </td>
        <td> 0.898 </td>
        <td> 0.787 </td>
        <td> 0.975 </td>
    </tr>
    <tr>
        <td colspan="2">最大周期抖动（μs）</td>
        <td> 15.750 </td>
        <td> 11.385 </td>
        <td> 9.220</td>
        <td> 9041.090 </td>
        <td> 7765.730 </td>
        <td> 7395.130 </td>
        <td> 7874.350 </td>
        <td> 7400.230 </td>
        <td> 3429.480 </td>
        <td> 16.120 </td>
        <td> 15.445 </td>
        <td> 39.575 </td>
    </tr>
    <tr>
        <td colspan="2">周期抖动超过80μs的次数</td>
        <td> 0 </td>
        <td> 0 </td>
        <td> 0 </td>
        <td> 59705 </td>
        <td> 13318 </td>
        <td> 716 </td>
        <td> 59902 </td>
        <td> 13131 </td>
        <td> 675 </td>
        <td> 0 </td>
        <td> 0 </td>
        <td> 0 </td>
    </tr>
</table>
