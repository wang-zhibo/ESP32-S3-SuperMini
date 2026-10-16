#pragma once
#include <Arduino.h>

/**
 * @brief 指数加权移动平均 (EMA) + 异常值抑制的 RSSI 滤波器
 *        解决室内多径效应引起的 10~15dBm 瞬态信号抖动
 */
class RSSIFilter {
public:
    explicit RSSIFilter(float alpha = 0.25f)
        : _alpha(alpha), _filteredRssi(0.0f), _initialized(false) {}

    void reset() {
        _initialized = false;
        _filteredRssi = 0.0f;
    }

    float update(float rawRssi) {
        if (!_initialized) {
            _filteredRssi = rawRssi;
            _initialized = true;
            return _filteredRssi;
        }

        // 异常毛刺限幅：单次采样突变量不超过 18 dBm
        float delta = rawRssi - _filteredRssi;
        if (delta > 18.0f) delta = 18.0f;
        if (delta < -18.0f) delta = -18.0f;

        _filteredRssi += _alpha * delta;
        return _filteredRssi;
    }

    float get() const {
        return _filteredRssi;
    }

    bool isInitialized() const {
        return _initialized;
    }

    void setAlpha(float alpha) {
        if (alpha > 0.01f && alpha <= 1.0f) {
            _alpha = alpha;
        }
    }

private:
    float _alpha;
    float _filteredRssi;
    bool  _initialized;
};
