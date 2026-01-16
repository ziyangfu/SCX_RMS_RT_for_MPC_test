# 用户态

## 1. 背景

汽车电子电气架构演进过程中，跨域融合与中央计算成为共识。控制器硬件上，经历了单盒多板、单板多芯、单芯异构等几个阶段。软件域上，正在经历智驾域与座舱域的融合（Cortex A + Cortex A）以及运动域与智驾域融合（Cortex R + Cortex A），后续还会演进到片上SoC+虚拟化技术+混合部署框架+多域操作系统的技术方案，最终形成一个混合关键性系统(MCS, Mixed Criticality System)。

就运动域与智驾域而言，这可能涉及到运动域与智驾域的业务融合重构，智驾域中的功能安全岛可能会合并至运动域。例如，AEB、ABS、VDC等功能可以直接融合在一起。而运动域中跟算力相关的部分软件，也可能挪移到智驾域中，例如舒适领航、路面预瞄、AI悬架等。

在这种背景下，有一些应用，它既需要Linux的管理能力、丰富的生态以及SoC的强大算力，又对确定性与实时性有所追求。

这种与MCU打交道的应用，惯性使然，一般是周期性运算与发送的。它会关心我的发送周期是不是稳定，抖动有多少？在固定的周期内，我的算法能不能算完。而影响周期稳定性的因素有很多，算法计算非恒定耗时、调度延迟、通信延迟等。本文尝试从周期抖动出发，以MPC模型预测控制算法为例，测试它的周期抖动，然后再进一步分析。

这类应用，一般可以采用isolcpus孤立核心以及采用taskset或者亲和性设置绑定核心。调度器选择上，采用RR或FIFO这类RT调度，就像Adaptive AUTOSAR默认采用的那样。

理想星环在它的智驾OS中，提到了一种RMS单调速率调度的调度算法，并基于Linux内核的sched_ext实现为一个用户态的调度器，并设计了供应用使用的静态链接库。同时，给出了他们在QEMU与A78开发板上的测试结果。他们的说明文章，对如何在真实的嵌入式平台上进行编译部署与测试，说明并不详细。本文尝试在树莓派3B+的平台上搭建sched_ext开发与测试环境，并尝试运行RMS调度器并进行性能测试。

## 2. 什么是 sched_ext（scx）？

 sched_ext （简称  scx ）是 Linux 内核 6.12 版本正式引入的一项革命性技术，它允许开发者使用  eBPF（Extended Berkeley Packet Filter）来编写自定义的  CPU 调度器 。

在传统的 Linux 中，调度逻辑（如 CFS、EEVDF或者RT）被硬编码在内核源码中。如果不使用预置调度器，转而使用新的调度策略，必须修改内核代码并重启系统，这对于测试和部署来说门槛极高。 sched_ext 改变了这一游戏规则：它在内核调度框架中插入了一个 BPF 挂载层，使得用户可以在不重启内核的情况下，像加载插件一样动态地加载、卸载不同的调度算法。

scx的出现，不仅仅极大的便利了调度器的开发与调试。还可以针对特定场景，实现调度器的特化。这意味着，我们可以针对汽车实时应用场景，开发一种基于时间序列或者基于静态优先级的调度器，类似于RTOS中的调度器。星环的RMS调度器，就是为了实时性做了一种特化的调度器。

借助 eBPF 的验证机制，自定义调度器即使有 Bug 也不会导致整个系统崩溃（Panic），只会自动退回到系统默认调度器。

## 3. 如何搭建scx运行环境

我们在树莓派3B+上，部署开发测试环境。树莓派3B+的硬件规格如下：

* Broadcom BCM2837B0, Cortex-A53 (ARMv8) 64-bit SoC @ 1.4GHz
* 1GB LPDDR2 SDRAM
* 2.4GHz and 5GHz IEEE 802.11.b/g/n/ac wireless LAN, Bluetooth 4.2, BLE

操作系统是Raspberry Pi OS 64-bit，这是一个基于 Debian 13的Linux发行版，默认内核版本是 6.12。这满足了sched_ext的基本条件。

### 3.1 配置Linux内核

默认的BSP是不带实时补丁包与SCX支持的。因此，需要重新配置并编译内核以及编译安装scx进行测试。

配置内核，打开 SCHED_CLASS_EXT等相关配置项：

```bash
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

menuconfig 配置如下：

<img src="readme/Screenshot from 2025-11-14 13-42-16-17685544476771.png" alt="Screenshot from 2025-11-14 13-42-16" style="zoom:33%;" />

<img src="readme/Screenshot from 2025-11-14 13-43-06-17685544476782.png" alt="Screenshot from 2025-11-14 13-43-06" style="zoom:25%;" />

<img src="readme/Screenshot from 2025-11-14-13-42-44-17685544476793.png" alt="Screenshot from 2025-11-14-13-42-44" style="zoom:25%;" />

### 3.2 调度器优先级调整

sched_ext优先级比cfs要低，不修改内核调整的话必然不适合实时任务。因此需要手动调整调度器的优先级。注意，是调度器的优先级，不是调度优先级。

下面这段代码，定义了调度器在内核中的优先级顺序。内核的主调度函数（`schedule()`）在选择下一个运行进程时，会遍历这个数组，按照顺序检查每个调度类别是否有可运行的进程。

如果不改变调度器的优先级的话，sched_ext在fair的下面，其优先级会低于 CFS/EEVDF（完全公平调度器）所管理的**所有普通进程**。那么以实时性增强为目的的sched_ext调度器，将无法实现目标中的实时性。只要fair中还有可运行的进程，调度器都会去选定一个fair进程运行。

放在rt或者deadline之上，会有比较大的风险，需要谨慎处理。

最合理的方式，是将ext_sched调整到了rt和fair（EEVDF）之间的位置。如下：

```c
// include/asm-generic/vmlinux.lds.h
/*
 * The order of the sched class addresses are important, as they are
 * used to determine the order of the priority of each sched class in
 * relation to each other.
 */
#define SCHED_DATA				\
	STRUCT_ALIGN();				\
	__sched_class_highest = .;		\
	*(__stop_sched_class)			\
	*(__dl_sched_class)			\
	*(__rt_sched_class)			\
	*(__fair_sched_class)			\
	*(__ext_sched_class)			\
	*(__idle_sched_class)			\
	__sched_class_lowest = .;
// 修改后
#define SCHED_DATA				\
	STRUCT_ALIGN();				\
	__sched_class_highest = .;		\
	*(__stop_sched_class)			\
	*(__dl_sched_class)			\
	*(__rt_sched_class)			\
	*(__ext_sched_class)			\   // 上移
	*(__fair_sched_class)			\
	*(__idle_sched_class)			\
	__sched_class_lowest = .;
```

> 编译arm64内核时报错：
>
> 在Linux内核源代码中调整这些后，编译arm64的内核时，报错 `arch/arm64/kernel/vdso/vdso.lds:184 syntax error`
>
> 原因：
>
> 莫名其妙，注意不要在 `#define SCHED_DATA`的上方加注释，不然也会报这个错误。
>
> 在 `*(__ext_sched_class)` 之后的 `\` 后面，可能插入了：
>
> - **全角空格** (`\u3000`)
>
> - **非打印控制字符**
>
> - **其他非法 Unicode 字符**

同步的，也修改一下调度器优先级关系的检查（断言），确认内核调度器类的优先级顺序是正确的。如下：

```c
// kernel/sched/core.c
/* Make sure the linker didn't screw up */
#ifdef CONFIG_SMP
	BUG_ON(!sched_class_above(&stop_sched_class, &dl_sched_class));
#endif
	BUG_ON(!sched_class_above(&dl_sched_class, &rt_sched_class));
	BUG_ON(!sched_class_above(&rt_sched_class, &fair_sched_class));
	BUG_ON(!sched_class_above(&fair_sched_class, &idle_sched_class));
#ifdef CONFIG_SCHED_CLASS_EXT
	BUG_ON(!sched_class_above(&fair_sched_class, &ext_sched_class));
	BUG_ON(!sched_class_above(&ext_sched_class, &idle_sched_class));
#endif
// 修改后
// rt在ext的上面，ext在fair的上面
#ifdef CONFIG_SCHED_CLASS_EXT
	BUG_ON(!sched_class_above(&rt_sched_class, &ext_sched_class));
	BUG_ON(!sched_class_above(&ext_sched_class, &fair_sched_class));
#endif
```

### 3.3 打入实时补丁patch

如果需要打上实时补丁，可以从如下的地址下载，并打入patch。补丁包需与内核版本号一致：

```bash
# 内核代码
https://github.com/raspberrypi/linux
# 补丁包
https://mirrors.edge.kernel.org/pub/linux/kernel/projects/rt/6.12/
# aliyun mirrors
https://mirrors.aliyun.com/linux-kernel/projects/rt/6.12/?spm=a2c6h.25603864.0.0.51aab584MPNDiQ
```

最后交叉编译内核，并且部署内核、设备树文件。这一步可以参考树莓派的官方BSP开发文档。最终可以看到

非实时内核：

```bash
rpi@raspberrypi:~/opt/bin $ uname -a
Linux raspberrypi 6.12.54-v8-fzy-sched+ #2 SMP PREEMPT Fri Nov  7 11:30:16 CST 2025 aarch64 GNU/Linux
```

打入preempt-rt patch的实时内核：

```bash
rpi@raspberrypi:~/Downloads/scx/scx/build/scheds/c $ uname -a
Linux raspberrypi 6.12.57-rt14-v8-scx-rt #2 SMP PREEMPT_RT Fri Nov 14 13:51:45 CST 2025 aarch64 GNU/Linux
```

> 一开始选的6.12.54，结果发现RT补丁在6.12中，只有57，后面测试时非实时的内核，也用的57版本。囧...

### 3.4 编译scx仓库

scx官方仓库：https://github.com/sched-ext/scx。最终将其编译安装至~/opt/bin。如下：

```bash
rpi@raspberrypi:~/opt/bin $ ls
scx_central  scx_flatcg  scx_nest  scx_pair  scx_prev  scx_qmap  scx_sdt  scx_simple  scx_userland
```

如有bpftool的报错，可以安装bpftool，apt过慢的话，可以修改为国内镜像仓库。

```bash
sudo apt install bpftool
```

运行scx_simple测试。

```bash
rpi@raspberrypi:~/opt/bin $ sudo ./scx_simple 
libbpf: struct_ops simple_ops: member priv not found in kernel, skipping it as it's set to zero
local=4 global=0
local=46 global=24
local=78 global=40
^CEXIT: unregistered from user space
```

 `member priv not found in kernel`原因是当前的vmlinux是6.16的，而rpi上编译的内核是6.12，在6.16中加入了priv的内部使用变量。这不影响。

```c
// kernel/sched/ext.c
struct sched_ext_ops {
    // ...
    /* internal use only, must be NULL */
	void *priv;
}
```

至此，scx的开发编译测试环境完成。

## 4. 星环RMS调度复现与测试

### 4.1 编译RMS调度器

在scx官方库的scheds/c下，放置rms的相关文件。然后在Makefile中修改添加scx_rms。

星环的rms源码中，使用了过期的eBPF API，如果有这类报错，可以替换成新的API。

```bash
14_rpi_sched_ext/scx/scheds/c$ ls | grep rms
librms.c
librms.h
scx_rms.bpf.c
scx_rms.c

# Makefile
C_SCHEDS := scx_simple scx_qmap scx_central scx_userland scx_nest scx_flatcg scx_pair scx_prev scx_rms

# Build regular schedulers (no library dependency) - from source to output dir
$(addprefix $(OBJ_DIR)/,$(filter-out scx_rms,$(C_SCHEDS))): $(OBJ_DIR)/%: $(SRC_DIR)/%.c $(OBJ_DIR)/%.bpf.skel.h
	@echo "Building scheduler: $@"
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $< -o $@ $(LIBBPF_DEPS) $(THREAD_DEPS)
# 特殊规则：为scx_rms链接librms.so
$(OBJ_DIR)/scx_rms: $(SRC_DIR)/scx_rms.c $(OBJ_DIR)/scx_rms.bpf.skel.h $(LIBRMS_SO)
	@echo "Building scheduler with librms: $@"
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $< -o $@ $(LIBBPF_DEPS) $(THREAD_DEPS) -L$(OBJ_DIR) -lrms
	
clean:
	rm -f *.bpf.o *.bpf.skel.h *.l1o *.l2o *.l3o scx_simple scx_qmap scx_central scx_userland scx_nest scx_flatcg scx_pair scx_prev scx_sdt scx_rms
```

### 4.2 测试RMS调度器

参照星环文档。

```c
// 示例程序 rms_task.c
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

编译：

```bash
gcc -o rms_task rms_task.c -L. -lrms -lm -lbpf -I. -Wl,-rpath,.
```

运行结果如下：

![image-20251113145430485](scx在异构计算中尝试/image-20251113145430485.png)

## 5. 控制算法调度周期稳定性测试

尝试以10ms调度周期的MPC算法为例，探索在探索Linux是否打上实时补丁、高低负载、传统调度器与基于sched_ext的用户态调度器RMS（来自RTOS）条件下的运行周期抖动与调度延迟。

MPC算法是非确定性计算耗时的算法。它是为了尝试替代LQR所做的一个尝试。它不能保证，在确定的时间，一定能计算完成。测试它在10ms下，到底能不能稳定计算完。超限的概率有多少。这也是测试的一个目的。

先说个人初步分析的结论。不一定准确，姑且一看。

结论1：Linux上的传统CFS调度器，不适合该类应用，运行1000次，会有930次超出80us，超出率：93%
结论2：RT调度器，无负载平均周期抖动15us，带负载平均周期抖动32us，80us周期抖动超出率：5.4%，适合软实时任务，AP默认采用该类调度
结论3：源自RTOS的RMS调度器，微小抖动极佳，无负载平均周期抖动2us，带负载平均周期抖动9us，但80us超出率：9.6%，有潜力，仍需继续开发
结论4：MPC大部分时间能够稳定在10ms周期内运行，但在部分计算中，会大幅度超出，最大可能有200ms+

树莓派3B+上现在有3个内核：

- kernel8_scx_no_rt.img
  - 内核版本：6_12_54
  - 默认调度器优先级顺序，sched_ext在fair的后面
  - 没有实时补丁
- kernel_6_12_57_scx_no_rt.img
  - 调整了调度器的优先级顺序，sched_ext在fair的前面
  - 没有实时补丁
- kernel_6_12_57_scx_rt.img
  - 调整了调度器顺序,，sched_ext在fair的前面
  - 实时补丁

kernel8_scx_no_rt.img这个内核，是之前忘记调整调度器优先级的一个内核。sched_ext在fail的后面。从结果上看，RMS调度的抖动极高。可以看到，它确实不适合偏向实时向的场景。

参数说明：使用亲和性绑定程序在cpu3上运行，周期10ms，计算预算5ms。

```bash
rpi@raspberrypi:~/Downloads/rt_test/build $ sudo ./task_sched_test -m rms -c 3
[Mode] RMS (via librms)
cpu3 U: 0.000000 N: 0
cpu3 U: 0.500000 N: 1
rms init cpu3 pid 1876
sched_setaffinity 3 ok
Starting task loop on CPU 3 Period: 10000us Budget: 5000us
Statistics (last 1000 cycles): Avg Jitter: 79372.815 us, Max Jitter: 119369.215 us, >80us: 1000
Statistics (last 1000 cycles): Avg Jitter: 225703.106 us, Max Jitter: 329503.795 us, >80us: 1000
Statistics (last 1000 cycles): Avg Jitter: 489573.191 us, Max Jitter: 639367.176 us, >80us: 1000
Statistics (last 1000 cycles): Avg Jitter: 743803.006 us, Max Jitter: 809502.850 us, >80us: 1000
....
^Ccpu3 U: 0.500000 N: 1
cpu3 U: 0.000000 N: 0
RMS resources cleaned up.
Exiting gracefully.
```

整个测试方案如下：

```bash
├── CMakeLists.txt
├── include
│   ├── librms.h
│   ├── mpc_task.hpp
│   └── sched_strategies.hpp
├── lib
│   ├── librms.so
│   └── readme.txt
├── readme.md
└── src
    ├── main.cpp
    ├── mpc_task.cpp
    └── sched_strategies.cpp
```

mpc_task是算法源码API接口的适配层，MPC的算法，暂时无法对外开源。

主要测试2个抖动，平均周期抖动与最大周期抖动。

周期抖动：**实际运行周期**（相邻两次任务激活的时间差）与**理论预期周期**（`period_ns`）之间的偏差。

平均周期抖动定义为：在一段观察时间内，所有任务循环产生的周期抖动**绝对值**的算术平均值。它反映了系统调度的整体稳定性。

最大周期抖动定义为：在整个运行过程中，实际周期偏离理论周期的**最极端情况**。包括正向最大偏差（延迟唤醒）和负向最大偏差（提前唤醒）。

采用streeng进行系统加压。

### 5.1 非实时内核

CFS(EEVDF)

```bash
# no stress
Final Statistics:
  Total Cycles: 1001
  Avg Abs Jitter (All): 3541.739 us
  Avg Abs Jitter (<=80us): 69.311 us
  >80us Count: 85
  Top 10 Max +Jitter: [152322.656, 101720.990, 101587.448, 101558.646, 101444.896, 101436.146, 101370.052, 101338.333, 101292.396, 101284.687] us
  Top 10 Max -Jitter: [] us
# stress-ng -c 2
Final Statistics:
  Total Cycles: 1001
  Avg Abs Jitter (All): 4188.274 us
  Avg Abs Jitter (<=80us): 73.038 usls
  >80us Count: 578
  Top 10 Max +Jitter: [121243.802, 121218.541, 121020.521, 120784.843, 120755.521, 120698.750, 120612.552, 120602.291, 120573.802, 120478.437] us
  Top 10 Max -Jitter: [] us
```

RT

```bash
# no stress
Final Statistics:
  Total Cycles: 1001
  Avg Abs Jitter (All): 3485.799 us
  Avg Abs Jitter (<=80us): 14.988 us
  >80us Count: 48
  Top 10 Max +Jitter: [152084.740, 101697.448, 101560.937, 101543.490, 101496.458, 101362.657, 101329.583, 101309.635, 101276.302, 101247.448] us
  Top 10 Max -Jitter: [] us
# stess-ng -c 2
Final Statistics:
  Total Cycles: 1001
  Avg Abs Jitter (All): 4116.751 us
  Avg Abs Jitter (<=80us): 32.546 us
  >80us Count: 54
  Top 10 Max +Jitter: [120865.833, 120568.386, 120518.438, 120405.417, 120396.250, 120395.000, 120393.333, 120348.489, 120292.188, 120248.958] us
  Top 10 Max -Jitter: [] us
```

SCX_RMS

```bash
# no stress
# P10B5
Final Statistics:
  Total Cycles: 1001
  Avg Abs Jitter (All): 31791.577 us
  Avg Abs Jitter (<=80us): 2.307 us
  >80us Count: 198
  Top 10 Max +Jitter: [1239997.656, 1219996.041, 1209990.052, 1199999.374, 1189998.801, 1160001.562, 1109999.687, 1099997.864, 1090000.885, 1079999.479] us
  Top 10 Max -Jitter: [-76.198, -60.989, -58.750, -47.187, -40.157, -40.000, -32.031, -30.833, -29.375, -28.386] us
# P20B10
Final Statistics:
  Total Cycles: 1001
  Avg Abs Jitter (All): 25924.013 us
  Avg Abs Jitter (<=80us): 4.072 us
  >80us Count: 55
  Top 10 Max +Jitter: [760003.750, 760001.042, 759999.895, 759999.792, 759999.062, 759997.865, 759997.708, 759997.708, 759997.187, 759988.073] us
  Top 10 Max -Jitter: [-195.260, -89.635, -41.041, -39.688, -39.427, -39.010, -37.187, -37.136, -37.031, -35.365] us

# stress-ng -c 2
# P10B5
Final Statistics:
  Total Cycles: 1001
  Avg Abs Jitter (All): 24828.490 us
  Avg Abs Jitter (<=80us): 9.156 us
  >80us Count: 96
  Top 10 Max +Jitter: [700019.114, 700017.917, 700000.833, 699990.156, 699982.916, 690017.188, 690012.812, 690012.447, 690003.542, 690001.094] us
  Top 10 Max -Jitter: [-82.447, -65.520, -60.261, -60.000, -59.844, -56.354, -56.094, -55.990, -53.073, -49.427] us
# P20B10
Final Statistics:
  Total Cycles: 1001
  Avg Abs Jitter (All): 76809.749 us
  Avg Abs Jitter (<=80us): 12.041 us
  >80us Count: 181
  Top 10 Max +Jitter: [2259944.114, 2179996.353, 2139944.582, 2120019.165, 2120008.593, 2100012.499, 2099993.228, 2080010.781, 2079975.260, 2060060.312] us
  Top 10 Max -Jitter: [-59.739, -54.739, -54.427, -54.323, -53.907, -52.552, -51.302, -50.833, -48.959, -47.864] us
```

Deadline

```bash
# no stress
Final Statistics:
  Total Cycles: 1001
  Avg Abs Jitter (All): 7672.730 us
  Avg Abs Jitter (<=80us): 2.890 us
  >80us Count: 59
  Top 10 Max +Jitter: [220054.062, 220001.250, 220001.094, 220000.833, 220000.573, 220000.313, 220000.312, 220000.312, 220000.260, 220000.208] us
  Top 10 Max -Jitter: [-48.386, -42.553, -40.104, -34.115, -31.875, -28.646, -27.396, -27.135, -25.469, -24.062] us
# stress-ng -c 2
Final Statistics:
  Total Cycles: 1001
  Avg Abs Jitter (All): 8906.842 us
  Avg Abs Jitter (<=80us): 7.309 us
  >80us Count: 63
  Top 10 Max +Jitter: [260056.823, 260044.427, 260031.458, 259993.229, 259979.739, 259976.094, 259975.000, 259963.490, 250041.458, 250036.614] us
  Top 10 Max -Jitter: [-55.521, -45.052, -44.218, -37.865, -37.500, -36.146, -35.573, -34.739, -32.656, -32.343] us

```

### 5.2 实时内核

CFS(EEVDF)

```bash
# no stress
Final Statistics:
  Total Cycles: 1001
  Avg Abs Jitter (All): 3561.942 us
  Avg Abs Jitter (<=80us): 76.097 us
  >80us Count: 564
  Top 10 Max +Jitter: [116783.594, 102836.770, 102818.542, 102716.979, 102709.740, 102706.354, 102493.854, 102463.854, 102440.729, 102358.073] us
  Top 10 Max -Jitter: [] us
# stress-ng -c 2
Final Statistics:
  Total Cycles: 1001
  Avg Abs Jitter (All): 5799.442 us
  Avg Abs Jitter (<=80us): 77.676 us
  >80us Count: 930
  Top 10 Max +Jitter: [270189.531, 258625.261, 256492.917, 256347.187, 255874.948, 245366.615, 238642.656, 224193.177, 188886.302, 184109.270] us
  Top 10 Max -Jitter: [] us
```

RT

```bash
# no stress
Final Statistics:
  Total Cycles: 1001
  Avg Abs Jitter (All): 3513.672 us
  Avg Abs Jitter (<=80us): 11.318 us
  >80us Count: 48
  Top 10 Max +Jitter: [140766.771, 104153.333, 104028.594, 104010.990, 103659.948, 103610.416, 103579.584, 102878.750, 102429.687, 102109.270] us
  Top 10 Max -Jitter: [] us
# stess-ng -c 2
Final Statistics:
  Total Cycles: 1001
  Avg Abs Jitter (All): 4950.775 us
  Avg Abs Jitter (<=80us): 31.537 us
  >80us Count: 63
  Top 10 Max +Jitter: [256023.854, 255834.635, 255553.490, 247465.885, 243949.479, 222784.531, 189779.584, 181051.459, 179470.729, 105178.802] us
  Top 10 Max -Jitter: [] us
```

Deadline

```bash
# no stress
Final Statistics:
  Total Cycles: 1001
  Avg Abs Jitter (All): 7752.093 us
  Avg Abs Jitter (<=80us): 1.092 us
  >80us Count: 60
  Top 10 Max +Jitter: [220015.261, 220000.782, 220000.729, 220000.677, 220000.677, 220000.417, 220000.312, 220000.260, 220000.105, 220000.104] us
  Top 10 Max -Jitter: [-63.438, -26.615, -23.021, -7.812, -6.094, -4.532, -4.531, -4.531, -4.010, -3.958] us
# stress-ng -c 2
Final Statistics:
  Total Cycles: 1001
  Avg Abs Jitter (All): 12135.116 us
  Avg Abs Jitter (<=80us): 5.786 us
  >80us Count: 113
  Top 10 Max +Jitter: [530043.698, 530025.156, 530013.594, 530008.854, 530004.531, 530002.604, 529994.843, 529974.635, 509998.020, 440003.385] us
  Top 10 Max -Jitter: [-46.927, -40.521, -31.927, -29.375, -27.865, -25.573, -24.792, -24.791, -24.219, -23.593] us
```

测试截图记录如下：

![image-20251128154110353](scx在异构计算中尝试/image-20251128154110353.png)

no stress

![image-20251128155229487](scx在异构计算中尝试/image-20251128155229487.png)

## 附录：RMS实时调度器的问题汇总

#### 1. librms的功能是什么？

将RMS的操作封装成库（.a或者.so），对外仅提供三个API函数，用户无需操心RMS的各种相关事项，例如调度参数的传递、亲和性设置、EXT调度策略、任务可调度检测等。只需要提供预算时间、周期以及绑定核心，三个参数，即可。

极大的降低了用户的使用难度。

#### 2. librms中有两个锁，`rms_cpu_<CPU>.lock`，`rms_cpu_exit_<CPU>.lock`，为什么要这两个锁，作用是什么？

内核bpf.c与应用空间程序的交互，有2个map，分别是task_attr_hmap与rms_entry_map， task_attr_hmap是哈希表，描述task，rms_entry_map里面是CPU的利用率和task数，这个不是系统的CPU利用率，而是RMS算法中定义的，有一套专门的算法来计算。这两个map，是通过bpf_map__pin，放在了文件系统中，即 `/sys/fs/bpf/rms*`。文件系统中的文件，是可以所有进程都访问的。这就存在数据一致性问题。因此需要两个锁，而且是flock中的独占锁，来锁定。
这两个锁的作用是确保在用户空间对eBPF映射中存储的**CPU利用率和任务数量**进行操作时的**数据一致性和线程安全**，防止了多个进程或线程同时修改同一CPU的调度参数，从而维护了RMS调度器的可调度性保证。

而且，是为每个CPU都创建与维护了独立的锁文件的。

> flock是建议性锁，不具备强制性的，防君子不防小人。虽然A进程用flock锁住了文件，不讲武德的B进程直接修改文件，内核并不阻止。
>
> 这里，所有用RMS的入口都是一个，且是以库的形式提供，所以都会经过锁这一段。

#### 3. 为什么不用Linux内核的DL调度，而是使用sched_ext实现的实时用户态调度器（RMS）？
sched_ext机制的RMS调度在任务优先级、负载分配等方面由BPF程序控制，但实际抢占能力仍依赖Linux内核当前的抢占模式，以及PREEMPT_RT补丁的支持情况。对比sched_deadline，RMS在我们的应用场景中，主要在**任务可预测性、对周期任务的处理顺序、以及CPU占用上限控制方面**体现优势。此外在特定模块中，可通过RMS的统一配置与监控，简化调度策略调整。

> 上述是星环开发人员的回答

#### 4. bpf_timer在PREEMPT-RT下，不能再使用，原因是什么？应该如何处理？

   实时内核的bpf_timer，因为锁问题，不允许使用。

#### 5. PREEMPT-RT实时内核与非实时内核，RMS的性能分别如何？

#### 6. RMS的固定优先级是怎么做的？

#### 7. `/sys/fs/bpf`下的rms_entry_map、task_attr_hmap，它们的作用是什么？

![image-20251126105449047](scx在异构计算中尝试/image-20251126105449047.png)



#### 8. Linux中如何保证定时周期可预测？
将Linux内核转换为完全可抢占模式（PREEMPT_RT），并采用高精度定时器（hrtimer）在内核态驱动任务周期。这套方案将Linux系统的行为从“尽力而为”转变为提供高度确定性的时限保障，从而构建了一个从定时触发到任务执行的、高度可预测的调度闭环，使得基于sched_ext实现的RMS调度器能够为周期性任务提供可预测的定时保障。
> 上述是星环开发人员的回答

> 星环RMS调度的介绍可以搜索：rate_monotonic_scheduling_real_time_practice
