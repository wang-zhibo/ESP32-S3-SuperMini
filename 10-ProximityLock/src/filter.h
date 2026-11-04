#pragma once
#include <Arduino.h>

/**
 * @brief 指数加权移动平均 (EMA) + 异常值抑制的 RSSI 滤波器
 *        解决室内多径效应引起的 10~15dBm 瞬态信号抖动
 */
class RSSIFilter {
public:
    explicit RSSIFilter(float alphaRise = 0.55f, float alphaFall = 0.22f)
        : _alphaRise(alphaRise),
          _alphaFall(alphaFall),
          _filteredRssi(0.0f),
          _initialized(false),
          _lastUpdateMs(0),
          _strongCount(0) {}

    void reset() {
        _initialized = false;
        _filteredRssi = 0.0f;
        _lastUpdateMs = 0;
        _strongCount = 0;
    }

    float update(float rawRssi) {
        uint32_t now = millis();
        // 关键优化 1: 若初次更新，或已失联超过 8 秒，判定为回座重连新事件，直接以当前真实信号对齐
        if (!_initialized || (_lastUpdateMs > 0 && (now - _lastUpdateMs >= 8000))) {
            _filteredRssi = rawRssi;
            _initialized = true;
            _lastUpdateMs = now;
            _strongCount = 0;
            return _filteredRssi;
        }
        _lastUpdateMs = now;

        // 关键优化 2: 不断连回座快速捕获
        // 当用户走远锁屏但蓝牙未断连（_filteredRssi 处于深衰落区），随后走回桌前连续 2 帧测得近场强信号 (>= -68dBm)
        // 立即快速收敛对齐，彻底消除从 -88dBm 缓慢爬坡 3 秒才亮屏的滞后感
        if (rawRssi >= -68.0f && _filteredRssi < -72.0f) {
            _strongCount++;
            if (_strongCount >= 2) {
                _filteredRssi = rawRssi;
                _strongCount = 0;
                return _filteredRssi;
            }
        } else {
            _strongCount = 0;
        }

        // 非对称毛刺限幅：
        // 下跌限幅 -18 dBm（抑制室内多径效应与转身人体吸收引起的瞬态深衰落，防误锁）
        // 上升限幅 +28 dBm（允许人走回工位时信号快速拉升）
        float delta = rawRssi - _filteredRssi;
        if (delta > 28.0f) delta = 28.0f;
        if (delta < -18.0f) delta = -18.0f;

        // 非对称双速率滤波：靠近敏捷 (alphaRise = 0.55)，离开稳健 (alphaFall = 0.22)
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
    float    _alphaRise;
    float    _alphaFall;
    float    _filteredRssi;
    bool     _initialized;
    uint32_t _lastUpdateMs;
    uint8_t  _strongCount;
};
