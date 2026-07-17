# MAX98357 for HUB8735 Ultra (RTL8735B)

[English](README.md) | **繁體中文**

用 HUB8735 Ultra(或其他 RTL8735B / AmebaPro2 板)直接驅動 MAX98357 I2S 功率放大器、播放 SD 卡上的 WAV 音檔的 Arduino Library。

## 為什麼需要這個 Library

HUB8735 Ultra 的 Arduino SDK 沒有提供 I2S 播放的 library。本專案直接呼叫底層 mbed `i2s_api`(實作已編譯在官方套件的 `liboutsrc.a` 內,無需額外安裝),並遵循 Realtek 官方 `module_i2s.c` 的正確初始化順序與中斷模型:

- RTL8735B 底層**沒有 TX-only 模式** — 以 TXRX 模式運行,RX 頁全部 arm 好後丟棄資料
- `i2s_enable()` **必須在所有 DMA page 預填送出之後**才呼叫,否則會產生 `page own control error` 風暴
- TX 完成中斷中**永遠補頁**(無資料時送靜音),DMA 永不斷炊;SD 卡讀取都在呼叫端 context,不進 ISR

## 接線

RTL8735B 的 I2S 腳位是**固定 pin group,不可重新映射**:

| MAX98357 | HUB8735 Ultra | SoC 腳位 | 說明 |
|---|---|---|---|
| BCLK | **D24** | PF_13 | I2S 位元時脈 |
| LRC | **D12** | PF_15 | 左右聲道時脈(WS) |
| DIN | **D11** | PF_14 | 音訊資料 |
| VIN | 5V | — | 建議在 VIN–GND 間加 470µF 電解電容穩壓 |
| GND | GND | — | 共地 |
| GAIN | 懸空 = 9dB | — | 接 VIN = 6dB(較不易過熱) |
| SD_MODE | 懸空,或接任一 GPIO | — | 接 GPIO 可用 `setShutdownPin()` 硬體關斷(見下方) |

**注意事項:**

- `begin()` 會佔用整組五支腳:**D11、D12、D22、D23、D24**(含用不到的 SD_RX/MCLK,這是硬體 pin group 的限制)
- **D12 同時是板載按鈕(PUSH_BTN)**,使用本 library 後板載按鈕不可用,外接按鈕請改接其他 GPIO
- **D13 不能當按鈕腳**:它是 Camera Flash LED 的 PWM 腳,板上有下拉電路,恆為 LOW

## 安裝

把 `MAX98357/` 資料夾放進 Arduino 的 libraries 目錄(sketchbook 的 `libraries/`,或板子套件的 `libraries/`),重啟 IDE 即可 `#include <MAX98357.h>`。

## 使用

```cpp
#include "AmebaFatFS.h"
#include <MAX98357.h>

AmebaFatFS fs;
MAX98357 amp;

void setup() {
    fs.begin();                  // SD 卡
    amp.begin();                 // I2S 初始化(佔用 D11/D12/D22/D23/D24)
    amp.setShutdownPin(10);      // 選用:SD_MODE 接 D10,不播放時硬體關斷
    amp.setVolume(0.8);          // 軟體音量 0.0 ~ 1.0

    if (!amp.playWav(fs, "test001.wav")) {   // 阻塞式播放 SD 卡根目錄的檔案
        Serial.println(amp.lastError());
    }
}

void loop() {}
```

完整範例見 [examples/PlayWav](examples/PlayWav/PlayWav.ino)。

### API

| 函式 | 說明 |
|---|---|
| `bool begin()` | 初始化 I2S0(master/TX)。佔用 D11/D12/D22/D23/D24 |
| `void end()` | 釋放 I2S 與腳位 |
| `void setVolume(float vol)` | 軟體音量 0.0~1.0(逐 sample 縮放),預設 1.0 |
| `void setShutdownPin(int pin)` | 選用:指定接到 SD_MODE 的 GPIO,不播放時拉低硬體關斷(解決部分模組 idle 發熱問題)。注意 3.3V 拉高後為「僅左聲道」模式 |
| `bool playWav(AmebaFatFS &fs, const char *filename)` | 播放 SD 卡根目錄的 WAV(阻塞至播完) |
| `bool playWav(File &f)` | 播放已開啟的檔案 |
| `const char *lastError()` | 最近一次失敗的原因 |

### 支援的音檔格式

RIFF **PCM 16-bit** WAV,單聲道或立體聲(單聲道自動複製到左右聲道),取樣率:
8k / 11.025k / 12k / 16k / 22.05k / 24k / 32k / 44.1k / 48k / 88.2k / 96k Hz。

MP3 或其他格式請先轉檔:

```bash
ffmpeg -i input.mp3 -ar 16000 -ac 1 -sample_fmt s16 test001.wav
```

## 硬體建議

- **電源**:VIN–GND 間加 470µF 電解電容(正極接 VIN),吸收 Class-D 的脈衝電流,避免大音量時 5V 被拉凹
- **發熱**:4Ω 小揚聲器若在安靜段仍發熱,是無濾波 Class-D 的 ~300kHz 漣波電流所致 — 縮短喇叭線,或每條輸出串 ferrite bead + 對地 470pF
- **不要**把電容並聯或串聯在喇叭輸出端(BTL 橋式輸出,並聯大電容會損壞放大器)

## 技術背景

I2S 腳位對應與初始化順序取自:

- [ambpro2_sdk `i2s_api.c`](https://github.com/Freertos-kvs-LTS/ambpro2_sdk/blob/main/component/mbed/targets/hal/rtl8735b/i2s_api.c) — I2S0/I2S1 固定 pin group 定義
- [ameba-rtos-pro2 `module_i2s.c`](https://github.com/Ameba-AIoT/ameba-rtos-pro2/blob/main/component/media/mmfv2/module_i2s.c) — 正確的 DMA page 管理與中斷模型

在 HUB8735 Ultra(ideasHatch AmebaPro2 4.0.15 套件)實機測試通過。

## License

MIT
