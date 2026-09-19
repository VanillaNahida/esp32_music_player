#ifndef __CONFIG_H
#define __CONFIG_H

#include "Music.h"

/* ============================================================
 * 音乐盒配置（对照 AudioLab 合成器）
 * 修改后重新编译即可生效
 * ============================================================ */

/* ---- 引擎 ---- */
#define MUSIC_SAMPLE_RATE       44100      /* I2S 采样率（Hz）：32000/44100/48000 均可 */
#define MUSIC_MAX_POLYPHONY     10         /* 最大同时发声数（复音数） */
#define MUSIC_MASTER_VOLUME_Q8  256        /* 主音量：0~256，256=100%（越大越响） */

/* ---- 开机默认音色：WAVE_SINE / WAVE_TRIANGLE / WAVE_SQUARE / WAVE_SAWTOOTH ----
 * 运行中按板载 BOOT 键可在 4 种音色之间循环切换（见下面的 KEY_BOOT_PIN）。 */
#define MUSIC_WAVEFORM          WAVE_TRIANGLE

/* ---- 音符时长：1=遵循 MIDI 原时长（乐谱 duration），0=所有音符统一固定时长 ---- */
#define MUSIC_DURATION_FROM_MIDI    1       /* 1=MIDI，0=固定时长 */
#define MUSIC_NOTE_DURATION_MS      800     /* 固定时长（毫秒），FROM_MIDI=0 时生效 */

/* ---- 包络（对应 AudioLab：起音 6ms / 衰减 / 延音 0.55 电平）---- */
#define MUSIC_ATTACK_MS         6       /* 起音时长：6ms（0.006s） */
#define MUSIC_DECAY_ENABLE      1       /* 衰减开关：1 开，0 关 */
#define MUSIC_DECAY_TIME_MS     500     /* 衰减时间：500ms（0.5s），0.1~10s 可选 */
#define MUSIC_SUSTAIN_ENABLE    1       /* 延音开关：1 开（衰减后保持电平），0 关（衰减到底） */
#define MUSIC_SUSTAIN_GAIN_Q8   255     /* 延音电平（Q8）：255/256≈100%，即峰值的 99% */

/* ---- 音轨控制：按导出音符的 track 字段（0=旋律 1=伴奏 ...） ---- */
#define MUSIC_TRACK_ENABLE      {1, 1}       /* 每轨是否播放 */
#define MUSIC_TRACK_VOLUME_Q8   {220, 256}   /* 每轨音量：256=100% */

/* ---- I2S（MAX98357A）输出引脚 ---- */
#define MUSIC_BCLK_PIN          5      /* BCLK：位时钟 */
#define MUSIC_WS_PIN            4      /* LRC/WS：声道选择 */
#define MUSIC_DIN_PIN           6      /* DIN：数据输出 */

/* 输出增益（Q16，即 /65536）。为音量五档（0~0.375）留出动态余量，
 * 最大档约 0.5*0.375；配合软限幅避免削顶噪声。
 * 整体偏轻可调大；偏响/爆音可调小。 */
#define MUSIC_OUT_GAIN          32768

/* ---- U8g2 OLED (SSD1306 128x64, I2C) 引脚 ---- */
#define OLED_SDA_PIN            8
#define OLED_SCL_PIN            9

/* ---- 按键引脚（低电平触发，按键另一端接 GND，内部上拉） ---- */
#define KEY1_PIN                1      /* 短按：播放/暂停/继续；长按 2s：从当前曲目开头重播 */
#define KEY2_PIN                2      /* 短按：音量五级循环 */
#define KEY3_PIN                3      /* 短按：切换下一曲 */
#define KEY_LONG_MS             2000   /* 长按判定阈值（毫秒） */

/* ---- 板载 BOOT 键（ESP32-S3-DevKitC-1 上的 BOOT 按钮 = GPIO0，低电平触发，
 * 板上已有上拉，这里再开内部上拉无副作用）----
 * 短按：循环切换音色（正弦 -> 三角 -> 方波 -> 锯齿）。 */
#define KEY_BOOT_PIN            0

/* ---- 曲目 ----
 * src/music/ 下的所有乐谱都会在编译前被自动扫描并全部编入固件（曲目表
 * src/score_playlist.h 由 scripts/gen_score_playlist.py 生成），运行时
 * KEY3 短按循环切换。这里指定开机先播哪一首：填乐谱头文件名；找不到该
 * 文件时从第 0 首开始。 */
#define MUSIC_START_FILE        "n-buna.h"

#endif