/*
 * 最小替身头: PNGdec 的 s3_simd_rgb565.S 只用到这一个宏
 * (原版来自 esp-dsp 组件的 dsps_fft2r_platform.h)。
 * 放在本组件 src/ 下, 供汇编文件用 #include "..." 直接找到。
 */

#ifndef DSPS_FFT2R_PLATFORM_H
#define DSPS_FFT2R_PLATFORM_H

#ifndef dsps_fft2r_sc16_aes3_enabled
#define dsps_fft2r_sc16_aes3_enabled 1
#endif

#endif /* DSPS_FFT2R_PLATFORM_H */
