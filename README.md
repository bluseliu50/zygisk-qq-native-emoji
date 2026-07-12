# QQ Native Emoji

通过 Zygisk 将 QQ 聊天气泡中的内置 emoji 替换为系统原生 emoji 字体显示。

## 效果

安装后，QQ 对话气泡中的 emoji 将统一使用系统 emoji 字体渲染，不再显示 QQ 内置 emoji。

## 环境要求

- 已 root 的 Android 设备
- 支持 Zygisk 的 root 框架
- Android 8.0（API 26）及以上
- QQ（仅作用于 `com.tencent.mobileqq` 进程）

## 安装

1. 下载最新版模块 zip（从 [Releases](../../releases) 页面）
2. 在 root 管理器中刷入该 zip
3. 重启设备
4. 打开 QQ，发送 emoji 验证效果

## 使用

安装即生效，无需额外配置。**模块开关即功能开关**——在 root 管理器中禁用本模块并重启即可恢复 QQ 原有 emoji。

## 更新

在 root 管理器中检查更新即可。本模块通过 `updateJson` 机制实现 OTA，有新版本时会自动提示。

## 构建

### 前置要求

- Android NDK r25+
- CMake 3.18+

### 步骤

```bash
export ANDROID_NDK_HOME=/path/to/ndk
chmod +x build.sh
./build.sh
```

生成的 zip 位于 `build/` 目录。

也可通过 GitHub Actions 自动构建：推送 `v*` 格式的 tag 即可触发。

## 技术原理

本模块通过 Zygisk 注入 QQ 进程，直接修改 ART 运行时的 `ArtMethod` 结构体，将 QQ 的 emoji 查找方法设为 native 方法并使其统一返回 `-1`，从而强制 QQ 回退到系统 emoji 字体。

此方案不依赖任何已知的 hook 框架（如 LSPosed/Xposed），不会被 QQ 的框架检测机制识别。

## 免责声明

- 本项目仅供个人学习和研究使用
- 本项目不隶属于腾讯公司，也未获得腾讯公司的授权或认可
- "QQ" 和 "腾讯" 是腾讯公司的商标
- 使用本模块可能导致 QQ 账号风险，用户需自行承担一切后果
- 本项目不保证任何明示或暗示的功能适用性
- 如果本项目侵犯了您的合法权益，请联系作者删除
