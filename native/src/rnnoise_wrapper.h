/**
 * RNNoise wrapper with production-grade post-processing pipeline.
 *
 * RNNoise processes exactly 480 float samples per frame (10ms @ 48kHz).
 * This wrapper adds a multi-stage post-processing chain on top:
 *
 *   1. Double-pass RNNoise (two DenoiseState instances in series).
 *   2. Biquad HPF (80 Hz) + LPF (8 kHz) to remove hum and HF hiss.
 *   3. Adaptive noise gate that learns the room's noise floor and
 *      uses VAD + energy to decide when to silence the output.
 *   4. Spectral floor clamp: forces residual noise below an adaptive
 *      threshold to exact zero when VAD is low.
 *   5. Soft silence: injects shaped comfort noise at -60 dBFS when the
 *      gate is closed, preventing ear fatigue and channel "dead air".
 *   6. Real-time metrics (input/output RMS, VAD, gate gain, noise floor).
 *
 * REAL-TIME RULES:
 * - processFrame() does NO allocations -- pure arithmetic, fixed loops.
 * - setSuppressionLevel() / setVadThreshold() are lock-free (atomic store).
 * - init() and destroy() are NOT real-time safe.
 */

#ifndef AINOICEGUARD_RNNOISE_WRAPPER_H
#define AINOICEGUARD_RNNOISE_WRAPPER_H

#include <atomic>
#include <cstddef>
#include <cstdint>

/* Forward-declare RNNoise opaque type. */
struct DenoiseState;

namespace ainoiceguard {

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
  std::atomic<float> noiseFloor{0.0f};     /* Learned noise floor RMS */
  std::atomic<uint64_t> framesProcessed{0};
};

/**
 * 2nd-order IIR biquad filter (Direct Form I).
 * Two instances are used: one HPF at 80 Hz, one LPF at 8 kHz.
 * Coefficients are pre-computed for 48 kHz in initFilters().
 * No allocations; state lives in fixed member variables.
 */
struct BiquadState {
  float b0 = 1.f, b1 = 0.f, b2 = 0.f;  /* feedforward (numerator) */
  float a1 = 0.f, a2 = 0.f;              /* feedback (denominator), a0 = 1 */
  float x1 = 0.f, x2 = 0.f;             /* input delay line */
  float y1 = 0.f, y2 = 0.f;             /* output delay line */

  void reset() { x1 = x2 = y1 = y2 = 0.f; }

  inline float process(float x) {
    float y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
    x2 = x1; x1 = x;
    y2 = y1; y1 = y;
    return y;
  }
};

class RNNoiseWrapper {
 public:
  RNNoiseWrapper();
  ~RNNoiseWrapper();

  RNNoiseWrapper(const RNNoiseWrapper&) = delete;
  RNNoiseWrapper& operator=(const RNNoiseWrapper&) = delete;

  /** Initialize RNNoise states, filters, and gate state. */
  bool init();

  /** Destroy RNNoise states. */
  void destroy();

  /**
   * Process a single frame IN-PLACE. frame must point to kRNNoiseFrameSize floats.
   *
   * Full pipeline (all real-time safe):
   *   1.  Measure input RMS
   *   2.  Double-pass RNNoise (primary + residual suppression)
   *   3.  Blend with original based on suppression level
   *   4.  Biquad HPF (80 Hz) + LPF (8 kHz)
   *   5.  Compute post-filter RMS for adaptive noise floor
   *   6.  Update adaptive noise floor estimate (EMA, calibrates in ~2s)
   *   7.  Gate decision: VAD + energy vs adaptive threshold
   *   8.  Hold timer (keeps gate open briefly after speech ends)
   *   9.  Asymmetric gain smoothing (fast close, slow open)
   *   10. Apply gate gain
   *   11. Spectral floor clamp (force residuals to zero when VAD low)
   *   12. Soft silence injection (shaped -60 dBFS noise when gate closed)
   *   13. Measure output RMS, update metrics
   *
   * Returns the RNNoise VAD probability [0.0, 1.0].
   */
  float processFrame(float* frame);

  /** Set suppression level [0.0 = bypass, 1.0 = full]. Thread-safe. */
  void setSuppressionLevel(float level);
  float getSuppressionLevel() const;

  /** Set VAD gate threshold [0.0 .. 1.0]. Default: 0.65. Thread-safe. */
  void setVadThreshold(float threshold);
  float getVadThreshold() const;

  /** Enable/disable soft silence injection during gated silence. */
  void setComfortNoise(bool enabled);

  bool isInitialized() const { return state_ != nullptr; }

  /** Access real-time metrics (lock-free atomic reads). */
  const AudioMetrics& metrics() const { return metrics_; }

 private:
  /* ── RNNoise instances (double-pass) ── */
  DenoiseState* state_ = nullptr;
  DenoiseState* state2_ = nullptr;

  /* ── User-configurable parameters (atomic for lock-free UI access) ── */
  std::atomic<float> suppressionLevel_{1.0f};
  std::atomic<float> vadThreshold_{0.65f};
  std::atomic<bool> comfortNoiseEnabled_{true};

  /* ── Gate state (processing thread only -- NOT atomic) ── */
  float smoothGain_ = 1.0f;
  int holdCounter_ = 0;

  /* ── Adaptive noise floor (processing thread only) ── */
  float noiseFloorEstimate_ = 0.0f;
  uint64_t calibrationFrames_ = 0;

  /* ── Biquad filters (processing thread only) ── */
  BiquadState hpf_;   /* High-pass at 80 Hz */
  BiquadState lpf_;   /* Low-pass at 8 kHz */

  /* ── LFSR + shaping state for comfort noise ── */
  uint32_t noiseState_ = 0x12345678;
  float prevNoise_ = 0.0f;

  /* ── Metrics ── */
  AudioMetrics metrics_;

  /* ── Helper functions (all real-time safe) ── */
  void initFilters();
  void updateNoiseFloor(float postRms, float vad);
  float computeGateTarget(float vad, float postRms);
  void spectralClamp(float* frame, float vad);
  void applySoftSilence(float* frame);
  float comfortNoiseSample();

  static float computeRms(const float* buf, size_t len);
};

}  // namespace ainoiceguard

#endif  // AINOICEGUARD_RNNOISE_WRAPPER_H
