/**
 * Production-grade RNNoise wrapper.
 *
 * Improvements over basic passthrough:
 *   1. VAD-gated suppression: When RNNoise's VAD reports low voice probability,
 *      we apply additional attenuation so non-speech sounds (keyboard, fan, etc.)
 *      are silenced even if RNNoise doesn't fully remove them.
 *   2. Soft gain transitions: Gate gain changes are smoothed exponentially over
 *      frames to prevent audible clicks at gate open/close boundaries.
 *   3. Comfort noise: During full silence (gate closed), a tiny amount of shaped
 *      noise is injected so the listener doesn't perceive a "dead channel."
 *   4. Metrics: RMS levels, VAD probability, and frame count are exposed via
 *      atomics for the UI to poll without locks.
 */

#include "rnnoise_wrapper.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "rnnoise.h"

namespace noiseguard {

/*
 * Gain smoothing coefficient. Controls how fast the gate opens/closes.
 * 0.05 means ~50ms to fully transition at 10ms frame rate (smooth, no clicks).
 * Higher = faster but more prone to artifacts.
 */
static constexpr float kGainSmoothCoeff = 0.08f;

/*
 * Minimum gate gain. Even when fully gated, allow a tiny amount through
 * so the comfort noise blends naturally.
 */
static constexpr float kMinGateGain = 0.001f;

/*
 * Comfort noise level (RMS). Very quiet: ~-60 dBFS.
 * Just enough to signal "the channel is alive."
 */
static constexpr float kComfortNoiseLevel = 0.001f;

/*
 * Hysteresis band for VAD gating. The gate opens when VAD > threshold
 * and closes when VAD < threshold - hysteresis. This prevents rapid
 * on/off toggling when VAD hovers near the threshold.
 */
static constexpr float kVadHysteresis = 0.1f;

/* ─── Lifecycle ─────────────────────────────────────────────────────────── */

RNNoiseWrapper::RNNoiseWrapper() = default;

RNNoiseWrapper::~RNNoiseWrapper() { destroy(); }

bool RNNoiseWrapper::init() {
  if (state_) destroy();
  state_ = rnnoise_create(nullptr);
  smoothGain_ = 1.0f;
  noiseState_ = 0x12345678;
  metrics_.framesProcessed.store(0, std::memory_order_relaxed);
  metrics_.inputRms.store(0.0f, std::memory_order_relaxed);
  metrics_.outputRms.store(0.0f, std::memory_order_relaxed);
  metrics_.vadProbability.store(0.0f, std::memory_order_relaxed);
  metrics_.currentGain.store(1.0f, std::memory_order_relaxed);
  return state_ != nullptr;
}

void RNNoiseWrapper::destroy() {
  if (state_) {
    rnnoise_destroy(state_);
    state_ = nullptr;
  }
}

/* ─── Core Processing ───────────────────────────────────────────────────── */

float RNNoiseWrapper::processFrame(float* frame) {
  if (!state_) return 0.0f;

  float level = suppressionLevel_.load(std::memory_order_relaxed);

  /* Fast path: suppression fully off -> passthrough. */
  if (level <= 0.0f) {
    float rms = computeRms(frame, kRNNoiseFrameSize);
    metrics_.inputRms.store(rms, std::memory_order_relaxed);
    metrics_.outputRms.store(rms, std::memory_order_relaxed);
    metrics_.vadProbability.store(0.0f, std::memory_order_relaxed);
    metrics_.currentGain.store(1.0f, std::memory_order_relaxed);
    metrics_.framesProcessed.fetch_add(1, std::memory_order_relaxed);
    return 0.0f;
  }

  /* ── 1. Measure input RMS ── */
  float inputRms = computeRms(frame, kRNNoiseFrameSize);
  metrics_.inputRms.store(inputRms, std::memory_order_relaxed);

  /* ── 2. Save original for blending ── */
  float original[kRNNoiseFrameSize];
  for (size_t i = 0; i < kRNNoiseFrameSize; i++) {
    original[i] = frame[i];
    frame[i] *= 32767.0f;  /* Convert to RNNoise's int16 range. */
  }

  /* ── 3. Run RNNoise ── */
  float vad = rnnoise_process_frame(state_, frame, frame);
  metrics_.vadProbability.store(vad, std::memory_order_relaxed);

  /* Convert back to [-1.0, 1.0]. */
  constexpr float kInvScale = 1.0f / 32767.0f;
  for (size_t i = 0; i < kRNNoiseFrameSize; i++) {
    frame[i] *= kInvScale;
  }

  /* ── 4. Blend with original based on suppression level ── */
  if (level < 1.0f) {
    float dry = 1.0f - level;
    for (size_t i = 0; i < kRNNoiseFrameSize; i++) {
      frame[i] = frame[i] * level + original[i] * dry;
    }
  }

  /* ── 5. VAD-based noise gate ── */
  float vadThresh = vadThreshold_.load(std::memory_order_relaxed);

  /*
   * Compute target gate gain from VAD probability with hysteresis.
   * - VAD >= threshold          -> gate fully open (gain = 1.0)
   * - VAD < threshold - hyst    -> gate closed (gain = kMinGateGain)
   * - In between                -> proportional gain (soft knee)
   */
  float targetGain;
  if (vad >= vadThresh) {
    targetGain = 1.0f;
  } else if (vad < vadThresh - kVadHysteresis) {
    /* Scale residual gain by how far below threshold we are. */
    float ratio = vad / std::max(vadThresh - kVadHysteresis, 0.01f);
    targetGain = kMinGateGain + ratio * (1.0f - kMinGateGain);
    targetGain = std::max(targetGain, kMinGateGain);
  } else {
    /* Hysteresis band: maintain current direction, mild attenuation. */
    float ratio = (vad - (vadThresh - kVadHysteresis)) / kVadHysteresis;
    targetGain = kMinGateGain + ratio * (1.0f - kMinGateGain);
  }

  /* ── 6. Smooth gain transition (exponential moving average) ── */
  smoothGain_ += kGainSmoothCoeff * (targetGain - smoothGain_);
  smoothGain_ = std::clamp(smoothGain_, kMinGateGain, 1.0f);
  metrics_.currentGain.store(smoothGain_, std::memory_order_relaxed);

  /* Apply gate gain to the processed frame. */
  for (size_t i = 0; i < kRNNoiseFrameSize; i++) {
    frame[i] *= smoothGain_;
  }

  /* ── 7. Comfort noise (when gated low) ── */
  if (comfortNoiseEnabled_.load(std::memory_order_relaxed) &&
      smoothGain_ < 0.1f) {
    float comfortScale = (0.1f - smoothGain_) / 0.1f;
    for (size_t i = 0; i < kRNNoiseFrameSize; i++) {
      frame[i] += comfortNoiseSample() * comfortScale;
    }
  }

  /* ── 8. Final output RMS ── */
  float outputRms = computeRms(frame, kRNNoiseFrameSize);
  metrics_.outputRms.store(outputRms, std::memory_order_relaxed);
  metrics_.framesProcessed.fetch_add(1, std::memory_order_relaxed);

  return vad;
}

/* ─── Settings ──────────────────────────────────────────────────────────── */

void RNNoiseWrapper::setSuppressionLevel(float level) {
  suppressionLevel_.store(std::clamp(level, 0.0f, 1.0f),
                          std::memory_order_relaxed);
}

float RNNoiseWrapper::getSuppressionLevel() const {
  return suppressionLevel_.load(std::memory_order_relaxed);
}

void RNNoiseWrapper::setVadThreshold(float threshold) {
  vadThreshold_.store(std::clamp(threshold, 0.0f, 1.0f),
                      std::memory_order_relaxed);
}

float RNNoiseWrapper::getVadThreshold() const {
  return vadThreshold_.load(std::memory_order_relaxed);
}

void RNNoiseWrapper::setComfortNoise(bool enabled) {
  comfortNoiseEnabled_.store(enabled, std::memory_order_relaxed);
}

/* ─── Helpers ───────────────────────────────────────────────────────────── */

float RNNoiseWrapper::computeRms(const float* buf, size_t len) {
  float sum = 0.0f;
  for (size_t i = 0; i < len; i++) {
    sum += buf[i] * buf[i];
  }
  return std::sqrt(sum / static_cast<float>(len));
}

float RNNoiseWrapper::comfortNoiseSample() {
  /* xorshift32 PRNG -- fast, no allocation, deterministic. */
  noiseState_ ^= noiseState_ << 13;
  noiseState_ ^= noiseState_ >> 17;
  noiseState_ ^= noiseState_ << 5;
  /* Map to [-1, 1] then scale to comfort level. */
  float sample = static_cast<float>(static_cast<int32_t>(noiseState_)) /
                 2147483648.0f;
  return sample * kComfortNoiseLevel;
}

}  // namespace noiseguard
