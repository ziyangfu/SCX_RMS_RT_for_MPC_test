cmake_minimum_required(VERSION 3.14)
project(task_sched_test)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 设置包含目录
include_directories(${CMAKE_SOURCE_DIR}/include)
include_directories(${CMAKE_SOURCE_DIR}/src/MPC_V13)
include_directories(${CMAKE_SOURCE_DIR}/src/MPC_V13/Algorithm)
include_directories(${CMAKE_SOURCE_DIR}/src/MPC_V13/AlgorithmAdaptor)
include_directories(${CMAKE_SOURCE_DIR}/src/MPC_V13/Algorithm/osqp_code/include) # Assuming OSQP headers are here

# 查找本地的 librms 库
# 假设 librms.so 位于项目根目录下的 lib/ 文件夹
find_library(RMS_LIB rms PATHS ${CMAKE_SOURCE_DIR}/lib NO_DEFAULT_PATH)

# 收集 MPC_V13 下的所有源文件
# 排除 build 目录下的文件 (CMake 临时文件) 和 example.c
file(GLOB_RECURSE MPC_SOURCES 
    src/MPC_V13/Algorithm/*.cpp
    src/MPC_V13/Algorithm/*.c
    src/MPC_V13/AlgorithmAdaptor/*.cpp
    src/MPC_V13/AlgorithmAdaptor/*.c
)

# 过滤掉不需要的文件
list(FILTER MPC_SOURCES EXCLUDE REGEX ".*/build/.*")
list(FILTER MPC_SOURCES EXCLUDE REGEX ".*/example.c")
list(FILTER MPC_SOURCES EXCLUDE REGEX ".*/main.cpp") # 排除 MPC_V13/main.cpp，防止多重 main 定义

# 添加可执行文件
add_executable(task_sched_test
    src/main.cpp
    src/sched_strategies.cpp
    src/mpc_task.cpp
    ${MPC_SOURCES}
)

# 链接库
# pthread 是必须的，rt 用于高精度计时，rms 为你的自定义库
# 查找 libbpf
find_library(BPF_LIB bpf)
if(NOT BPF_LIB)
    message(STATUS "libbpf not found explicitly, trying -lbpf")
    set(BPF_LIB bpf)
endif()

target_link_libraries(task_sched_test PRIVATE pthread rt ${RMS_LIB} ${BPF_LIB})

# 设置 RPATH，使得程序运行时能找到同目录下的 librms.so
# 或者 lib 目录下的 .so
set_target_properties(task_sched_test PROPERTIES
    BUILD_RPATH "${CMAKE_SOURCE_DIR}/lib"
    INSTALL_RPATH "${CMAKE_SOURCE_DIR}/lib"
)
