/**
 * Production-grade RNNoise wrapper with double-pass processing.
 *
 * Key techniques for Krisp-level silence:
 *   1. Double-pass RNNoise: Two independent DenoiseState instances process
 *      each frame in series. Pass 1 removes most noise; pass 2 catches
 *      residual artifacts the first pass missed.
 *   2. Asymmetric gate timing: Gate closes FAST (noise cut in ~20ms) but
 *      opens SLOWLY (voice fades in over ~120ms), preventing word clipping.
 *   3. Hard noise floor: Samples below -54dB are clamped to exactly zero,
 *      eliminating the faint hiss that survives RNNoise + gating.
 *   4. Comfort noise: Tiny shaped noise during full silence so the channel
 *      doesn't sound "dead" to the listener.
 */

#include "rnnoise_wrapper.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "rnnoise.h"

namespace noiseguard {

/*
 * Gate ATTACK coefficient (closing = cutting noise).
 * 0.25 at 10ms frame rate -> gate closes in ~2 frames (~20ms).
 * Fast attack = noise gets silenced almost instantly.
 */
static constexpr float kGateAttackCoeff = 0.25f;

/*
 * Gate RELEASE coefficient (opening = letting voice through).
 * 0.04 at 10ms frame rate -> gate opens over ~12 frames (~120ms).
 * Slow release = voice fades in naturally, no clipped word onsets.
 */
static constexpr float kGateReleaseCoeff = 0.04f;

/*
 * Minimum gate gain: TRUE ZERO.
 * When the gate is fully closed, output is absolute silence.
 * Comfort noise is added separately after the gate.
 */
static constexpr float kMinGateGain = 0.0f;

/*
 * Comfort noise level (RMS). Kept very low; disabled by default via
 * comfortNoiseEnabled_ so we get true digital silence unless explicitly
 * enabled for testing.
 */
static constexpr float kComfortNoiseLevel = 0.0003f;

/*
 * Hysteresis band for VAD gating (wider = less flicker).
 */
static constexpr float kVadHysteresis = 0.15f;

/*
 * Hard noise floor. Samples with absolute value below this are zeroed.
 * We keep this independent of the gate so that when the gate is closed
 * we still clamp tiny residuals to EXACT zero (no hiss/buzz at rest).
 */
static constexpr float kNoiseFloor = 0.003f;

/* ─── Lifecycle ─────────────────────────────────────────────────────────── */

RNNoiseWrapper::RNNoiseWrapper() = default;

RNNoiseWrapper::~RNNoiseWrapper() { destroy(); }

bool RNNoiseWrapper::init() {
  if (state_) destroy();
  state_ = rnnoise_create(nullptr);
  state2_ = rnnoise_create(nullptr);
  smoothGain_ = 1.0f;
  noiseState_ = 0x12345678;
  metrics_.framesProcessed.store(0, std::memory_order_relaxed);
  metrics_.inputRms.store(0.0f, std::memory_order_relaxed);
  metrics_.outputRms.store(0.0f, std::memory_order_relaxed);
  metrics_.vadProbability.store(0.0f, std::memory_order_relaxed);
  metrics_.currentGain.store(1.0f, std::memory_order_relaxed);
  return state_ != nullptr && state2_ != nullptr;
}

void RNNoiseWrapper::destroy() {
  if (state_) {
    rnnoise_destroy(state_);
    state_ = nullptr;
  }
  if (state2_) {
    rnnoise_destroy(state2_);
    state2_ = nullptr;
  }
}

/* ─── Core Processing ───────────────────────────────────────────────────── */

float RNNoiseWrapper::processFrame(float* frame) {
  if (!state_ || !state2_) return 0.0f;

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

  /* ── 3. RNNoise pass 1 (primary denoising) ── */
  float vad1 = rnnoise_process_frame(state_, frame, frame);

  /* ── 4. RNNoise pass 2 (catch residual noise) ── */
  float vad2 = rnnoise_process_frame(state2_, frame, frame);

  /* Use the higher VAD confidence from either pass. */
  float vad = std::max(vad1, vad2);
  metrics_.vadProbability.store(vad, std::memory_order_relaxed);

  /* Convert back to [-1.0, 1.0]. */
  constexpr float kInvScale = 1.0f / 32767.0f;
  for (size_t i = 0; i < kRNNoiseFrameSize; i++) {
    frame[i] *= kInvScale;
  }

  /* ── 5. Blend with original based on suppression level ── */
  if (level < 1.0f) {
    float dry = 1.0f - level;
    for (size_t i = 0; i < kRNNoiseFrameSize; i++) {
      frame[i] = frame[i] * level + original[i] * dry;
    }
  }

  /* ── 6. VAD-based noise gate ── */
  float vadThresh = vadThreshold_.load(std::memory_order_relaxed);

  float targetGain;
  if (vad >= vadThresh) {
    targetGain = 1.0f;
  } else if (vad < vadThresh - kVadHysteresis) {
    float ratio = vad / std::max(vadThresh - kVadHysteresis, 0.01f);
    targetGain = kMinGateGain + ratio * (1.0f - kMinGateGain);
    targetGain = std::max(targetGain, kMinGateGain);
  } else {
    float ratio = (vad - (vadThresh - kVadHysteresis)) / kVadHysteresis;
    targetGain = kMinGateGain + ratio * (1.0f - kMinGateGain);
  }

  /* ── 7. Asymmetric gain smoothing (fast close, slow open) ── */
  float coeff = (targetGain < smoothGain_) ? kGateAttackCoeff : kGateReleaseCoeff;
  smoothGain_ += coeff * (targetGain - smoothGain_);
  smoothGain_ = std::clamp(smoothGain_, kMinGateGain, 1.0f);
  metrics_.currentGain.store(smoothGain_, std::memory_order_relaxed);

  /* Apply gate gain. */
  for (size_t i = 0; i < kRNNoiseFrameSize; i++) {
    frame[i] *= smoothGain_;
  }

  /* ── 8. Hard noise floor clamp (force true silence) ── */
  float floorThresh = kNoiseFloor;
  for (size_t i = 0; i < kRNNoiseFrameSize; i++) {
    if (std::abs(frame[i]) < floorThresh) {
      frame[i] = 0.0f;
    }
  }

  /* ── 9. Comfort noise (when gated low) ── */
  if (comfortNoiseEnabled_.load(std::memory_order_relaxed) &&
      smoothGain_ < 0.1f) {
    float comfortScale = (0.1f - smoothGain_) / 0.1f;
    for (size_t i = 0; i < kRNNoiseFrameSize; i++) {
      frame[i] += comfortNoiseSample() * comfortScale;
    }
  }

  /* ── 10. Final output RMS ── */
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
  noiseState_ ^= noiseState_ << 13;
  noiseState_ ^= noiseState_ >> 17;
  noiseState_ ^= noiseState_ << 5;
  float sample = static_cast<float>(static_cast<int32_t>(noiseState_)) /
                 2147483648.0f;
  return sample * kComfortNoiseLevel;
}

}  // namespace noiseguard
