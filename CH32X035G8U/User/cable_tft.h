/********************************** (C) COPYRIGHT *******************************
 * File Name          : cable_tft.h
 * Author             : CableTester Team
 * Version            : V1.0.0
 * Date               : 2026/04/26
 * Description        : ST7735S TFT display driver header file for CableTester
*********************************************************************************/

#ifndef __CABLE_TFT_H
#define __CABLE_TFT_H

#include "ch32x035.h"
#include "cable_font.h"  // 包含字体定义

#ifdef __cplusplus
extern "C" {
#endif

/* 屏幕参数 - 硬件竖屏(80x160)，顺时针旋转90度后为横屏(160x80) */
#define TFT_WIDTH       160     // 旋转后的逻辑宽度
#define TFT_HEIGHT      80      // 旋转后的逻辑高度

/* 显示偏移量（用于校准显示位置） */
#define DISPLAY_OFFSET_X  1     // X轴向右偏移1像素
#define DISPLAY_OFFSET_Y  26    // Y轴向下偏移26像素

/* 边框参数 */
#define BORDER_THICKNESS  2     // 边框线宽（像素）

/* 颜色定义 (RGB565格式) */
#define COLOR_BLACK     0x0000
#define COLOR_WHITE     0xFFFF
#define COLOR_RED       0xF800
#define COLOR_GREEN     0x07E0
#define COLOR_BLUE      0x001F
#define COLOR_YELLOW    0xFFE0
#define COLOR_CYAN      0x07FF
#define COLOR_MAGENTA   0xF81F
#define COLOR_GRAY      0x8410
#define COLOR_ORANGE    0xFA00
#define COLOR_DARK_GRAY 0x4208

/* 函数声明 */
void TFT_Init(void);
void TFT_Clear(uint16_t color);
void TFT_DrawBorder(uint16_t color);  // 绘制边框（调试用）
void TFT_Set_Window(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2);
void TFT_DrawPixel(uint16_t x, uint16_t y, uint16_t color);
void TFT_DrawLine(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1, uint16_t color);
void TFT_FillRect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color);

// 字体相关函数（推荐使用，支持多字体）
void TFT_DrawChar_Font(uint16_t x, uint16_t y, char c, uint16_t color, uint16_t bg, font_type_t font);
void TFT_DrawString_Font(uint16_t x, uint16_t y, const char *str, uint16_t color, uint16_t bg, font_type_t font);

// 兼容旧接口（默认使用FONT_5x7字体，保持向后兼容）
void TFT_DrawChar(uint16_t x, uint16_t y, char c, uint16_t color, uint16_t bg);
void TFT_DrawString(uint16_t x, uint16_t y, const char *str, uint16_t color, uint16_t bg);

#ifdef __cplusplus
}
#endif

#endif /* __CABLE_TFT_H */
