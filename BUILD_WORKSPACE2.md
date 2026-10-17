# workspace2 编译说明

本目录从 `G:/0_TEMP/6_eda/workspace` 复制了当前布线器的源码、头文件、构建配置、测试、测试样例及运行配置。复制文件与来源逐个核对 SHA256，清单见 `SOURCE_MANIFEST.json`。

## 文件位置

- `src/`、`include/`：C++ 源码和头文件，比赛程序入口为 `src/contest_main.cpp`。
- `CMakeLists.txt`：CMake 构建配置。
- `tests/`：测试代码；`tests/fixtures/sample001/` 包含自测所需的 LEF、DEF、热图。
- `benchmark_thermal.py`：三个正式 case 的 balanced 参数配置。
- `CONTEST.md`：算法、参数和此前正式评分结果的说明。
- `build_contest.sh`、`run_contest.sh`：原项目的 Linux 构建与运行脚本。

编译需要支持 C++17 的编译器和 CMake；C++ 代码没有第三方库依赖。本机使用 MinGW g++ 12.2.0 和 CMake。Python 仅用于部分测试和运行配置脚本。

## Windows PowerShell 编译与测试

```powershell
Set-Location G:\0_TEMP\6_eda\workspace2
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release -DROUTER_SAMPLE_DIR=G:/0_TEMP/6_eda/workspace2/tests/fixtures/sample001
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
.\build\contest_router.exe --version
```

程序位于 `G:/0_TEMP/6_eda/workspace2/build/contest_router.exe`。本目录已使用上述配置独立完成 Release 编译，并通过全部 7 项 CTest 测试。

`ROUTER_SAMPLE_DIR` 显式指向本目录中的样例，运行测试无需依赖原 workspace。若移动本目录，请相应修改此参数，并使用新的构建目录重新配置。

正式 case 的大输入文件、评分器、历史输出和原构建缓存不属于编译依赖，仍保存在原位置。
