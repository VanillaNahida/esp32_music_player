#ifndef __KEY_H
#define __KEY_H

#include <stdint.h>

/* 按键事件码：Key_GetNum() 的返回值，0 = 本轮无事件。
 * 所有按键行为统一：消抖确认"按下"时上报一次（KEYx_SHORT），按住不放不连发，
 * 松手不产生事件。不检测长按（原 KEY1"长按 2s 重播"已改为 KEY4 短按）。 */
#define KEY_NONE        0u
#define KEY1_SHORT      1u      /* 播放 / 暂停 / 继续 */
#define KEY2_SHORT      2u      /* 音量五级循环 */
#define KEY3_SHORT      3u      /* 切换下一曲 */
#define KEY4_SHORT      4u      /* 从当前曲目开头重播 */
#define KEY_BOOT_SHORT  5u      /* 板载 BOOT 键（GPIO0）：切换音色 */

void Key_Init(void);

/* 非阻塞按键扫描：每次调用最多返回一个事件码（见上面的宏），无事件返回 KEY_NONE */
uint8_t Key_GetNum(void);

#endif
