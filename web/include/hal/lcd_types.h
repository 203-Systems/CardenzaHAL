#pragma once
#include "hal/color_types.h"
typedef enum { LCD_RGB_DATA_ENDIAN_BIG = 0, LCD_RGB_DATA_ENDIAN_LITTLE } lcd_rgb_data_endian_t;
typedef enum { LCD_COLOR_RANGE_LIMIT, LCD_COLOR_RANGE_FULL } lcd_color_range_t;
typedef enum { LCD_YUV_CONV_STD_BT601, LCD_YUV_CONV_STD_BT709 } lcd_yuv_conv_std_t;
typedef enum { LCD_YUV422_PACK_ORDER_YUYV, LCD_YUV422_PACK_ORDER_YVYU, LCD_YUV422_PACK_ORDER_UYVY, LCD_YUV422_PACK_ORDER_VYUY } lcd_yuv422_pack_order_t;
typedef int lcd_clock_source_t;
