#include "Music.h"
#include "config.h"

#include <stdint.h>
#include <math.h>
#include <string.h>

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <driver/i2s_common.h>
#include <driver/i2s_std.h>

/* ============================================================
 * 软件混音合成引擎（ESP32 I2S 版）
 *
 * 每个采样周期的最终样本不再写 PWM，而是转成 int16 PCM 经
 * I2S DMA 输出给 MAX98357A 功放。
 *
 * 包络模型还原 AudioLab：起音 6ms 指数上升 → 可选指数衰减到
 * 延音电平(0.55) 或接近零 → 音符结束线性淡出。
 * ============================================================ */

/* ---- 引擎常量 ---- */
#define PEAK_Q16            16384u               /* 0.25 * 2^16（AudioLab 单音峰值） */
#define INIT_G              7u                   /* 0.0001 * 2^16，包络地板 */
#define PEAK_REF_Q16        14745u               /* 0.9*0.25 * 2^16（主音量 0.9 下的峰值参考） */

#define BLOCK_SAMPLES       256u                 /* 每块采样数（约 5.3ms @48k） */

/* ---- 包络状态 ---- */
#define ENV_OFF       0
#define ENV_ATTACK    1
#define ENV_DECAY     2
#define ENV_SUSTAIN   3
#define ENV_RELEASE   4

#define DECAY_ON      (MUSIC_DECAY_ENABLE != 0)
#define SUS_ON        (MUSIC_SUSTAIN_ENABLE != 0)

/* ---- 音轨配置（config.h） ---- */
static const uint8_t  g_track_en[]  = MUSIC_TRACK_ENABLE;
static const uint16_t g_track_vol[] = MUSIC_TRACK_VOLUME_Q8;
#define TRACK_NUM   (sizeof(g_track_en) / sizeof(g_track_en[0]))

/* ---- Voice（活动音符） ---- */
typedef struct {
    uint8_t  active;
    uint8_t  env;
    uint32_t phase;          /* 相位累加器（32bit） */
    uint32_t phase_inc;
    int32_t  gain;           /* Q16.16 当前增益 */
    int32_t  peak;           /* Q16.16 峰值 */
    uint32_t sus_target;     /* Q16.16 延音目标电平 */
    uint32_t attack_end;     /* 攻击段结束采样点 */
    uint32_t end_sample;     /* 音符时长结束采样点（进入释放） */
    uint32_t rel_step;       /* 释放段线性步进 */
    uint32_t rel_ns;         /* 释放段时长（采样数） */
} Voice;

static Voice g_voices[MUSIC_MAX_POLYPHONY];

/* ---- 查找表与包络参数 ---- */
static uint32_t g_phase_inc[128];   /* MIDI 音号 -> 相位增量 */
static int8_t   g_sine_lut[256];    /* 正弦表：音色可运行时切换，故始终构建（256B） */

/* ---- 当前音色（波形）----
 * 运行时可切换，所以这里是变量而不是编译期 #if；config.h 的
 * MUSIC_WAVEFORM 只决定开机默认值。 */
static volatile uint8_t g_waveform = (uint8_t)MUSIC_WAVEFORM;

static uint32_t g_attack_ns;        /* 攻击时长（采样数） */
static uint32_t g_decay_ns;         /* 衰减时长（采样数） */
static uint32_t g_release_ns;       /* 释放时长（采样数） */
static uint32_t g_k_attack;         /* 攻击指数系数（Q16.16） */
static uint32_t g_k_decay;          /* 衰减指数系数（Q16.16） */

/* ---- 播放状态 ---- */
static const MusicScore *g_cur       = 0;
static uint32_t g_note_idx           = 0;
static uint32_t g_spp_tick           = 0;   /* 每个 tick 的采样数（由乐谱 tick_us 计算） */
static uint32_t g_tick_us            = 1u;  /* 乐谱时间单位（微秒/tick），用于固定时长换算 */
static volatile uint32_t g_sample_cnt = 0;
static volatile uint8_t  g_playing   = 0;
static volatile uint8_t  g_finished  = 0;

/* 主音量：五级（0=静音 ... 4=最大），按档位乘系数 */
#define VOL_LEVELS  5u
static volatile uint8_t g_vol_level = 3u;
static const uint16_t g_vol_gain[VOL_LEVELS] = { 0, 8192, 16384, 24576, 32768 };

/* ---- I2S 通道与输出块缓冲 ---- */
static i2s_chan_handle_t s_chan = NULL;
static int16_t g_block_buf[BLOCK_SAMPLES];
static uint32_t g_block_wr = 0u;

/* 由起止电平与时长计算指数系数（初始化时调用，允许浮点） */
static uint32_t EnvK(double from, double to, uint32_t ns)
{
    if (ns == 0u) return 0u;
    return (uint32_t)(exp(log(to / from) / (double)ns) * 65536.0);
}

/* 计算音符峰值增益（Q16.16）：MASTER * 轨音量 * velocity/127 * 0.25 */
static int32_t PeakGain(uint8_t velocity, uint8_t track)
{
    uint64_t num = (uint64_t)MUSIC_MASTER_VOLUME_Q8 * g_track_vol[track]
                   * velocity * PEAK_Q16;
    return (int32_t)(num / (256u * 256u * 127u));
}

/* 启动一个音符（在渲染块内调用） */
static void NoteOn(const ScoreNote *n)
{
    /* 轨道号超出配置数组范围时不再直接丢弃，而是钳到最后一档。
     * （track 是 MIDI 轨道下标 0~15，标准 MIDI 的声音常在 1、2、3… 轨；
     *   以前 MUSIC_TRACK_ENABLE 只写 2 项，track>=2 的音符会被整段丢掉。） */
    uint8_t tr = n->track;
    if (tr >= TRACK_NUM) tr = (uint8_t)(TRACK_NUM - 1u);
    if (!g_track_en[tr]) return;

    Voice *v = 0;
    uint32_t oldest = 0xFFFFFFFFu;
    uint32_t i;
    for (i = 0u; i < MUSIC_MAX_POLYPHONY; i++) {
        if (!g_voices[i].active) { v = &g_voices[i]; break; }
        if (g_voices[i].end_sample < oldest) {
            oldest = g_voices[i].end_sample;
            v = &g_voices[i];
        }
    }
    if (v == 0) return;

    v->active       = 1;
    v->env          = ENV_ATTACK;
    v->phase        = 0u;
    v->phase_inc    = g_phase_inc[n->midi];
    v->peak         = PeakGain(n->velocity, tr);   /* 用钳好的轨道号，避免越界读音量表 */
    v->gain         = (int32_t)INIT_G;
    v->sus_target   = SUS_ON ? ((uint32_t)v->peak * MUSIC_SUSTAIN_GAIN_Q8) >> 8u : INIT_G;
    v->attack_end   = g_sample_cnt + g_attack_ns;
    if (MUSIC_DURATION_FROM_MIDI != 0) {
        v->end_sample = g_sample_cnt + (uint32_t)n->duration * g_spp_tick;
    } else {
        uint32_t dur_ticks = ((uint32_t)MUSIC_NOTE_DURATION_MS * 1000u) / g_tick_us;
        if (dur_ticks < 1u) dur_ticks = 1u;
        v->end_sample = g_sample_cnt + dur_ticks * g_spp_tick;
    }
    v->rel_ns       = g_release_ns;
    v->rel_step     = 0u;
}

/* 取一个 voice 的波形采样（返回 Q16.16 贡献值）。
 * 音色运行时可切换，故按 g_waveform 分支（每次切换对所有正在发声的
 * 音符立即生效，无需重播）。 */
static int32_t WaveSample(const Voice *v)
{
    switch (g_waveform)
    {
    case WAVE_SINE:
        return ((int32_t)v->gain * g_sine_lut[v->phase >> 24]) >> 7;

    case WAVE_SQUARE:
        return (v->phase & 0x80000000u) ? -v->gain : v->gain;

    case WAVE_SAWTOOTH:
    {
        int32_t saw = (int32_t)((v->phase >> 16) & 0xFFFFu) - 0x8000;
        return (v->gain * saw) >> 15;
    }

    case WAVE_TRIANGLE:
    default:
    {
        /* 标准三角波：相位 0..65535 先线性上升再线性下降，连续、关于 0 对称。
         * 旧式 (u^0x8000)-0x8000 在峰值处"垂直跌落"，听感像锯齿。 */
        uint32_t u = v->phase >> 16;                       /* 0..65535 */
        int32_t  slope = (u < 32768u) ? (int32_t)u
                                      : (int32_t)(65535u - u); /* 0..32767 */
        int32_t  tri = (slope << 1) - 32767;               /* -32767..+32767 */
        return (v->gain * tri) >> 15;                      /* gain*32767 安全 */
    }
    }
}

/* 软限幅：进入上下 20% 后逐渐压缩而非硬切成方波，减少削顶刺耳。
 * 输入为缩放后的 int32 采样，输出限制在 int16 范围内。 */
static int32_t SoftClip(int32_t x)
{
    const int32_t knee = 26000;
    int32_t ax = x < 0 ? -x : x;
    int32_t o;
    if (ax <= knee) return x;
    o = knee + (ax - knee) / 2;       /* 峰值折半，直到上限 */
    if (o > 32767) o = 32767;
    return (x < 0) ? -o : o;
}

/* 渲染一个采样（调度 + 包络 + 混音），返回 int16 PCM 幅值 */
static void StepSample(void)
{
    uint32_t i;

    /* 1. 调度：按时间序激活到时的音符（乐谱已按 time 排序） */
    if (g_cur != 0 && g_note_idx < g_cur->note_count) {
        const ScoreNote *n = &g_cur->notes[g_note_idx];
        while (g_sample_cnt >= (uint32_t)n->time * g_spp_tick) {
            NoteOn(n);
            g_note_idx++;
            if (g_note_idx >= g_cur->note_count) break;
            n = &g_cur->notes[g_note_idx];
        }
    }

    /* 2. 渲染所有活跃 voice */
    int32_t acc = 0;
    for (i = 0u; i < MUSIC_MAX_POLYPHONY; i++) {
        Voice *v = &g_voices[i];
        if (!v->active) continue;

        /* 音符时长结束 → 进入释放段 */
        if (g_sample_cnt >= v->end_sample && v->env != ENV_RELEASE) {
            v->env = ENV_RELEASE;
            v->rel_step = (uint32_t)v->gain / v->rel_ns;
            if (v->rel_step < 1u) v->rel_step = 1u;
        }

        switch (v->env) {
        case ENV_ATTACK:
        {
            uint32_t g  = (uint32_t)v->gain;
            uint32_t ng = (g * g_k_attack) >> 16;
            if (ng <= g) ng = g + 1u;
            v->gain = (ng > (uint32_t)v->peak) ? (int32_t)v->peak : (int32_t)ng;
            if (g_sample_cnt >= v->attack_end) {
                v->env = DECAY_ON ? ENV_DECAY : ENV_SUSTAIN;
            }
            break;
        }
        case ENV_DECAY:
            v->gain = (int32_t)(((uint32_t)v->gain * g_k_decay) >> 16);
            if ((uint32_t)v->gain <= v->sus_target) {
                if (SUS_ON) { v->gain = (int32_t)v->sus_target; v->env = ENV_SUSTAIN; }
                else        { v->active = 0; v->env = ENV_OFF; continue; }
            }
            break;
        case ENV_SUSTAIN:
            break;
        case ENV_RELEASE:
            if ((uint32_t)v->gain <= v->rel_step) { v->active = 0; v->env = ENV_OFF; continue; }
            v->gain -= (int32_t)v->rel_step;
            break;
        default:
            break;
        }

        v->phase += v->phase_inc;
        acc += WaveSample(v);
    }

    /* 3. 混音样本 -> int16 PCM：acc 为 Q16.16，先乘输出增益缩放，
     *    再乘音量档位系数，最后软限幅（避免削顶成方波产生刺耳噪声）。 */
    {
        int32_t s = (int32_t)(((int64_t)acc * MUSIC_OUT_GAIN) >> 16);
        s = (int32_t)(((int64_t)s * g_vol_gain[g_vol_level]) >> 16);
        g_block_buf[g_block_wr] = SoftClip(s);
        g_block_wr++;
    }

    g_sample_cnt = g_sample_cnt + 1u;   /* 避免对 volatile 使用 ++ 触发编译告警 */
}

/* 渲染一整块 BLOCK_SAMPLES 个样本 */
static void RenderBlock(void)
{
    uint32_t i;
    for (i = 0u; i < BLOCK_SAMPLES; i++) StepSample();

    /* 自然结束检测：音符播完且所有 voice 释放完毕 */
    if (g_cur != 0 && g_note_idx >= g_cur->note_count) {
        uint8_t any = 0;
        for (i = 0u; i < MUSIC_MAX_POLYPHONY; i++) {
            if (g_voices[i].active) { any = 1; break; }
        }
        if (!any) { g_finished = 1; g_playing = 0; }
    }
}

/* ============================================================
 * I2S 播放任务：生成样本块并写入（DMA 消费速度决定实时节拍）
 * ============================================================ */
static void Music_Task(void *arg)
{
    int16_t silent[BLOCK_SAMPLES];
    memset(silent, 0, sizeof(silent));

    for (;;) {
        if (g_playing && !g_finished) {
            g_block_wr = 0u;
            RenderBlock();
            i2s_channel_write(s_chan, g_block_buf, sizeof(g_block_buf), NULL, 1000);
        } else {
            i2s_channel_write(s_chan, silent, sizeof(silent), NULL, 1000);
        }
    }
}

/* ============================================================
 * 对外接口
 * ============================================================ */
void Music_Init(void)
{
    uint32_t i;

    /* MIDI 音号 -> 相位增量表 */
    for (i = 0u; i < 128u; i++) {
        double freq = 440.0 * pow(2.0, ((double)i - 69.0) / 12.0);
        g_phase_inc[i] = (uint32_t)(freq * 4294967296.0 / (double)MUSIC_SAMPLE_RATE);
    }

    /* 正弦表：音色运行时可切，始终构建 */
    for (i = 0u; i < 256u; i++) {
        double s = sin(6.283185307179586 * (double)i / 256.0);
        g_sine_lut[i] = (int8_t)floor(s * 127.0 + 0.5);
    }

    /* 开机默认音色（config.h 的 MUSIC_WAVEFORM），越界则回到三角波 */
    g_waveform = ((uint8_t)MUSIC_WAVEFORM < WAVE_COUNT) ? (uint8_t)MUSIC_WAVEFORM
                                                        : (uint8_t)WAVE_TRIANGLE;

    g_attack_ns  = (MUSIC_SAMPLE_RATE / 1000u) * MUSIC_ATTACK_MS;
    g_decay_ns   = (MUSIC_SAMPLE_RATE / 1000u) * MUSIC_DECAY_TIME_MS;
    g_release_ns = DECAY_ON ? g_decay_ns : (MUSIC_SAMPLE_RATE / 1000u) * 50u;
    if (g_attack_ns == 0u) g_attack_ns = 1u;
    if (g_decay_ns  == 0u) g_decay_ns  = 1u;
    g_k_attack = EnvK((double)INIT_G, (double)PEAK_REF_Q16, g_attack_ns);
    g_k_decay  = SUS_ON ? EnvK((double)PEAK_REF_Q16, (double)PEAK_REF_Q16 * 0.55, g_decay_ns)
                        : EnvK((double)PEAK_REF_Q16, (double)INIT_G, g_decay_ns);

    /* ---- I2S 标准模式：16bit 单声道，驱动 MAX98357A ---- */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num   = 6;
    chan_cfg.dma_frame_num  = 240;
    chan_cfg.auto_clear     = true;
    esp_err_t err = i2s_new_channel(&chan_cfg, &s_chan, NULL);
    Serial.printf("[Music] new_channel: %s\n", esp_err_to_name(err));

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(MUSIC_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk          = I2S_GPIO_UNUSED,
            .bclk          = (gpio_num_t)MUSIC_BCLK_PIN,
            .ws            = (gpio_num_t)MUSIC_WS_PIN,
            .dout          = (gpio_num_t)MUSIC_DIN_PIN,
            .din           = I2S_GPIO_UNUSED,
            .invert_flags  = { 0, 0, 0 },
        },
    };
    err = i2s_channel_init_std_mode(s_chan, &std_cfg);
    Serial.printf("[Music] init_std: %s\n", esp_err_to_name(err));
    err = i2s_channel_enable(s_chan);
    Serial.printf("[Music] enable: %s\n", esp_err_to_name(err));

    /* 播放任务：固定到核心 0。
 * Arduino 的 loopTask 默认在核心 1，若把音频任务也钉在核心 1 会饿死
 * UI/按键循环（OLED 不刷新、按键无响应）。故分开：音频在 0 核。 */
    xTaskCreatePinnedToCore(Music_Task, "music", 12288, NULL, 4, NULL, 0);
}

void Music_Play(const MusicScore *score)
{
    uint32_t i;
    g_cur        = score;
    g_note_idx   = 0u;
    g_sample_cnt = 0u;
    g_finished   = 0u;
    g_tick_us    = score->tick_us;
    if (g_tick_us == 0u) g_tick_us = 1u;
    g_spp_tick   = (MUSIC_SAMPLE_RATE / 1000u) * (g_tick_us / 1000u);
    if (g_spp_tick == 0u) g_spp_tick = 1u;
    for (i = 0u; i < MUSIC_MAX_POLYPHONY; i++) {
        g_voices[i].active = 0;
        g_voices[i].env    = ENV_OFF;
    }
    g_playing = 1;
}

void Music_Pause(void)
{
    if (!g_playing) return;
    g_playing = 0;
}

void Music_Resume(void)
{
    if (g_playing || g_finished) return;
    g_playing = 1;
}

void Music_Stop(void)
{
    uint32_t i;
    for (i = 0u; i < MUSIC_MAX_POLYPHONY; i++) {
        g_voices[i].active = 0;
        g_voices[i].env    = ENV_OFF;
    }
    g_playing  = 0;
    g_finished = 1;
}

uint32_t Music_GetPlayMs(void)
{
    return (uint32_t)((uint64_t)g_sample_cnt * 1000u / MUSIC_SAMPLE_RATE);
}

void Music_SetVolume(uint8_t level)
{
    if (level >= VOL_LEVELS) level = VOL_LEVELS - 1u;
    g_vol_level = level;
}

uint8_t Music_GetVolume(void)
{
    return g_vol_level;
}

void Music_SetWaveform(uint8_t wave)
{
    if (wave >= WAVE_COUNT) wave = WAVE_COUNT - 1u;
    g_waveform = wave;      /* 渲染任务下一块采样即生效，不打断播放 */
}

uint8_t Music_GetWaveform(void)
{
    return g_waveform;
}

uint8_t Music_IsPlaying(void)
{
    return g_playing;
}

uint8_t Music_IsFinished(void)
{
    return g_finished;
}