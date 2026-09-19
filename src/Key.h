#ifndef __KEY_H
#define __KEY_H

#include <stdint.h>

/* 按键事件码：Key_GetNum() 的返回值，0 = 本轮无事件。
 * 短按 = 消抖后按下并松手；长按 = 持续按住 >= KEY_LONG_MS（config.h）。 */
#define KEY_NONE        0u
#define KEY1_SHORT      1u
#define KEY2_SHORT      2u
#define KEY3_SHORT      3u
#define KEY_BOOT_SHORT  4u      /* 板载 BOOT 键（GPIO0）：切换音色 */
#define KEY1_LONG       11u
#define KEY2_LONG       12u
#define KEY3_LONG       13u

void Key_Init(void);

/* 非阻塞按键扫描：每次调用最多返回一个事件码（见上面的宏），无事件返回 KEY_NONE */
uint8_t Key_GetNum(void);

#endif
