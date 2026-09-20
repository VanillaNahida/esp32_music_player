#ifndef __CONFIG_H
#define __CONFIG_H

#include "Music.h"

/* ============================================================
 * 音乐盒配置（对照 AudioLab 合成器）
 * 修改后重新编译即可生效
 * ============================================================ */

/* ---- 引擎 ---- */
#define MUSIC_SAMPLE_RATE       44100      /* I2S 采样率（Hz）：32000/44100/48000 均可 */
/* 最大同时发声数。原来设 10：实测曲库峰值同时发声 23 个（Aoharu.h，含释放段），
 * 超出的音符会把最早声部"抢断"（听感 = 音符被掐掉，密集曲子明显和原 MIDI 不一样）。
 * 32 对当前曲库留约 40% 余量；声部只在发声时才占 CPU，空闲声部只是一个判断，
 * 每多一个声部约 40B RAM。以后遇到更密集的 MIDI 可继续加大。 */
#define MUSIC_MAX_POLYPHONY     32
#define MUSIC_MASTER_VOLUME_Q8  256        /* 主音量：0~256，256=100%（越大越响） */

/* ---- 开机默认音色：WAVE_SINE / WAVE_TRIANGLE / WAVE_SQUARE / WAVE_SAWTOOTH ----
 * 运行中按板载 BOOT 键可在 4 种音色之间循环切换（见下面的 KEY_BOOT_PIN）。 */
#define MUSIC_WAVEFORM          WAVE_TRIANGLE

/* ---- 音符时长：1=遵循 MIDI 原时长（乐谱 duration），0=所有音符统一固定时长 ---- */
#define MUSIC_DURATION_FROM_MIDI    1       /* 1=MIDI，0=固定时长 */
#define MUSIC_NOTE_DURATION_MS      200     /* 固定时长（毫秒），FROM_MIDI=0 时生效 */

/* ---- 包络（对应 AudioLab：起音 6ms / 衰减 / 延音 0.55 电平）---- */
#define MUSIC_ATTACK_MS         6       /* 起音时长：6ms（0.006s） */
#define MUSIC_DECAY_ENABLE      1       /* 衰减开关：1 开，0 关 */
#define MUSIC_DECAY_TIME_MS     1500     /* 衰减时间：500ms（0.5s），0.1~10s 可选 */
#define MUSIC_SUSTAIN_ENABLE    1       /* 延音开关：1 开（衰减后保持电平），0 关（衰减到底） */
#define MUSIC_SUSTAIN_GAIN_Q8   255     /* 延音电平（Q8）：255/256≈100%，即峰值的 99% */

/* ---- 音轨控制 ----
 * track 字段 = MIDI 文件的轨道下标（0 起；AudioLab 原样导出，范围 0~15）。
 * 注意：标准 MIDI 的第 0 轨常常只是速度/节拍轨，真正的声音在第 1、2、3… 轨，
 * 所以这里必须把 16 轨都列出来 —— 数组长度就是引擎支持的轨道数，没列到的
 * 轨道号以前会被引擎**静默丢弃**（只写 2 项时，track>=2 的整条轨道都不发声）。
 * 每项：1=播放 / 0=静音；音量 256=100%。 */
#define MUSIC_TRACK_ENABLE      {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1}
#define MUSIC_TRACK_VOLUME_Q8   {220, 256, 256, 256, 256, 256, 256, 256, 256, 256, 256, 256, 256, 256, 256, 256}

/* ---- 播放控制 ----
 * 一曲播完后自动接下一曲，最后一首之后回到第 1 首（循环播放）。
 * 数值 = "播完"到"切下一曲"之间的间隔（毫秒），用来让屏幕上的"完成"提示
 * 显示一下；设为 0 = 立刻切换。
 * 间隔期间按 KEY1（播放控制）/ KEY4（重播）/ KEY3（下一曲）会取消这次自动切换。 */
#define MUSIC_AUTO_NEXT_MS      1000

/* ---- I2S（MAX98357A）输出引脚 ---- */
#define MUSIC_BCLK_PIN          5      /* BCLK：位时钟 */
#define MUSIC_WS_PIN            4      /* LRC/WS：声道选择 */
#define MUSIC_DIN_PIN           6      /* DIN：数据输出 */

/* 输出增益（Q16，即 /65536）。为音量五档（0~0.375）留出动态余量，
 * 最大档约 0.5*0.375；配合软限幅避免削顶噪声。
 * 整体偏轻可调大；偏响/爆音可调小。 */
#define MUSIC_OUT_GAIN          32768

/* ---- U8g2 OLED (SSD1306 128x64, 硬件 I2C) ----
 * 引脚不变：ESP32 的 I2C 外设可映射到任意 GPIO，所以从软件 I2C 换成硬件
 * I2C 不需要改接线。
 * OLED_I2C_HZ = 硬件 I2C 时钟。位翻转的软件 I2C 每帧要 ~74ms，刷屏时肉眼
 * 能看到从上到下的"扫描感"；硬件 I2C 400kHz 整帧约 23ms，滚动时更进一步
 * 只推变化的那几个 tile（384B ≈ 8.6ms），基本看不出刷新。
 * 若出现花屏/雪花/间歇性不显示（模块上拉偏弱或走线较长），把它降到
 * 100000 再试；想更快可以试 1000000（1MHz），多数 SSD1306 模块能跑。 */
#define OLED_SDA_PIN            8
#define OLED_SCL_PIN            9
#define OLED_I2C_HZ             400000

/* ---- 按键引脚（低电平触发，按键另一端接 GND，内部上拉）----
 * 全部为短按触发（按下瞬间响应），已不再使用长按。 */
#define KEY1_PIN                1      /* 短按：播放 / 暂停 / 继续 */
#define KEY2_PIN                2      /* 短按：音量五级循环 */
#define KEY3_PIN                3      /* 短按：切换下一曲 */
#define KEY4_PIN                7      /* 短按：从当前曲目开头重播（原 KEY1 长按功能） */

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