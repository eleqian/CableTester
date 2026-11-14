/********************************** (C) COPYRIGHT *******************************
 * File Name          : cable_tft.c
 * Author             : CableTester Team
 * Version            : V1.0.0
 * Date               : 2026/04/26
 * Description        : ST7735S TFT display driver implementation for CableTester
 *********************************************************************************/

#include "cable_tft.h"
#include "cable_spi.h"
#include "cable_gpio.h"  // 包含TFT控制宏定义
#include "cable_font.h"  // 包含字体定义
#include "debug.h"
#include <string.h>
#include <stdio.h>

/* SPI引脚定义 (使用硬件SPI1) */
// TFT_CS   - PA4 (软件控制片选)
// TFT_SCK  - PA5 (SPI1 SCK)
// TFT_MOSI - PA7 (SPI1 MOSI)
// TFT_RS   - PA6 (软件控制RS/DC)
// TFT_RST  - 硬件RC电路复位，无软件控制

/* 注意：TFT控制宏已在cable_gpio.h中定义 */

/**
 * @brief  TFT写入命令字节
 * @param  cmd: 命令字节
 * @return none
 */
void TFT_Write_Command(uint8_t cmd)
{
    TFT_RS_LOW();      // RS=0: 命令模式
    TFT_CS_LOW();      // CS=0: 使能片选
    SPI_WriteByte(cmd);
    TFT_CS_HIGH();     // CS=1: 禁用片选
}

/**
 * @brief  TFT写入数据字节
 * @param  data: 数据字节
 * @return none
 */
void TFT_Write_Data(uint8_t data)
{
    TFT_RS_HIGH();     // RS=1: 数据模式
    TFT_CS_LOW();      // CS=0: 使能片选
    SPI_WriteByte(data);
    TFT_CS_HIGH();     // CS=1: 禁用片选
}

/**
 * @brief  TFT初始化
 * @return none
 */
void TFT_Init(void)
{
    // TFT_RST由硬件RC电路完成复位，无需软件控制
    Delay_Ms(100);

    // ST7735S初始化序列
    TFT_Write_Command(0x11);  // SLPOUT - 退出睡眠模式
    Delay_Ms(100);

    TFT_Write_Command(0xB1);  // FRMCTR1 - 帧率控制
    TFT_Write_Data(0x01);
    TFT_Write_Data(0x2C);
    TFT_Write_Data(0x2D);

    TFT_Write_Command(0xB2);  // FRMCTR2
    TFT_Write_Data(0x01);
    TFT_Write_Data(0x2C);
    TFT_Write_Data(0x2D);

    TFT_Write_Command(0xB3);  // FRMCTR3
    TFT_Write_Data(0x01);
    TFT_Write_Data(0x2C);
    TFT_Write_Data(0x2D);
    TFT_Write_Data(0x01);
    TFT_Write_Data(0x2C);
    TFT_Write_Data(0x2D);

    TFT_Write_Command(0xB4);  // INVCTR - 显示反转控制
    TFT_Write_Data(0x03);

    TFT_Write_Command(0xC0);  // PWCTR1 - 电源控制
    TFT_Write_Data(0xA2);
    TFT_Write_Data(0x02);
    TFT_Write_Data(0x84);

    TFT_Write_Command(0xC1);  // PWCTR2
    TFT_Write_Data(0xC5);

    TFT_Write_Command(0xC2);  // PWCTR3
    TFT_Write_Data(0x0A);
    TFT_Write_Data(0x00);

    TFT_Write_Command(0xC3);  // PWCTR4
    TFT_Write_Data(0x8A);
    TFT_Write_Data(0x2A);

    TFT_Write_Command(0xC4);  // PWCTR5
    TFT_Write_Data(0x8A);
    TFT_Write_Data(0xEE);

    TFT_Write_Command(0xC5);  // VMCTR1 - VCOM控制
    TFT_Write_Data(0x0E);

    // 修正颜色反转问题：使用INVON开启显示反转
    TFT_Write_Command(0x21);  // INVON - 开启显示反转（修正黑变白的问题）

    // 设置显示方向 - 顺时针旋转90度并修正颜色顺序
    TFT_Write_Command(0x36);  // MADCTL - 存储器数据访问控制
    // Bit7(MY)=1, Bit6(MX)=0, Bit5(MV)=1, Bit4(ML)=0, Bit3(BGR)=1, Bit2(MH)=0
    // MV=1: 行列交换（旋转90度）
    // MY=1: Y轴镜像
    // BGR=1: 使用BGR颜色顺序（修正红变黄、绿变紫的问题）
    TFT_Write_Data(0xAC);  // 0xA4 | 0x08 = 0xAC (启用BGR)

    // 像素格式
    TFT_Write_Command(0x3A);  // COLMOD - 接口像素格式
    TFT_Write_Data(0x05);     // 16位/pixel

    // 清屏
    TFT_Clear(COLOR_BLACK);

    TFT_Write_Command(0x29);  // DISPON - 打开显示
}

/**
 * @brief  清屏
 * @param  color: 填充颜色
 * @return none
 */
void TFT_Clear(uint16_t color)
{
    TFT_FillRect(0, 0, TFT_WIDTH, TFT_HEIGHT, color);
}

/**
 * @brief  设置显示窗口
 * @param  x1, y1: 左上角坐标
 * @param  x2, y2: 右下角坐标
 * @return none
 */
void TFT_Set_Window(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2)
{
    x1 += DISPLAY_OFFSET_X;
    x2 += DISPLAY_OFFSET_X;
    // 列地址设置
    TFT_Write_Command(0x2A);
    TFT_Write_Data(x1 >> 8);
    TFT_Write_Data(x1 & 0xFF);
    TFT_Write_Data(x2 >> 8);
    TFT_Write_Data(x2 & 0xFF);

    y1 += DISPLAY_OFFSET_Y;
    y2 += DISPLAY_OFFSET_Y;
    // 行地址设置
    TFT_Write_Command(0x2B);
    TFT_Write_Data(y1 >> 8);
    TFT_Write_Data(y1 & 0xFF);
    TFT_Write_Data(y2 >> 8);
    TFT_Write_Data(y2 & 0xFF);
}

/**
 * @brief  绘制一个像素点
 * @param  x, y: 坐标
 * @param  color: 颜色
 * @return none
 */
void TFT_DrawPixel(uint16_t x, uint16_t y, uint16_t color)
{
    // 边界检查
    if (x >= TFT_WIDTH || y >= TFT_HEIGHT)
        return;

    TFT_Set_Window(x, y, x, y);
    TFT_Write_Command(0x2C);
    TFT_Write_Data(color >> 8);
    TFT_Write_Data(color & 0xFF);
}

/**
 * @brief  绘制边框（用于调试显示偏移）
 * @param  color: 边框颜色
 * @return none
 */
void TFT_DrawBorder(uint16_t color)
{
    uint16_t i;

    // 绘制上边框
    for (i = 0; i < TFT_WIDTH; i++) {
        TFT_DrawPixel(i, BORDER_THICKNESS / 2, color);
        if (BORDER_THICKNESS > 1) {
            TFT_DrawPixel(i, BORDER_THICKNESS - 1, color);
        }
    }

    // 绘制下边框
    for (i = 0; i < TFT_WIDTH; i++) {
        TFT_DrawPixel(i, TFT_HEIGHT - 1 - BORDER_THICKNESS / 2, color);
        if (BORDER_THICKNESS > 1) {
            TFT_DrawPixel(i, TFT_HEIGHT - BORDER_THICKNESS, color);
        }
    }

    // 绘制左边框
    for (i = 0; i < TFT_HEIGHT; i++) {
        TFT_DrawPixel(BORDER_THICKNESS / 2, i, color);
        if (BORDER_THICKNESS > 1) {
            TFT_DrawPixel(BORDER_THICKNESS - 1, i, color);
        }
    }

    // 绘制右边框
    for (i = 0; i < TFT_HEIGHT; i++) {
        TFT_DrawPixel(TFT_WIDTH - 1 - BORDER_THICKNESS / 2, i, color);
        if (BORDER_THICKNESS > 1) {
            TFT_DrawPixel(TFT_WIDTH - BORDER_THICKNESS, i, color);
        }
    }
}

/**
 * @brief  绘制直线 (Bresenham算法)
 * @param  x0, y0: 起点坐标
 * @param  x1, y1: 终点坐标
 * @param  color: 颜色
 * @return none
 */
void TFT_DrawLine(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1, uint16_t color)
{
    int16_t dx, dy, sx, sy, err, e2;

    dx = (int16_t)x1 - (int16_t)x0;
    dy = (int16_t)y1 - (int16_t)y0;

    sx = (dx > 0) ? 1 : -1;
    sy = (dy > 0) ? 1 : -1;

    err = dx - dy;

    while (1) {
        TFT_DrawPixel(x0, y0, color);

        if (x0 == x1 && y0 == y1)
            break;

        e2 = 2 * err;
        if (e2 > -dy) {
            err -= dy;
            x0 += sx;
        }
        if (e2 < dx) {
            err += dx;
            y0 += sy;
        }
    }
}

/**
 * @brief  快速填充矩形区域（使用批量SPI写入）
 * @param  x, y: 左上角坐标
 * @param  w, h: 宽度和高度
 * @param  color: 填充颜色
 * @return none
 */
void TFT_FillRect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color)
{
    uint32_t pixel_count;
    uint8_t color_high = color >> 8;
    uint8_t color_low = color & 0xFF;
    uint32_t i;

    if (x >= TFT_WIDTH || y >= TFT_HEIGHT)
        return;
    if (x + w > TFT_WIDTH)
        w = TFT_WIDTH - x;
    if (y + h > TFT_HEIGHT)
        h = TFT_HEIGHT - y;

    // 设置窗口
    TFT_Set_Window(x, y, x + w - 1, y + h - 1);
    TFT_Write_Command(0x2C);

    // 切换到数据模式并保持CS低电平
    TFT_RS_HIGH();
    TFT_CS_LOW();

    // 计算总像素数
    pixel_count = (uint32_t)w * h;

    // 批量发送像素数据（每个像素2字节）
    for (i = 0; i < pixel_count; i++) {
        // 等待TXE
        SPI_Wait_TXE();
        SPI_I2S_SendData(SPI1, color_high);

        // 等待TXE
        SPI_Wait_TXE();
        SPI_I2S_SendData(SPI1, color_low);
    }

    // 等待所有数据发送完成
    SPI_Wait_Idle();

    // 释放CS
    TFT_CS_HIGH();
}

/**
 * @brief  高效绘制字符（使用批量SPI写入，减少事务开销）
 * @param  x, y: 坐标
 * @param  c: 字符
 * @param  color: 前景色
 * @param  bg: 背景色
 * @param  font: 字体类型
 * @return none
 */
void TFT_DrawChar_Font(uint16_t x, uint16_t y, char c, uint16_t color, uint16_t bg, font_type_t font)
{
    const font_info_t *font_info = Font_GetInfo(font);
    uint8_t i, j;
    uint8_t color_high = color >> 8;
    uint8_t color_low = color & 0xFF;
    uint8_t bg_high = bg >> 8;
    uint8_t bg_low = bg & 0xFF;

    // 获取字符字体数据
    const uint8_t *font_data = Font_GetCharData(font, c);
    if (!font_data) {
        return;
    }

    // 设置显示窗口（只设置一次）
    TFT_Set_Window(x, y, x + font_info->width - 1, y + font_info->height - 1);
    TFT_Write_Command(0x2C);  // RAMWR

    // 切换到数据模式并保持CS低电平
    TFT_RS_HIGH();
    TFT_CS_LOW();

    for (j = 0; j < font_info->height; j++) {      // 逐行
        // 计算当前行属于哪一组（0-2）和组内偏移（0-7）
        uint8_t group = j / 8;       // 0, 1, 或 2
        uint8_t offset = j % 8;      // 0-7

        for (i = 0; i < font_info->width; i++) {  // 逐列
            // 获取第group组、第i列的字节数据
            uint8_t byte_data = font_data[group * font_info->width + i];

            // 等待TXE
            SPI_Wait_TXE();
            if (byte_data & (1 << offset)) {
                SPI_I2S_SendData(SPI1, color_high);
            } else {
                SPI_I2S_SendData(SPI1, bg_high);
            }

            // 等待TXE
            SPI_Wait_TXE();
            if (byte_data & (1 << offset)) {
                SPI_I2S_SendData(SPI1, color_low);
            } else {
                SPI_I2S_SendData(SPI1, bg_low);
            }
        }
    }

    // 等待所有数据发送完成
    SPI_Wait_Idle();

    // 释放CS
    TFT_CS_HIGH();
}

/**
 * @brief  高效绘制字符串（使用批量SPI写入）
 * @param  x, y: 坐标
 * @param  str: 字符串
 * @param  color: 前景色
 * @param  bg: 背景色
 * @param  font: 字体类型
 * @return none
 */
void TFT_DrawString_Font(uint16_t x, uint16_t y, const char *str, uint16_t color, uint16_t bg, font_type_t font)
{
    const font_info_t *font_info = Font_GetInfo(font);
    uint8_t char_width = font_info->width + 1;  // 字符宽度+间距

    while (*str) {
        TFT_DrawChar_Font(x, y, *str, color, bg, font);
        x += char_width;
        if (x > TFT_WIDTH - char_width) {
            break;
        }
        str++;
    }
}

/**
 * @brief  绘制一个字符（兼容旧接口，默认使用5x8字体）
 * @param  x, y: 坐标
 * @param  c: 字符
 * @param  color: 前景色
 * @param  bg: 背景色
 * @return none
 * @note   此函数为向后兼容保留，内部调用TFT_DrawChar_Font并指定FONT_5x8
 *         如需指定其他字体，请使用TFT_DrawChar_Font函数
 */
void TFT_DrawChar(uint16_t x, uint16_t y, char c, uint16_t color, uint16_t bg)
{
    TFT_DrawChar_Font(x, y, c, color, bg, FONT_5x8);
}

/**
 * @brief  绘制字符串（兼容旧接口，默认使用5x8字体）
 * @param  x, y: 坐标
 * @param  str: 字符串
 * @param  color: 前景色
 * @param  bg: 背景色
 * @return none
 * @note   此函数为向后兼容保留，内部调用TFT_DrawString_Font并指定FONT_5x8
 *         如需指定其他字体，请使用TFT_DrawString_Font函数
 */
void TFT_DrawString(uint16_t x, uint16_t y, const char *str, uint16_t color, uint16_t bg)
{
    TFT_DrawString_Font(x, y, str, color, bg, FONT_5x8);
}
