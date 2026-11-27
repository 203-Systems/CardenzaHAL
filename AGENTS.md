# Cardenza HAL

若本仓位于 203 工作区，先读取上级目录的 `../AGENTS.md`，再按本文件和仓内维护文档处理项目细节。

本仓负责鸣片通用硬件能力、设备共享设置、固件模板与模拟器。维护 HAL 时先读原 `AGENT.md` 中相应章节。使用 HAL 开发固件时按 `docs/FIRMWARE-PROJECT.md`。[维护规则](AGENT.md) · [固件项目规范](docs/FIRMWARE-PROJECT.md)

- API 保持通用且尽量暴露 ESP-IDF 原生类型。初始化从 `cardenzaHAL.cpp` 跟到各驱动，保留 `InitReport` 对各子系统的结果。设备应用的界面、格式与音频处理留在应用层。
- 显示改动先读[显示开发指南](docs/DISPLAY.zh-CN.md)，对照 `display/changed_regions.h` 的脏区计算与 `display/presenter.h` 的节奏控制。USB 开发改动对照[协议](docs/usb-dev.md)和 `tools/web/screen-stream.js` 的主机解码。
- `web/` 是独立模拟后端。新增固件侧 ESP-IDF 调用时检查模拟支持及明确的能力边界，保持真机源文件与 Web 后端的构建边界。
- 改共享分区、HOME、设置、设备在线或 USB 开发协议时核对 Launcher、CLI、社区和固件消费者。固件模板、发布 action、原规范和 CLI 中的规范副本由各自的生成／同步入口维护。
- 主机测试用 `cmake -S tests -B build-tests -DCMAKE_BUILD_TYPE=Debug`、`cmake --build build-tests` 和 `ctest --test-dir build-tests --output-on-failure`。纯逻辑测试和 `tests/hal_faults/` 的驱动失败测试覆盖不同层。涉及设备与网页后端时，再选对应示例及测试。[主机 CI](.github/workflows/host-tests.yml) · [开发指南](docs/developing.zh-CN.md)
