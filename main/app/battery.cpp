/*
 * 电池电量 + 充电检测
 * 移植自参考工程 watch.ino 的 battery_monitor_task (仅底层 ADC 改为 esp_adc 兼容层)
 */

#include "battery.h"

#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"

#include "arduino_compat.h"
#include "atomic_utils.h"
#include "board.h"

#define TAG "BATTERY"

/* 充电检测: 采集 200ms, 每 10ms 一次共 20 次数字采样。
 *
 * 背景: 参考工程直接 digitalRead(CHARGE_DETECT_PIN)==0 判充电, 但该脚实际接
 *       充电芯片给 LED 的 PWM 输出 —— 充电时是高低变化的方波, 不充电时靠外部
 *       10k 上拉到 3.3V。所以单次数字读取会随 PWM 相位随机跳动, 完全紊乱。
 *
 * 做法: 在 200ms 内均匀采样 20 次数字电平, 统计低电平次数, >= 2 即判为充电。
 *       相当于用"低电平占空比"近似该脚的平均电压(充电时占空比明显偏高),
 *       无需 ADC 引脚(本板 CHARGE_DETECT_PIN=39 在 ESP32-S3 上不是 ADC)。
 *
 * 取舍: 真正干净的做法是硬件在检测脚加一个电容把 PWM 平滑成稳定的直流电平,
 *       但改硬件麻烦; 直接改软件(多次均匀采样)是最快、最方便的办法, 故采用之。
 */
#define CHARGE_SAMPLE_TIMES   20   /* 采样次数 */
#define CHARGE_SAMPLE_MS      10   /* 相邻采样间隔 (20 x 10ms = 200ms) */
#define CHARGE_LOW_THRESHOLD  2    /* 低电平次数达到该值即视为充电 */

static bool read_charge_detect(void)
{
  int8_t low_count = 0;
  for (int i = 0; i < CHARGE_SAMPLE_TIMES; i++) {
    if (digitalRead(CHARGE_DETECT_PIN) == LOW) {
      low_count++;
    }
    vTaskDelay(pdMS_TO_TICKS(CHARGE_SAMPLE_MS));
  }
  return low_count >= CHARGE_LOW_THRESHOLD;
}

void battery_monitor_task(void* parameter)
{
  gpio_reset_pin((gpio_num_t)BATTERY_ADC_PIN);
  pinMode(BATTERY_ADC_PIN, ANALOG);
  gpio_pullup_dis((gpio_num_t)BATTERY_ADC_PIN);
  gpio_pulldown_dis((gpio_num_t)BATTERY_ADC_PIN);
  analogReadResolution(12);
  analogSetPinAttenuation(BATTERY_ADC_PIN, ADC_11db);

  pinMode(CHARGE_DETECT_PIN, INPUT);

  // --- 状态变量 ---
  float last_raw_voltage = 0.0f;
  float voltage_before_charge = 0.0f;
  float fixed_ir_drop = 0.0f;
  float filtered_percentage = -1.0f;

  bool last_charging_state = false;
  int lasttime = -20000;

  const int SAMPLES_PER_CYCLE = 20;
  TickType_t last_wake_time = xTaskGetTickCount();

  bool last_charging = read_charge_detect();   // 200ms 均匀采样 (启动多等约 0.2s)
  bool initial_charging = last_charging;        // true 表示正在充电

  if(initial_charging){
    atomic_store_bool(&is_charging, true);
    Serial.println("启动时充电器已插入");
    //赋予经验压升差值
    fixed_ir_drop = 0.15f;
    // 同步充电状态
    last_charging_state = true;
    // 标记需要初始化 voltage_before_charge，设为负数作为标志位
    voltage_before_charge = -1.0f;
  }

  while (1) {
    bool charging = read_charge_detect();   // 200ms 内 20 次数字采样, 低电平 >= 2 判充电
    if (charging && !last_charging) {
        atomic_store_bool(&is_charging, true);
        Serial.println("充电器插入");
    } else if(!charging && last_charging){
        atomic_store_bool(&is_charging, false);
        Serial.println("充电器拔出");
        fixed_ir_drop = 0;
    }
    last_charging = charging;

    if(millis() - lasttime > 20000){
        lasttime = millis();

        // 采样并计算
        uint32_t mv_sum = 0;
        for (int i = 0; i < SAMPLES_PER_CYCLE; i++) {
            mv_sum += analogReadMilliVolts(BATTERY_ADC_PIN);
            vTaskDelay(pdMS_TO_TICKS(2));
        }
        float avg_pin_mv = (float)mv_sum / SAMPLES_PER_CYCLE;
        float raw_voltage = (avg_pin_mv / 1000.0f) * VOLTAGE_DIVIDER_RATIO;

        bool current_charging_state = atomic_load_bool(&is_charging);

        // --- 启动时已充电的首次推算 ---
        // 如果 voltage_before_charge 被标记为 -1.0f，说明是启动时已插充电器，需要用当前电压反推
        if (voltage_before_charge < 0.0f && current_charging_state) {
            voltage_before_charge = raw_voltage - fixed_ir_drop; // 反推拔出前的大致电压
            if (voltage_before_charge < 0.0f) voltage_before_charge = 0.0f; // 安全保护
            Serial.print("启动反推插入前电压: "); Serial.println(voltage_before_charge);
        }

        //处理充电插入瞬间的状态
        if (current_charging_state && !last_charging_state) {
            if (last_raw_voltage > 0.1f) {
                voltage_before_charge = last_raw_voltage;
            }
            fixed_ir_drop = 0.0f;
        }

        // 在插入充电后的时，锁定固定的内阻压降
        if (current_charging_state && fixed_ir_drop == 0.0f && voltage_before_charge > 0.1f) {
            float current_rise = raw_voltage - voltage_before_charge;
            if (current_rise > 0.0f) {
                fixed_ir_drop = current_rise;
            }
        }

        // 计算用于电量百分比的补偿电压
        float voltage_for_calc = raw_voltage;

        if (current_charging_state && fixed_ir_drop > 0.0f) {
            if (raw_voltage < 4.15f) { // 恒流阶段
                voltage_for_calc = raw_voltage - fixed_ir_drop;

                // 防止因为测量波动导致算出来的值低于插入前电压
                if (voltage_for_calc < voltage_before_charge) {
                    voltage_for_calc = voltage_before_charge;
                }
            } else {
                voltage_for_calc = raw_voltage;
            }
        }

        //校准并计算
        float cal = atomic_load_float(&Calibration);
        float calibrated_voltage = voltage_for_calc * (cal / 100.0f);

        int raw_percent = (int)((calibrated_voltage - BATTERY_EMPTY_V) / (BATTERY_FULL_V - BATTERY_EMPTY_V) * 100.0f);
        if (raw_percent < 0) raw_percent = 0;
        if (raw_percent > 100) raw_percent = 100;

        if (filtered_percentage < 0.0f) {
            filtered_percentage = (float)raw_percent;
        } else {
            filtered_percentage = (FILTER_ALPHA * raw_percent) + ((1.0f - FILTER_ALPHA) * filtered_percentage);
        }
        int final_percent = (int)round(filtered_percentage);

        // 存储结果
        atomic_store_float(&battery_voltage, raw_voltage);
        atomic_store_int(&battery_percentage, final_percent);

        last_raw_voltage = raw_voltage;
        last_charging_state = current_charging_state;
    }
    vTaskDelayUntil(&last_wake_time, pdMS_TO_TICKS(200));
  }
}

void battery_monitor_start(void)
{
    xTaskCreatePinnedToCore(battery_monitor_task, "BatteryMon", 4096, NULL, 1, NULL, 1);
}
