#ifndef __CABLE_FONT_H
#define __CABLE_FONT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* 字体类型枚举 */
typedef enum {
    FONT_5x8 = 0,   // 5x8点阵字体（紧凑，实际高度为8像素）
    FONT_8x16,      // 8x16点阵字体（清晰）
    FONT_12x24,     // 12x24点阵字体（大字体）
    FONT_MAX
} font_type_t;

/* 扩展字符定义（紧接ASCII '~'后的第96个字符） */
#define CHAR_OMEGA 0x7F  /* 欧姆符号 Ω */

/* 字体信息结构 */
typedef struct {
    uint8_t width;        // 字符宽度
    uint8_t height;       // 字符高度
    uint8_t first_char;   // 第一个字符ASCII码
    uint8_t char_count;   // 字符数量
    const uint8_t *data;  // 字模数据指针
} font_info_t;

/* 函数声明 */
const font_info_t *Font_GetInfo(font_type_t type);
uint8_t Font_GetCharWidth(font_type_t type);
uint8_t Font_GetCharHeight(font_type_t type);
const uint8_t *Font_GetCharData(font_type_t type, int ch);

#ifdef __cplusplus
}
#endif

#endif /* __CABLE_FONT_H */
