# Xiaozhi ESP32 — Persona Fork

[English](#english) | [中文](#中文)

A fork of the open-source **XiaoZhi ESP32** firmware, focused on a small
ESP32-S3 desk terminal: board configuration, paper-and-ink UI, expression
states, and a full-screen media layer. The upstream firmware, board matrix,
protocols, and documentation remain the foundation of this project.

---

## English

### What this fork adds

- Board configuration for an ESP32-S3 3.5-inch desk terminal.
- Persona UI v2: A/B/C information views, six expression states, an idle dino
  strip, and a 480×320 dino game view.
- Full-screen media preview for images and screenshots, with tap or timeout
  dismissal.
- Small integration fixes kept close to the board layer.

It is a firmware fork, not a replacement for upstream. Upstream supports many
more chips, boards, transports, and languages.

### Build

Requirements: ESP-IDF `release/v6.1` (minimum v6.0.1), Python 3, and the
repository's build script.

```bash
source /path/to/esp-idf/export.sh
python3 scripts/build.py <board-directory> --name <variant-name> --language zh-CN
```

The build produces `build/merged-binary.bin` (full flash image),
`build/xiaozhi.bin` (application image), and `build/generated_assets.bin`.
Release assets and SHA-256 manifests are attached to the GitHub releases.

### Releases

Each release is cut from this fork's `main` and pinned by tag. See the
[releases](https://github.com/yes999zc/xiaozhi-esp32/releases) page for the
matching firmware images and manifests.

### Acknowledgements

This fork would not exist without:

- [78/xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) — the upstream
  firmware and all of its maintainers and contributors.
- [ESP-IDF](https://github.com/espressif/esp-idf), [LVGL](https://lvgl.io/),
  [ESP-SR](https://github.com/espressif/esp-sr), and the other open-source
  components used by the firmware.
- The XiaoZhi community for protocol documentation, board examples, and
  testing feedback.

The original upstream README and documentation are preserved in
[README_UPSTREAM.md](README_UPSTREAM.md); upstream license and attribution
remain in effect.

### License

MIT. See [LICENSE](LICENSE) and the upstream license notices.

---

## 中文

### 项目简介

这是开源 **小智 ESP32** 固件的一个分支，聚焦一块 ESP32-S3 桌面终端：板级配置、
纸墨风格 UI、表情状态和全屏媒体层。上游固件、板卡矩阵、协议和文档仍然是本项目
的基础。

### 本分支增加的内容

- ESP32-S3 3.5 英寸桌面终端的板级配置。
- 人格化 UI v2：A/B/C 信息视图、六种表情状态、空闲恐龙条、480×320 恐龙游戏视图。
- 图片和截图的全屏媒体预览，支持点击或超时关闭。
- 少量贴近板级层的集成修复。

这是固件分支，不是对上游的替代。上游支持更多芯片、板卡、传输方式和语言。

### 构建

依赖：ESP-IDF `release/v6.1`（最低 v6.0.1）、Python 3，以及仓库自带的构建脚本。

```bash
source /path/to/esp-idf/export.sh
python3 scripts/build.py <board-directory> --name <variant-name> --language zh-CN
```

产物包括 `build/merged-binary.bin`（完整刷机镜像）、`build/xiaozhi.bin`
（应用镜像）和 `build/generated_assets.bin`。发布时会把镜像和 SHA-256 清单附到
GitHub Release。

### 发布

每次发布都从本分支 `main` 切出，并用 tag 固定。镜像和清单见
[Releases](https://github.com/yes999zc/xiaozhi-esp32/releases)。

### 致谢

本分支建立在以下开源工作之上：

- [78/xiaozhi-esp32](https://github.com/78/xiaozhi-esp32)：上游固件及其所有
  维护者和贡献者。
- [ESP-IDF](https://github.com/espressif/esp-idf)、[LVGL](https://lvgl.io/)、
  [ESP-SR](https://github.com/espressif/esp-sr)，以及固件使用的其他开源组件。
- 小智社区：协议文档、板卡示例和测试反馈。

上游 README 和文档保留在 [README_UPSTREAM.md](README_UPSTREAM.md)，上游许可证
与署名继续有效。

### 许可证

MIT，见 [LICENSE](LICENSE) 及上游许可证声明。
