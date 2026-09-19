#include "Key.h"
#include "config.h"
#include <Arduino.h>

/* ============================================================
 * 三按键（KEY1/KEY2/KEY3）非阻塞消抖状态机
 *
 * 事件上报时机：
 *   KEY1  —— 短按在【松手】时上报（KEY1_SHORT）。必须先等松手才能确定
 *            不是长按，否则长按会先触发一次播放/暂停再触发重播；
 *            按住 >= KEY_LONG_MS 时在【按住期间】上报一次 KEY1_LONG。
 *   KEY2/KEY3/BOOT —— 按下消抖完成即上报（KEYx_SHORT），手感更跟手，不检测长按。
 *
 * 对同一按键：一次按下只产生一个事件（长按触发后松手不再补短按）。
 * 消抖：原始电平连续稳定 KEY_DEBOUNCE_MS 才被认可。
 * 时间基准由 main.cpp 的 millis() 提供。
 * ============================================================ */

#define KEY_DEBOUNCE_MS     10u

typedef struct {
    uint8_t  pin;           /* GPIO（低电平=按下） */
    uint8_t  ev_short;      /* 短按事件码 */
    uint8_t  ev_long;       /* 长按事件码；KEY_NONE = 不检测长按 */
    uint8_t  on_press;      /* 1=短按在按下时上报（无需等松手） */
    uint8_t  stable;        /* 消抖后的电平：1=按下 */
    uint8_t  raw;           /* 上一次采样到的原始电平 */
    uint8_t  long_fired;    /* 本次按下是否已上报过长按 */
    uint32_t t_raw;         /* 原始电平最近一次变化的时刻 */
    uint32_t t_down;        /* 确认按下的时刻 */
} KeyCtx;

static KeyCtx s_keys[] = {
    /* pin            短按            长按       按下即报  运行期状态 */
    { KEY1_PIN,      KEY1_SHORT,     KEY1_LONG, 0u, 0u, 0u, 0u, 0u, 0u },
    { KEY2_PIN,      KEY2_SHORT,     KEY_NONE,  1u, 0u, 0u, 0u, 0u, 0u },
    { KEY3_PIN,      KEY3_SHORT,     KEY_NONE,  1u, 0u, 0u, 0u, 0u, 0u },
    { KEY_BOOT_PIN,  KEY_BOOT_SHORT, KEY_NONE,  1u, 0u, 0u, 0u, 0u, 0u },
};
#define KEY_NUM     (sizeof(s_keys) / sizeof(s_keys[0]))

void Key_Init(void)
{
    for (uint8_t i = 0u; i < KEY_NUM; i++)
        pinMode(s_keys[i].pin, INPUT_PULLUP);
}

/* 单个按键扫描，返回事件码（0=无） */
static uint8_t Key_Scan(KeyCtx *k, uint32_t now)
{
    uint8_t raw = (digitalRead(k->pin) == LOW) ? 1u : 0u;   /* 1=按下 */

    if (raw != k->raw)                  /* 电平刚跳变：重新计时，等它稳定 */
    {
        k->raw   = raw;
        k->t_raw = now;
        return KEY_NONE;
    }
    if (now - k->t_raw < KEY_DEBOUNCE_MS)
        return KEY_NONE;                /* 还在消抖窗口内 */

    if (raw != k->stable)               /* 电平已稳定且与上次认可的不同 */
    {
        k->stable = raw;
        if (raw)                        /* ---- 按下 ---- */
        {
            k->t_down     = now;
            k->long_fired = 0u;
            if (k->on_press) return k->ev_short;
        }
        else if (!k->on_press && !k->long_fired)   /* ---- 松手：没触发长按才算短按 ---- */
        {
            return k->ev_short;
        }
        return KEY_NONE;
    }

    /* ---- 按住不放：到点上报一次长按 ---- */
    if (k->stable && k->ev_long != KEY_NONE && !k->long_fired &&
        (now - k->t_down) >= (uint32_t)KEY_LONG_MS)
    {
        k->long_fired = 1u;
        return k->ev_long;
    }

    return KEY_NONE;
}

uint8_t Key_GetNum(void)
{
    uint32_t now = millis();
    uint8_t  ev  = KEY_NONE;

    for (uint8_t i = 0u; i < KEY_NUM; i++)
    {
        uint8_t e = Key_Scan(&s_keys[i], now);
        if (e != KEY_NONE && ev == KEY_NONE) ev = e;   /* 一轮只上报一个事件 */
    }
    return ev;
}
