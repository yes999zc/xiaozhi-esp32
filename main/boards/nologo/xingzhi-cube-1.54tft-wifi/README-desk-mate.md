# Desk Mate Cube

Desk Mate Cube is the 240 × 240 variant for the Xingzhi Cube 1.54-inch Wi-Fi board. It has one companion screen: network and device status, a face, confirmed microphone mute state, and the latest conversation subtitle. It does not display a scrolling log.

![Desk Mate Cube on-device preview](desk-mate-cube-preview.jpg)

## 中文使用说明

Cube 使用一个 240 × 240 伙伴界面，显示连接状态、表情、麦克风状态和最近一句字幕。

| 按键 | 操作 |
| --- | --- |
| BOOT 短按 | 开始或结束对话；启动联网阶段可进入配网。 |
| 音量 + / − 短按 | 增大或减小扬声器音量。 |
| 音量 + 长按约 1.5 秒 | 循环切换屏幕亮度。 |
| 音量 − 长按约 1.5 秒 | 切换麦克风静音，屏幕显示服务端确认结果。 |
| 侧边电源/复位键 | 实机短按会重启；厂家说明短按可开机、长按约 5 秒可关机，关机与开机操作仍需在这台设备上验证。 |

麦克风静音需要连接并配置控制服务；扬声器音量为零不等于麦克风静音。关机或深度休眠期间无法远程唤醒。

## Controls

| Control | Action |
| --- | --- |
| BOOT, short press | Start or end a conversation; enter Wi-Fi setup during startup. |
| Volume +, short press | Increase speaker volume by 10. |
| Volume −, short press | Decrease speaker volume by 10. |
| Volume +, hold 1.5 seconds | Cycle screen brightness between 30%, 65% and 100%. |
| Volume −, hold 1.5 seconds | Request microphone mute or unmute from the paired server. The screen shows pending, confirmed, or failed state. |
| Side power/reset control | The manufacturer describes short press to power on and a 5-second hold to power off. Short press while running restarts the connected device; the power-off behavior still needs physical verification on this unit. |

Microphone mute needs a configured control endpoint. Speaker volume 0 and microphone mute are separate controls. A disconnected or unconfigured server cannot confirm a mute request.

## Build

```sh
python3 scripts/build.py nologo/xingzhi-cube-1.54tft-wifi --name desk-mate-cube --language zh-CN
```

The public build defaults to an unusable OTA placeholder. Configure `XIAOZHI_OTA_BASE` or provision `wifi/ota_url` for your own server, and provision a private device pairing token separately. Do not publish paired images or NVS backups. The original `xingzhi-cube-1.54tft-wifi` variant keeps its existing interface and button behavior.

The existing battery power-save and shutdown behavior remains in place. Remote wake while deeply asleep depends on the hardware and power state and requires separate testing. See the [manufacturer's power-button instructions](https://www.nologo.tech/ai/aiproduct/esp32s3ai/esp32s3xiaozhi/esp32s3ai_qa.html).
