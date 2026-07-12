# AGENTS.md

本文件为 AI 代理（如 Claude Code）提供项目导航信息。

## 项目概述

Zygisk 模块，通过直接修改 ART 运行时的 `ArtMethod` 结构体，将 QQ 的 `com.tencent.mobileqq.text.EmotcationConstants` 类中 emoji 查找方法设为 native，使方法统一返回 `-1`，强制 QQ 回退到系统 emoji 字体显示。

## 技术栈

- **语言**：C++17
- **框架**：Zygisk API v4
- **依赖**：仅 Android 系统库（`log`、`dl`），无第三方依赖
- **构建**：CMake + Android NDK
- **CI**：GitHub Actions

## 核心技术

### ArtMethod 直接修改（非框架 hook）

不使用 LSPlant、Dobby、Xposed 等 hook 框架。原因：QQ 会检测这些框架的痕迹。

具体做法：
1. 将目标方法的 `access_flags_` 设置 `kAccNative` 标志位
2. 将 `entry_point_from_compiled_code_` 设置为 `art_quick_generic_jni_trampoline`（libart.so 标准 JNI 桥接函数）
3. 将 `data_`（JNI 入口点）设置为我们的 native 函数指针（返回 `-1`）

关键：`entry_point` 指向 libart.so 中的标准函数（非匿名 RWX 内存），不会触发 QQ 对 hook 框架的检测。

### ArtMethod 偏移运行时确定

通过 `java.lang.Object` 的相邻方法指针差值确定 `sizeof(ArtMethod)`，进而推算 `data_` 和 `entry_point` 偏移。`access_flags_` 固定在偏移 4。

### 方法匹配

按签名匹配（返回 `int`、参数全 `int`、参数个数 1 或 2），不依赖可能被混淆的方法名。

### Hook 时机

在 `postAppSpecialize` 中启动轮询线程，通过 QQ 的 ClassLoader 等待 `EmotcationConstants` 自然加载，加载后立即 hook。

## 架构

```
native/
├── zgisk.hpp          # Zygisk API v4 头文件（官方，不得修改）
├── CMakeLists.txt      # CMake 构建配置
└── src/
    └── module.cpp      # 全部核心逻辑

module/
├── module.prop         # 模块元数据（含 updateJson OTA 地址）
└── customize.sh        # 安装时输出信息

build.sh                # 一键构建：CMake 双 ABI 编译 → 模块打包
update.json             # OTA 元数据（手动维护）
.github/workflows/
└── build.yml           # CI：tag push 自动构建并发布 Release
```

## 构建

```bash
export ANDROID_NDK_HOME=/path/to/ndk
./build.sh
```

CI：推送 `v*` tag 触发 GitHub Actions，自动构建并创建 Release。

## OTA 更新机制

`module.prop` 中的 `updateJson` 指向仓库中的 `update.json`（GitHub raw）。root 管理器定期请求此 JSON 比对 `versionCode`。发布新版本后需手动更新 `update.json` 并提交到 main 分支。不使用 Action 自动更新此 JSON。

## 关键约定

- **不使用** LSPosed / Xposed / LSPlant 等 hook 框架（QQ 检测）
- **不使用** Dobby / inline hook（减少检测面）
- 方法查找使用签名匹配（不依赖方法名）
- 功能开关与模块开关联动（无独立配置界面）
- 目标进程仅 `com.tencent.mobileqq`
- `entry_point` 指向 libart.so 标准函数，不是匿名 RWX 内存
- 非 QQ 进程调用 `setOption(DLCLOSE_MODULE_LIBRARY)` 卸载 .so
