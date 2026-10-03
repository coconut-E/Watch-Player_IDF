# PNGdec (本地组件, 移植自 D:\Arduino\libraries\PNGdec v1.1.3)
#
# 唯一源码改动: src/PNGdec.h 第 24 行的平台判断加入 ESP_PLATFORM,
# 使其走 stdlib/string/stdint/stdio 分支 (与 bitbank2/jpegdec 的 IDF 移植做法一致)。
#
# SIMD: src/s3_simd_rgb565.S 为 ESP32-S3 汇编 (s3_rgb565),
#       png.inl 中通过 ARDUINO_ESP32S3_DEV 宏启用 (真彩+Alpha → RGB565),
#       汇编文件本身用 ARDUINO_ARCH_ESP32 + dsps_fft2r_sc16_aes3_enabled 启用。
#       若汇编编译失败, 删掉下面 SRCS 里的 "src/s3_simd_rgb565.S"
#       与本文件的 target_compile_definitions 即可回退到纯 C 路径 (结果一致, 仅稍慢)。

idf_component_register(
    SRCS "src/PNGdec.cpp"
         "src/adler32.c"
         "src/crc32.c"
         "src/infback.c"
         "src/inffast.c"
         "src/inflate.c"
         "src/inftrees.c"
         "src/zutil.c"
         "src/s3_simd_rgb565.S"
    INCLUDE_DIRS "src")

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    ARDUINO_ESP32S3_DEV
    ARDUINO_ARCH_ESP32)

# 老版本 zlib 代码可能触发 IDF 默认的 -Werror=all, 先放宽常见项
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-comment
    -Wno-sign-compare
    -Wno-error=implicit-fallthrough)
