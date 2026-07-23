# Autonomy Executor Lab

[English](README.md) | [简体中文](README.zh-CN.md)

一个确定性的 C++17 回调调度与轨迹诊断实验室，用于比较同一控制工作负载在 FIFO、固定优先级、最早截止期优先和 latest-only 策略下的实时性、新鲜度和过载行为。

## 核心能力

- 非抢占式回调执行器与四种可比较策略；
- 每个作业的等待、响应、数据年龄、结果和诊断；
- 对被替代样本的显式可审计记录；
- 绑定完整轨迹字段的版本化 SHA-256 指纹；
- 严格 CSV 输入与稳定 JSON/CSV 输出；
- 独立库、CLI、CMake 安装目标及安装后消费测试。

## 构建与运行

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
.\build\ael-sim.exe examples\overload.csv --policy latest-only --trace trace.csv
python tools\compare_policies.py .\build\ael-sim.exe examples\overload.csv
```

项目需要 CMake、Ninja 和 C++17 编译器，不依赖 ROS、DDS、GPU、网络或硬件。`--strict` 可让截止期或新鲜度失败直接阻断 CI。

## 工程边界

本项目是可复现的执行器策略实验工具，不是实时操作系统或通用可调度性证明器。模型为非抢占式，结果必须结合目标平台和完整运行时进一步验证。

## 协作

史浩轩负责总体设计与主要实现；刘泽康参与安全边界和集成接口核验。职责说明见 [CONTRIBUTORS.md](CONTRIBUTORS.md)。

采用 [MIT License](LICENSE)。
