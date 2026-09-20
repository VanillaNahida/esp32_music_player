#include "Key.h"
#include "config.h"
#include <Arduino.h>

/* ============================================================
 * 多按键（KEY1~KEY4 + 板载 BOOT）非阻塞消抖状态机
 *
 * 统一行为：原始电平连续稳定 KEY_DEBOUNCE_MS 才被认可；确认"按下"的那一刻
 * 立即上报一次事件（KEYx_SHORT），按住不放不会连发，松手不产生事件。
 * 不再检测长按（原来的 KEY1 长按 2s 重播已改成独立的 KEY4 短按），
 * 因此每个按键都在按下瞬间响应，无需等松手。
 * 时间基准由 main.cpp 的 millis() 提供。
 * ============================================================ */

#define KEY_DEBOUNCE_MS     10u

typedef struct {
    uint8_t  pin;       /* GPIO（低电平=按下） */
    uint8_t  ev;        /* 按下时上报的事件码 */
    uint8_t  stable;    /* 消抖后的电平：1=按下 */
    uint8_t  raw;       /* 上一次采样到的原始电平 */
    uint32_t t_raw;     /* 原始电平最近一次变化的时刻 */
} KeyCtx;

static KeyCtx s_keys[] = {
    /* pin            按下上报的事件    运行期状态 */
    { KEY1_PIN,      KEY1_SHORT,      0u, 0u, 0u },
    { KEY2_PIN,      KEY2_SHORT,      0u, 0u, 0u },
    { KEY3_PIN,      KEY3_SHORT,      0u, 0u, 0u },
    { KEY4_PIN,      KEY4_SHORT,      0u, 0u, 0u },
    { KEY_BOOT_PIN,  KEY_BOOT_SHORT,  0u, 0u, 0u },
};
#define KEY_NUM     (sizeof(s_keys) / sizeof(s_keys[0]))

void Key_Init(void)
{
    for (uint8_t i = 0u; i < KEY_NUM; i++)
        pinMode(s_keys[i].pin, INPUT_PULLUP);
}

/* 单个按键扫描，返回事件码（KEY_NONE=无事件） */
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
        if (raw) return k->ev;          /* 确认按下：上报一次，之后按住不再上报 */
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
