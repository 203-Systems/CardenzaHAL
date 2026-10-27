# Cardenza HAL

[English](README.md) | [简体中文](README.zh-CN.md)

ESP-IDF hardware drivers for Cardenza (ESP32-S3): audio, display, keyboard and per-key LEDs, SD storage, PDM microphone, IR, Func button, and I2C.

## Integration

```cmake
add_subdirectory(path/to/cardenza)
target_link_libraries(your_target PRIVATE CardenzaHAL)
```

Include headers relative to the HAL root. ESP-IDF components compiling the sources directly should add this root to `INCLUDE_DIRS`.

## Launcher Apps

See the [203-launcher application registration guide](https://github.com/203-Systems/203-launcher/blob/main/docs/REGISTERED_SOFTWARE.md#developer-workflow) for metadata and registration commands.
