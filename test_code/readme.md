4.2. 运行测试
注意：RMS 和 DL (Deadline) 调度器通常需要 root 权限。运行前请确保 scx_rms 调度器程序已经在后台运行（针对 RMS 模式）。

运行 RMS 模式 (对应文档中 RMS+BPF Timer):

`sudo ./task_sched_test -m rms -c 3`
运行 RT 模式 (对应文档中 RT+POSIX Timer):

`sudo ./task_sched_test -m rt -c 3`
运行 CFS 模式 (对应文档中 CFS+POSIX Timer):

不需要 sudo，除非为了绑定 CPU

`./task_sched_test -m cfs -c 3`
运行 Deadline 模式 (对应文档中 DL):

`sudo ./task_sched_test -m dl -c 3`