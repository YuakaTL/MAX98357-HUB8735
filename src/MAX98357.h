/*
 * MAX98357.h - I2S audio playback library for HUB8735 Ultra (RTL8735B)
 *
 * Drives a MAX98357 I2S amplifier through the RTL8735B I2S0 peripheral
 * using the low-level mbed i2s_api (compiled in liboutsrc.a of the
 * AmebaPro2 Arduino package).
 *
 * RTL8735B I2S pins are a FIXED pin group (they cannot be remapped):
 *
 *   I2S signal | SoC pin | HUB8735 Ultra pin | MAX98357 pin
 *   -----------+---------+-------------------+-------------
 *   SCK (BCLK) | PF_13   | D24               | BCLK
 *   WS  (LRCK) | PF_15   | D12               | LRC
 *   SD_TX      | PF_14   | D11               | DIN
 *   SD_RX      | PF_12   | D23               | (unused, still claimed)
 *   MCLK       | PF_11   | D22               | (unused, still claimed)
 *
 * NOTE: begin() claims ALL five pins above. D12 is also the on-board
 * push button (PUSH_BTN), so the button cannot be used together with
 * this library - move your button to another GPIO (e.g. D13).
 *
 * Supported WAV format: RIFF PCM, 16-bit, mono or stereo,
 * sample rate 8k/11.025k/12k/16k/22.05k/24k/32k/44.1k/48k/88.2k/96k.
 */

#ifndef MAX98357_H
#define MAX98357_H

#include <Arduino.h>
#include "AmebaFatFS.h"

class MAX98357 {
public:
    MAX98357();

    // Initialize I2S0 (master, TX). Claims D11/D12/D22/D23/D24.
    bool begin(void);
    // Release the I2S peripheral and its pins.
    void end(void);

    // Software volume, 0.0 ~ 1.0 (default 1.0). Applied per sample.
    void setVolume(float vol);

    // Optional: wire the MAX98357 SD_MODE pin to a GPIO and register it
    // here. The amp is then held in hardware shutdown except while
    // playWav() runs (eliminates idle heating on modules that fail to
    // auto-shutdown on clock loss). Note: SD_MODE driven high by a
    // 3.3V GPIO selects LEFT channel output.
    void setShutdownPin(int pin);

    // Play a WAV file from SD card, blocking until playback finishes.
    // fs.begin() must have been called already.
    // filename is relative to the SD root, e.g. "test001.wav".
    bool playWav(AmebaFatFS &fs, const char *filename);
    // Play from an already-opened file (positioned anywhere; the WAV
    // header is parsed from offset 0). The file is left open.
    bool playWav(File &f);

    // Human-readable reason of the last failure.
    const char *lastError(void) const;

private:
    bool parseWavHeader(File &f, uint32_t &sampleRate, uint16_t &channels,
                        uint16_t &bits, uint32_t &dataSize);
    bool produceSlot(File &f, uint32_t &remaining, uint16_t channels);
    int mapSampleRate(uint32_t hz);

    uint16_t _vol_q8;   // volume in Q8 fixed point, 256 = 1.0
    bool _inited;
    int _sdPin;         // GPIO driving SD_MODE, -1 = not used
    const char *_err;
};

#endif  // MAX98357_H
