#ifndef WALLPAPER_FORMAT_H
#define WALLPAPER_FORMAT_H

#include <stdint.h>

// 文件魔数
#define WP_MAGIC        "WP01"
#define WP_MAGIC_LEN    4
#define WP_HEADER_SIZE  16

// 壁纸类型
#define WP_TYPE_STATIC  0x01  // 单帧 RGB565
#define WP_TYPE_MJPEG   0x02  // 多帧 MJPEG

// 预览尺寸（固定 240x280 RGB565）
#define WP_PREVIEW_SIZE (240 * 280 * 2)  // 134400 bytes

// 文件头结构（16 字节，对齐）
typedef struct __attribute__((packed)) {
    char     magic[4];       // "WP01"
    uint8_t  type;           // WP_TYPE_STATIC / WP_TYPE_MJPEG
    uint8_t  reserved1;
    uint8_t  reserved2;
    uint8_t  reserved3;
    uint32_t mjpeg_size;     // MJPEG 数据长度（静态为 0）
    uint32_t reserved4;
} wp_header_t;

/*
 * .bin 文件布局:
 *
 * [偏移 0]    wp_header_t          (16 字节)
 * [偏移 16]   RGB565 预览数据      (134400 字节, 240x280)
 * [偏移 134416] MJPEG 原始数据      (仅 MJPEG 类型, 长度 = mjpeg_size)
 *
 * 静态壁纸总大小 = 16 + 134400 = 134416
 * MJPEG壁纸总大小 = 16 + 134400 + mjpeg_size
 */

// 判断文件扩展名是否为 MJPEG
static inline bool is_mjpeg_extension(const char* filename) {
    if (!filename) return false;
    const char* dot = strrchr(filename, '.');
    if (!dot) return false;
    return (strcasecmp(dot, ".mjpeg") == 0);
}

// 判断是否为静态图片扩展名
static inline bool is_static_image_extension(const char* filename) {
    if (!filename) return false;
    const char* dot = strrchr(filename, '.');
    if (!dot) return false;
    return (strcasecmp(dot, ".png") == 0 ||
            strcasecmp(dot, ".jpg") == 0 ||
            strcasecmp(dot, ".jpeg") == 0);
}

#endif // WALLPAPER_FORMAT_H
