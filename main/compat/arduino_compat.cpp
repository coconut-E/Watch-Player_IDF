/*
 * Arduino → ESP-IDF 兼容层实现 (见 arduino_compat.h)
 */

#include "arduino_compat.h"

#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_mac.h"

#include "lcd.h"
#include "touch.h"

SerialClass Serial;

/* ─────────────── Preferences (NVS) ─────────────── */
Preferences::Preferences() : _handle(0), _opened(false) {}

Preferences::~Preferences()
{
    if (_opened) {
        nvs_close(_handle);
    }
}

bool Preferences::begin(const char *name, bool readOnly)
{
    if (_opened) {
        nvs_close(_handle);
        _opened = false;
    }
    nvs_open_mode_t mode = readOnly ? NVS_READONLY : NVS_READWRITE;
    if (nvs_open(name, mode, &_handle) != ESP_OK) {
        return false;
    }
    _opened = true;
    return true;
}

void Preferences::end(void)
{
    if (_opened) {
        nvs_close(_handle);
        _opened = false;
    }
}

uint8_t Preferences::getUChar(const char *key, uint8_t defaultValue)
{
    uint8_t v = defaultValue;
    if (_opened) nvs_get_u8(_handle, key, &v);
    return v;
}

size_t Preferences::putUChar(const char *key, uint8_t value)
{
    if (_opened && nvs_set_u8(_handle, key, value) == ESP_OK) {
        nvs_commit(_handle);
        return 1;
    }
    return 0;
}

int32_t Preferences::getInt(const char *key, int32_t defaultValue)
{
    int32_t v = defaultValue;
    if (_opened) nvs_get_i32(_handle, key, &v);
    return v;
}

size_t Preferences::putInt(const char *key, int32_t value)
{
    if (_opened && nvs_set_i32(_handle, key, value) == ESP_OK) {
        nvs_commit(_handle);
        return 1;
    }
    return 0;
}

float Preferences::getFloat(const char *key, float defaultValue)
{
    float v = defaultValue;
    size_t len = sizeof(v);
    if (_opened) nvs_get_blob(_handle, key, &v, &len);
    return v;
}

size_t Preferences::putFloat(const char *key, float value)
{
    if (_opened && nvs_set_blob(_handle, key, &value, sizeof(value)) == ESP_OK) {
        nvs_commit(_handle);
        return 1;
    }
    return 0;
}

bool Preferences::isKey(const char *key)
{
    if (!_opened) return false;
    return nvs_find_key(_handle, key, NULL) == ESP_OK;
}

bool Preferences::remove(const char *key)
{
    if (!_opened) return false;
    return nvs_erase_key(_handle, key) == ESP_OK;
}

bool Preferences::getBool(const char *key, bool defaultValue)
{
    uint8_t v = defaultValue ? 1 : 0;
    if (_opened) nvs_get_u8(_handle, key, &v);
    return v != 0;
}

size_t Preferences::putBool(const char *key, bool value)
{
    if (_opened && nvs_set_u8(_handle, key, value ? 1 : 0) == ESP_OK) {
        nvs_commit(_handle);
        return 1;
    }
    return 0;
}

String Preferences::getString(const char *key, const String &defaultValue)
{
    if (!_opened) return defaultValue;

    size_t len = 0;
    if (nvs_get_str(_handle, key, NULL, &len) != ESP_OK || len == 0) {
        return defaultValue;
    }
    char *buf = (char *)malloc(len);
    if (!buf) return defaultValue;
    String out = defaultValue;
    if (nvs_get_str(_handle, key, buf, &len) == ESP_OK) {
        out = buf;
    }
    free(buf);
    return out;
}

size_t Preferences::putString(const char *key, const String &value)
{
    if (_opened && nvs_set_str(_handle, key, value.c_str()) == ESP_OK) {
        nvs_commit(_handle);
        return value.length();
    }
    return 0;
}

Preferences preferences;

/* ─────────────── ESP ─────────────── */
uint32_t ESPClass::getChipId()
{
    uint8_t mac[6] = {0};
    esp_efuse_mac_get_default(mac);
    return ((uint32_t)mac[2] << 24) | ((uint32_t)mac[3] << 16) |
           ((uint32_t)mac[4] << 8) | (uint32_t)mac[5];
}

ESPClass ESP;

/* ─────────────── 显示 ─────────────── */
void LGFX::setBrightness(uint8_t brightness)
{
    lcd_set_backlight(brightness);
}

bool LGFX::getTouch(uint16_t *x, uint16_t *y)
{
    int tx = 0, ty = 0;
    if (!touch_read(&tx, &ty)) {
        return false;
    }
    if (x) *x = (uint16_t)tx;
    if (y) *y = (uint16_t)ty;
    return true;
}

LGFX display;

/* ─────────────── ADC (Arduino analog* 兼容, 底层 esp_adc) ─────────────── */
static int          s_adc_bits  = 12;
static adc_atten_t  s_adc_atten = ADC_ATTEN_DB_12;
static adc_oneshot_unit_handle_t s_adc_unit = NULL;

typedef struct {
    bool             inited;
    adc_channel_t    chan;
    adc_cali_handle_t cali;
} adc_pin_ctx_t;
static adc_pin_ctx_t s_adc_pins[64];

void analogReadResolution(int bits)
{
    s_adc_bits = bits;
}

void analogSetPinAttenuation(int pin, int attenuation)
{
    (void)pin;
    switch (attenuation) {
        case ADC_0db:   s_adc_atten = ADC_ATTEN_DB_0;   break;
        case ADC_2_5db: s_adc_atten = ADC_ATTEN_DB_2_5; break;
        case ADC_6db:   s_adc_atten = ADC_ATTEN_DB_6;   break;
        case ADC_11db:
        default:        s_adc_atten = ADC_ATTEN_DB_12;  break;
    }
}

static adc_bitwidth_t adc_bitwidth(void)
{
    return (s_adc_bits == 12) ? ADC_BITWIDTH_12 : ADC_BITWIDTH_DEFAULT;
}

static bool adc_pin_init(int pin)
{
    if (pin < 0 || pin >= 64) return false;
    adc_pin_ctx_t *ctx = &s_adc_pins[pin];
    if (ctx->inited) return true;

    adc_unit_t unit = ADC_UNIT_1;
    adc_channel_t chan = ADC_CHANNEL_0;
    if (adc_oneshot_io_to_channel(pin, &unit, &chan) != ESP_OK) {
        return false;
    }
    if (unit != ADC_UNIT_1) {
        return false;   /* 仅支持 ADC1 */
    }

    if (!s_adc_unit) {
        adc_oneshot_unit_init_cfg_t unit_cfg = {};
        unit_cfg.unit_id = unit;
        if (adc_oneshot_new_unit(&unit_cfg, &s_adc_unit) != ESP_OK) {
            return false;
        }
    }

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten    = s_adc_atten,
        .bitwidth = adc_bitwidth(),
    };
    if (adc_oneshot_config_channel(s_adc_unit, chan, &chan_cfg) != ESP_OK) {
        return false;
    }

    ctx->chan = chan;
    ctx->cali = NULL;
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id  = unit,
        .chan     = chan,
        .atten    = s_adc_atten,
        .bitwidth = adc_bitwidth(),
    };
    adc_cali_create_scheme_curve_fitting(&cali_cfg, &ctx->cali);
#endif
    ctx->inited = true;
    return true;
}

uint32_t analogReadMilliVolts(int pin)
{
    if (!adc_pin_init(pin)) return 0;

    adc_pin_ctx_t *ctx = &s_adc_pins[pin];
    int raw = 0;
    if (adc_oneshot_read(s_adc_unit, ctx->chan, &raw) != ESP_OK) {
        return 0;
    }

    int mv = 0;
    if (ctx->cali && adc_cali_raw_to_voltage(ctx->cali, raw, &mv) == ESP_OK) {
        return (uint32_t)mv;
    }
    return (uint32_t)(raw * 3300 / 4095);
}

