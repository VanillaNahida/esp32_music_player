#ifndef __MUSIC_H
#define __MUSIC_H

/* 与 AudioLab 导出的乐谱头文件兼容的必要类型与接口。
 * AudioLab 的 .h 会 #include "Music.h"，并依赖下列结构与宏。
 * 必须与 STM32 版逐字节布局一致（见 参考项目/Hardware/Music.h）。 */
#include <stdint.h>

/* 音色（波形）选择：config.h 的 MUSIC_WAVEFORM 是开机默认值，运行时可用
 * Music_SetWaveform() 随时切换（本机 BOOT 键循环切换）。 */
#define WAVE_SINE       0
#define WAVE_TRIANGLE   1
#define WAVE_SQUARE     2
#define WAVE_SAWTOOTH   3
#define WAVE_COUNT      4

/* 单个音符：8 字节，由 AudioLab 导出器生成 */
typedef struct {
    uint16_t time;      /* 起始时刻，单位：tick */
    uint16_t duration;  /* 时长，单位：tick */
    uint8_t  midi;      /* MIDI 音号 0-127 */
    uint8_t  velocity;  /* 力度 1-127 */
    uint8_t  track;     /* 音轨号：0=旋律 1=伴奏 ... */
} ScoreNote;

/* 一首乐曲（乐谱数组放在 Flash） */
typedef struct {
    uint32_t        note_count;
    uint32_t        total_time_ms;
    uint32_t        tick_us;        /* 时间单位（微秒/tick），由乐谱头文件给出 */
    const ScoreNote *notes;
} MusicScore;

/* 初始化合成引擎（I2S MAX98357A 输出，DMA 播放任务） */
void Music_Init(void);

/* 开始/重新播放一首乐曲 */
void Music_Play(const MusicScore *score);

/* 暂停（冻结输出与计时） */
void Music_Pause(void);

/* 从暂停处继续 */
void Music_Resume(void);

/* 停止并静音 */
void Music_Stop(void);

/* 当前播放进度（毫秒） */
uint32_t Music_GetPlayMs(void);

/* 设置主音量档位：0~4（五级，0 静音，4 最大） */
void Music_SetVolume(uint8_t level);

/* 读取当前音量档位 */
uint8_t Music_GetVolume(void);

/* 设置当前音色：0..WAVE_COUNT-1（WAVE_SINE/WAVE_TRIANGLE/WAVE_SQUARE/WAVE_SAWTOOTH）。
 * 越界值会被夹到最后一个；对已经在发声的音符立即生效，不会打断播放。 */
void Music_SetWaveform(uint8_t wave);

/* 读取当前音色 */
uint8_t Music_GetWaveform(void);

/* 是否正在播放 */
uint8_t Music_IsPlaying(void);

/* 是否已自然播放完毕 */
uint8_t Music_IsFinished(void);

#endif