#pragma once
#include <Arduino.h>

/**
 * @brief 指数加权移动平均 (EMA) + 异常值抑制的 RSSI 滤波器
 *        解决室内多径效应引起的 10~15dBm 瞬态信号抖动
 */
class RSSIFilter {
public:
    explicit RSSIFilter(float alphaRise = 0.45f, float alphaFall = 0.22f)
        : _alphaRise(alphaRise), _alphaFall(alphaFall), _filteredRssi(0.0f), _initialized(false), _lastUpdateMs(0) {}

    void reset() {
        _initialized = false;
        _filteredRssi = 0.0f;
        _lastUpdateMs = 0;
    }

    float update(float rawRssi) {
        uint32_t now = millis();
        // 关键优化: 若初次更新，或已失联超过 8 秒 (大于离席超时判定周期)，判定为回座重连新事件
        // 立即以当前真实强信号为基准瞬态复位，彻底消除 18dBm 步进爬坡的漫长等待！
        if (!_initialized || (_lastUpdateMs > 0 && (now - _lastUpdateMs >= 8000))) {
            _filteredRssi = rawRssi;
            _initialized = true;
            _lastUpdateMs = now;
            return _filteredRssi;
        }
        _lastUpdateMs = now;

        // 异常毛刺限幅：单次采样突变量不超过 18 dBm
        float delta = rawRssi - _filteredRssi;
        if (delta > 18.0f) delta = 18.0f;
        if (delta < -18.0f) delta = -18.0f;

        // 关键架构优化：非对称双速率滤波
        // 靠近电脑（delta > 0）：采用较敏捷的 alphaRise (0.45)，人一走近即刻响应秒解锁
        // 离开电脑（delta <= 0）：采用稳健平滑的 alphaFall (0.22)，抗人体遮挡杜绝误锁屏
        float alpha = (delta > 0.0f) ? _alphaRise : _alphaFall;
        _filteredRssi += alpha * delta;
        return _filteredRssi;
    }

    float get() const {
        return _filteredRssi;
    }

    bool isInitialized() const {
        return _initialized;
    }

    void setAlphas(float alphaRise, float alphaFall) {
        if (alphaRise > 0.01f && alphaRise <= 1.0f) _alphaRise = alphaRise;
        if (alphaFall > 0.01f && alphaFall <= 1.0f) _alphaFall = alphaFall;
    }

private:
    float _alphaRise;
    float _alphaFall;
    float _filteredRssi;
    bool  _initialized;
    uint32_t _lastUpdateMs;
};
