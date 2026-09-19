#include <Arduino.h>
#include <U8g2lib.h>
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "Key.h"
#include "Music.h"

/* 曲目表：编译前由 scripts/gen_score_playlist.py 扫描 src/music/*.h 生成，
 * 收录 src/music/ 下的全部乐谱（SCORE_COUNT / g_playlist / g_playlist_names /
 * g_playlist_files）。新增曲目只需把 .h 丢进 src/music/ 再编译。 */
#include "score_playlist.h"

/* ============================================================
 * 多和弦音乐盒（ESP32 + I2S MAX98357A + U8g2 OLED 128x64）
 *
 * 开机自动播放 config.h 的 MUSIC_START_FILE 指定的乐谱（软件混音多和弦 ->
 * I2S 功放）；src/music/ 下的所有乐谱都会编入固件（见 score_playlist.h）。
 *   KEY1 短按：播放/暂停/继续；KEY1 长按 2s：从当前曲目开头重播
 *   KEY2 短按：音量（五级循环）
 *   KEY3 短按：切换下一曲
 *   BOOT 短按：切换音色（正弦/三角/方波/锯齿，立即生效不打断播放）
 *
 * 关键设计：OLED 渲染（软件 I2C 每帧约 300ms）放到独立任务 uiTask
 * 在 core 1 上跑；loop 只负责按键扫描与状态同步，按键响应不再被
 * 渲染阻塞（此前按键"按几次才反应"即因渲染卡住主循环所致）。
 * ============================================================ */

/* U8g2：SSD1306 128x64，软件 I2C 直连 GPIO */
U8G2_SSD1306_128X64_NONAME_F_SW_I2C u8g2(U8G2_R0, OLED_SCL_PIN, OLED_SDA_PIN);

/* 标题窗口与边距。
 * UI_MARGIN_X：左右留 4px 边距，曲名不再紧贴屏幕左边缘 —— 0.96" OLED 模块
 * 最左 1~2 列常被外壳边框压住，首字贴着 x=0 时看起来就像"跑到屏幕外面"。
 * UI_TITLE_Y：曲名下移 3px。setFontPosTop 下字形最上面一行落在 (draw_y - 2)
 * （该字库 ascent_A=8，而最高字形比"顶部参考行"还要高 2px），原来画在 y=0
 * 会把曲名顶部 1~2 行切到屏幕外；下移 3 后最靠上的笔画在 y=1，留 1px 余量。
 * UI_TITLE_W：可见宽度（= X1 - X0），曲名比它宽就自动滚动。 */
#define UI_MARGIN_X     4                     /* 左侧留白：曲名与进度行统一起点 */
#define UI_TITLE_X0     UI_MARGIN_X           /* 曲名起点 */
#define UI_TITLE_X1     128                   /* 曲名裁剪窗口右边界 */
#define UI_TITLE_W      (UI_TITLE_X1 - UI_TITLE_X0)   /* 可见宽度，滚动判定用 */
#define UI_TITLE_Y      3
#define UI_TITLE_H      18u
#define UI_PROG_Y       22u

/* 当前曲目索引（loop 写，uiTask 读） */
static volatile uint8_t s_song = 0u;

/* ---- 中文点阵字库：src/u8g2_font_wqy.c 里的完整文泉驿 GB2312 字集。
 *
 * 乱码根因：u8g2 字库内部一律按 Unicode 码位索引，而
 * u8g2_DrawStr() 走的是 ASCII 解码（u8x8_ascii_next：一个字节=一个字符），
 * 把 GB2312 双字节喂进去会被当成两个拉丁扩展字符查表，于是整行乱码。
 * 所以中文必须 ① 传 UTF-8 字符串（源文件本身即 UTF-8）
 *              ② 用 drawUTF8() / getUTF8Width()。---- */
extern const uint8_t u8g2_font_wqy12_t_gb2312_self[];

/* 首次显示/换曲时调用：重算曲名像素宽度与滚动需求，并复位滚动状态。
 * 一定要把 pix 归零 —— 否则从"正在滚动的长曲名"切到短曲名时，新曲名会停在
 * 旧的左移偏移上（表现为曲名偏左、左边被切在屏幕外，且因为短曲名不滚动而
 * 永远回不来）。 */
static void TitleReset(const char *title, uint8_t *scrolling, int16_t *pix_max,
                       uint16_t *pix, uint8_t *phase)
{
    u8g2.setFont(u8g2_font_wqy12_t_gb2312_self);
    int16_t w = (int16_t)u8g2.getUTF8Width(title);
    *scrolling = (w > UI_TITLE_W) ? 1u : 0u;      /* 比可见宽度长才滚动 */
    *pix_max   = (int16_t)(w - UI_TITLE_W);
    if (*pix_max < 0) *pix_max = 0;
    *pix   = 0u;                                  /* 回到曲名开头 */
    *phase = 2u;                                  /* 2=开头停顿：先静置 2s 再滚 */
}

/* 按乐谱头文件名查曲目索引（config.h 的 MUSIC_START_FILE）；找不到用第 0 首 */
static uint8_t Song_FromFile(const char *file)
{
    for (uint8_t i = 0u; i < SCORE_COUNT; i++)
        if (strcmp(g_playlist_files[i], file) == 0) return i;
    return 0u;
}

static const char TX_TIME[]   = "时间";
static const char TX_VOL[]    = "音量";
static const char TX_TIMBRE[] = "音色";
static const char TX_PLAY[]   = "播放";
static const char TX_PAUSE[]  = "暂停";
static const char TX_DONE[]   = "完成";

/* 音色名（下标与 Music.h 的 WAVE_* / WAVE_COUNT 一致） */
static const char *const WAVE_NAME[WAVE_COUNT] = { "正弦", "三角", "方波", "锯齿" };

/* ---- 共享 UI 状态：loop 写入，uiTask 读取 ---- */
static volatile uint32_t s_ms        = 0u;  /* 播放进度（毫秒） */
static volatile uint8_t  s_state     = 0u;  /* 0=播放 1=暂停 2=完成 */
static volatile uint8_t  s_vol       = 4u;  /* 音量显示值 1~5 */
static volatile uint8_t  s_wave      = 1u;  /* 当前音色（WAVE_*，BOOT 键切换） */
/* 状态行临时提示：由"最后按下的那个键"决定，2s 后自动回到时间行。
 * kind：0=无提示（显示时间+播放状态）1=音量 2=音色 */
static volatile uint8_t  s_popup       = 0u;
static volatile uint32_t s_popup_until = 0u;
static volatile uint8_t  s_dirty     = 1u;  /* 需要立即重绘 */

/* ---- UI 渲染任务（core 1）：滚动 + 绘制 ---- */
static void uiTask(void *arg)
{
    (void)arg;
    uint32_t t_title   = 0u, t_pause = 0u, t_render = 0u;
    uint8_t  phase     = 0u;      /* 0=滚动 1=末尾停顿 2=开头停顿 */
    uint8_t  scrolling = 0u;
    int16_t  pix_max   = 0;
    uint16_t pix       = 0u;
    uint8_t  last_popup = 0xFFu;  /* 上一帧提示行内容：0=时间 1=音量 2=音色 */
    uint8_t  last_song = 0xFFu;   /* 上一帧的曲目索引（换曲时重算标题） */
    const char *title  = "";      /* 当前曲名 */

    for (;;)
    {
        vTaskDelay(pdMS_TO_TICKS(20u));
        uint32_t now = millis();
        /* 状态行临时提示（音量/音色，2s 内有效），过期后回到时间+播放状态 */
        uint8_t popup = (s_popup_until && now < s_popup_until) ? s_popup : 0u;
        uint8_t need = 0u;
        uint8_t song = s_song;

        /* 换曲：重新取曲名、重算滚动参数（新曲名长度不同），立即重绘 */
        if (song != last_song)
        {
            last_song = song;
            title = g_playlist_names[song];
            TitleReset(title, &scrolling, &pix_max, &pix, &phase);
            t_title = now;
            t_pause = now;
            need = 1u;
        }

        /* 标题过长时自动滚动：每 30ms 步进 2px(共 pix_max 像素)，
         * 开头/末尾各停顿 2s（phase 2=开头停顿 0=滚动 1=末尾停顿） */
        if (scrolling && now - t_title >= 30u)
        {
            t_title = now;
            switch (phase)
            {
            case 0u:
                pix += 2u;
                if (pix >= (uint16_t)pix_max)
                {
                    pix = (uint16_t)pix_max;
                    phase = 1u;
                    t_pause = now;
                }
                need = 1u;
                break;
            case 1u:
                if (now - t_pause >= 2000u)
                {
                    pix = 0u;
                    phase = 2u;
                    t_pause = now;
                    need = 1u;
                }
                break;
            case 2u:
                if (now - t_pause >= 2000u) { phase = 0u; need = 1u; }
                break;
            }
        }

        /* 渲染时机：需要立即重绘 / 音量提示切换 / 每 1s 进度刷新 */
        if (!(s_dirty || popup != last_popup || now - t_render >= 1000u || need))
            continue;

        /* 组装状态文本（UTF-8） */
        uint32_t ms = s_ms;
        const char *st;
        if (s_state == 2u)      { ms = g_playlist[song]->total_time_ms; st = TX_DONE; }
        else if (s_state == 0u) { st = TX_PLAY; }
        else                     { st = TX_PAUSE; }

        u8g2.firstPage();
        do
        {
            /* 第 1 行：曲名，窗口裁剪 + 像素偏移滚动（中文 -> drawUTF8） */
            u8g2.setFont(u8g2_font_wqy12_t_gb2312_self);
            u8g2.setFontPosTop();
            u8g2.setClipWindow(UI_TITLE_X0, 0, UI_TITLE_X1, UI_TITLE_H);
            u8g2.drawUTF8(UI_TITLE_X0 - (int16_t)pix, UI_TITLE_Y, title);
            u8g2.setMaxClipWindow();

            /* 第 2 行：中文进度/音量（UTF-8 字符串 + drawUTF8） */
            u8g2.setFont(u8g2_font_wqy12_t_gb2312_self);
            u8g2.setFontPosTop();
            char buf[40];
            if (popup == 2u)          /* BOOT 切音色后的提示 */
                snprintf(buf, sizeof(buf), "%s %s", TX_TIMBRE,
                         WAVE_NAME[(s_wave < WAVE_COUNT) ? s_wave : 0u]);
            else if (popup == 1u)     /* KEY2 调音量后的提示 */
                snprintf(buf, sizeof(buf), "%s %u/5", TX_VOL, (unsigned)s_vol);
            else
                snprintf(buf, sizeof(buf), "%s %02u:%02u %s", TX_TIME,
                         (unsigned)(ms / 60000u),
                         (unsigned)((ms % 60000u) / 1000u), st);
            u8g2.drawUTF8(UI_MARGIN_X, UI_PROG_Y, buf);
        } while (u8g2.nextPage());

        s_dirty = 0u;
        last_popup = popup;
        t_render = now;
    }
}

void setup()
{
    Serial.begin(115200);
    Serial.println("[Boot] start");

    u8g2.begin();
    u8g2.clear();
    u8g2.sendBuffer();
    Serial.println("[Boot] OLED ok");

    Key_Init();
    Serial.println("[Boot] Key ok");

    Music_Init();
    Serial.println("[Boot] Music ok");

    /* 渲染任务（同 loop 一并放 core 1；缩放渲染时按时间片轮让出，
     * 不再阻塞按键扫描）。 */
    xTaskCreatePinnedToCore(uiTask, "ui", 8192, NULL, 1, NULL, 1);

    /* src/music/ 下所有乐谱都已编入曲目表；按 config.h 选开机播放哪一首 */
    s_song = Song_FromFile(MUSIC_START_FILE);
    Serial.printf("[Boot] %u songs, start #%u \"%s\"\n",
                  (unsigned)SCORE_COUNT, (unsigned)s_song, g_playlist_names[s_song]);
    Music_Play(g_playlist[s_song]);
    s_wave = Music_GetWaveform();     /* 开机音色（config.h 的 MUSIC_WAVEFORM） */
    Serial.printf("[Boot] timbre=%u\n", (unsigned)s_wave);
    Serial.println("[Boot] done");
}

void loop()
{
    static uint32_t t_heart = 0u;

    /* 心跳：每 2s 一次，确认 loop（按键扫描）仍在运行 */
    if (millis() - t_heart >= 2000u)
    {
        t_heart = millis();
        Serial.printf("[loop] song=%u/%u play=%u fin=%u vol=%u wave=%u ms=%lu\n",
                      (unsigned)s_song + 1u, (unsigned)SCORE_COUNT,
                      Music_IsPlaying(), Music_IsFinished(),
                      (unsigned)Music_GetVolume(), (unsigned)Music_GetWaveform(),
                      (unsigned long)Music_GetPlayMs());
    }

    uint8_t key = Key_GetNum();

    /* KEY1 短按：播放 / 暂停 / 播完后重播 */
    if (key == KEY1_SHORT)
    {
        if (Music_IsPlaying())        Music_Pause();
        else if (Music_IsFinished())  Music_Play(g_playlist[s_song]);
        else                          Music_Resume();
        s_dirty = 1u;
    }
    /* KEY1 长按 2s：当前曲目从头重播（播放中/暂停/已结束都适用） */
    else if (key == KEY1_LONG)
    {
        Music_Play(g_playlist[s_song]);
        s_dirty = 1u;
        Serial.printf("[key] restart #%u \"%s\"\n",
                      (unsigned)s_song, g_playlist_names[s_song]);
    }
    /* KEY2 短按：音量五级循环（0 静音 -> 4 最大），短暂显示音量 */
    else if (key == KEY2_SHORT)
    {
        uint8_t v = Music_GetVolume() + 1u;
        if (v > 4u) v = 0u;
        Music_SetVolume(v);
        s_vol = (uint8_t)(v + 1u);
        s_popup = 1u;                       /* 状态行显示音量 2s */
        s_popup_until = millis() + 2000u;
        s_dirty = 1u;
    }
    /* KEY3 短按：切下一曲（放到最后一首后再按回到第 0 首） */
    else if (key == KEY3_SHORT)
    {
        uint8_t next = (uint8_t)(s_song + 1u);
        if (next >= SCORE_COUNT) next = 0u;
        s_song = next;
        Music_Play(g_playlist[next]);
        s_dirty = 1u;
        Serial.printf("[key] next #%u/%u \"%s\"\n",
                      (unsigned)next, (unsigned)SCORE_COUNT, g_playlist_names[next]);
    }
    /* BOOT 键短按：循环切换音色（正弦 -> 三角 -> 方波 -> 锯齿），
     * 已在发声的音符立即换波形，不打断播放/进度 */
    else if (key == KEY_BOOT_SHORT)
    {
        uint8_t w = (uint8_t)((Music_GetWaveform() + 1u) % (unsigned)WAVE_COUNT);
        Music_SetWaveform(w);
        s_wave = w;
        s_popup = 2u;                       /* 状态行显示音色 2s */
        s_popup_until = millis() + 2000u;
        s_dirty = 1u;
        Serial.printf("[key] timbre -> %u\n", (unsigned)w);
    }

    /* 同步共享进度状态给 uiTask */
    if (Music_IsFinished())      s_state = 2u;
    else if (Music_IsPlaying())  s_state = 0u;
    else                          s_state = 1u;
    s_ms = Music_GetPlayMs();

    delay(10u);   /* 让出 CPU；按键 10ms 消抖配合，无需更密 */
}