MPC算法暂时无法开源。

将MPC_V13文件夹以及mpc_task.cpp文件放在src文件夹下

在 ARM64 Linux环境下，例如树莓派。

```bash
mkdir build
cd build
cmake ..
make
```

4.2. 运行测试
注意：RMS 和 DL (Deadline) 调度器通常需要 root 权限。运行前请确保 scx_rms 调度器程序已经在后台运行（针对 RMS 模式）。

```bash
rpi@raspberrypi:~/Downloads/rt_test/build $ sudo ./task_sched_test
Usage: ./task_sched_test -m <mode> [options]
Modes:
  rms   : Rate-Monotonic (via sched_ext)
  rt    : Real-Time (SCHED_FIFO)
  cfs   : Normal (SCHED_OTHER)
  dl    : Deadline (SCHED_DEADLINE)
Options:
  -c <cpu_id> : Bind to CPU (default 3)
  -p <ms>     : Period in ms (default 10)
  -b <ms>     : Budget in ms (default 5)
  -n <count>  : Number of cycles to run
  -t <min>    : Duration in minutes to run
Note: -n and -t are mutually exclusive.
```

运行 RMS 模式 :

`sudo ./task_sched_test -m rms -c 3 -p 10 -b 5 -n 1001`
运行 RT 模式:

`sudo ./task_sched_test -m rt -c 3 -p 10 -b 5 -n 1001`
运行 CFS 模式:

不需要 sudo，除非为了绑定 CPU

`./task_sched_test -m cfs -c 3 -p 10 -b 5 -n 1001`
运行 Deadline 调度

`sudo ./task_sched_test -m dl -c 3 -p 10 -b 5 -n 1001`