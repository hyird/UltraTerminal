编码基准来自 Web Platform Tests，许可证见 `license.md`。

- `hevc_key_frame.bin`：`webcodecs/h265.annexb` 的第一帧，字节 `[0, 4894)`。
- `av1_key_frame.bin`：`webcodecs/av1.mp4` 的第一条视频 sample，字节 `[48, 1986)`。

源文件：https://github.com/web-platform-tests/wpt/tree/master/webcodecs

两条基准均为 320×240，用于独立解码、尺寸校验和截断输入测试，不依赖系统媒体扩展或硬件。
