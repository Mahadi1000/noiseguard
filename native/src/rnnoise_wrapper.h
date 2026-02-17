/**
 * RNNoise wrapper for production-grade noise suppression.
 *
 * RNNoise processes exactly 480 float samples per frame (10ms @ 48kHz).
 * This wrapper adds:
 *   - VAD-based noise gate: when RNNoise reports low voice probability,
 *     apply additional attenuation to silence non-speech sounds.
 *   - Soft gain transitions: smooth gain changes over frames to avoid clicks.
 *   - Comfort noise: inject low-level noise during silence to avoid
 *     the unnatural "dead air" effect.
 *   - Metrics: input RMS, output RMS, VAD probability, frame counter.
 *
 * REAL-TIME RULES:
 * - processFrame() does NO allocations -- pure arithmetic, fixed loops.
 * - setSuppressionLevel() / setVadThreshold() are lock-free (atomic store).
 * - init() and destroy() are NOT real-time safe.
 */

#ifndef NOISEGUARD_RNNOISE_WRAPPER_H
#define NOISEGUARD_RNNOISE_WRAPPER_H

#include <atomic>
#include <cstddef>
#include <cstdint>

/* Forward-declare RNNoise opaque type. */
struct DenoiseState;

namespace noiseguard {

/* RNNoise operates on exactly 480 samples per frame (10ms at 48kHz). */
static constexpr size_t kRNNoiseFrameSize = 480;

/**
 * Real-time metrics exposed to the UI via atomic reads.
 * All fields are updated every frame from the processing thread.
 */
struct AudioMetrics {
  std::atomic<float> inputRms{0.0f};       /* Pre-processing RMS [0..1] */
  std::atomic<float> outputRms{0.0f};      /* Post-processing RMS [0..1] */
  std::atomic<float> vadProbability{0.0f}; /* Voice activity [0..1] */
  std::atomic<float> currentGain{1.0f};    /* Applied gate gain [0..1] */
  std::atomic<uint64_t> framesProcessed{0};
};

class RNNoiseWrapper {
 public:
  RNNoiseWrapper();
  ~RNNoiseWrapper();

  RNNoiseWrapper(const RNNoiseWrapper&) = delete;
  RNNoiseWrapper& operator=(const RNNoiseWrapper&) = delete;

  /** Initialize the RNNoise state. Returns true on success. */
  bool init();

  /** Destroy the RNNoise state. */
  void destroy();

  /**
   * Process a single frame IN-PLACE. frame must point to kRNNoiseFrameSize floats.
   *
   * Pipeline per frame:
   *   1. Measure input RMS
   *   2. Run RNNoise (returns VAD probability)
   *   3. Apply VAD-based noise gate with soft gain transition
   *   4. Optionally inject comfort noise during silence
   *   5. Measure output RMS, update metrics
   *
   * Returns the RNNoise VAD probability [0.0, 1.0].
   */
  float processFrame(float* frame);

  /** Set suppression level [0.0 = bypass, 1.0 = full]. Thread-safe. */
  void setSuppressionLevel(float level);
  float getSuppressionLevel() const;

  /**
   * Set VAD gate threshold [0.0 .. 1.0].
   * Frames with VAD below this are attenuated toward silence.
   * Default: 0.5. Higher = more aggressive gating.
   */
  void setVadThreshold(float threshold);
  float getVadThreshold() const;

  /** Enable/disable comfort noise injection during gated silence. */
  void setComfortNoise(bool enabled);

  bool isInitialized() const { return state_ != nullptr; }

  /** Access real-time metrics (lock-free atomic reads). */
  const AudioMetrics& metrics() const { return metrics_; }

 private:
  DenoiseState* state_ = nullptr;

  /* Suppression level [0..1]. Atomic for lock-free UI updates. */
  std::atomic<float> suppressionLevel_{1.0f};

  /* VAD gate threshold. Frames with VAD < threshold get attenuated. */
  std::atomic<float> vadThreshold_{0.5f};

  /* Comfort noise toggle. */
  std::atomic<bool> comfortNoiseEnabled_{true};

  /**
   * Smoothed gate gain. Transitions slowly to avoid clicks.
   * Updated each frame toward the target gain derived from VAD.
   * NOT atomic -- only touched from the processing thread.
   */
  float smoothGain_ = 1.0f;

  /* Simple LFSR for comfort noise generation (no allocation). */
  uint32_t noiseState_ = 0x12345678;

  /* Metrics updated every frame. */
  AudioMetrics metrics_;

  /* Generate a single comfort noise sample [-1, 1] scaled to a very low level. */
  float comfortNoiseSample();

  /* Compute RMS of a buffer. */
  static float computeRms(const float* buf, size_t len);
};

}  // namespace noiseguard

#endif  // NOISEGUARD_RNNOISE_WRAPPER_H
