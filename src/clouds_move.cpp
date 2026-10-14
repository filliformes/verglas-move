// Schwung Clouds — Mutable Instruments Clouds granular processor as audio FX
// MIT License (Emilie Gillet DSP, community port)
//
// Wraps Clouds GranularProcessor into Schwung audio_fx_api_v2 interface.
// Audio: 44100 Hz, 128 frames/block, stereo interleaved int16.
//
// Knob pages, labels and units come from module.json (chain_params +
// ui_hierarchy). Schwung >= 1.0 reads and writes params by key and never sends
// _level or knob_N_* to an audio FX, so there is no knob-overlay layer here.
// The output stage (mix, low shelf, HPF/LPF, limiter) runs in float and is
// clamped to int16 once, at the very end.

#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <cmath>

#include "clouds/dsp/granular_processor.h"
#include "stmlib/utils/random.h"

// ---- Random state (global, shared across instances) ----
namespace stmlib { uint32_t Random::rng_state_ = 0x12345678; }

// ---- Schwung API headers (inline, must match chain_host ABI exactly) ----
extern "C" {

typedef int (*move_mod_emit_value_fn)(void *ctx,
                                      const char *source_id,
                                      const char *target,
                                      const char *param,
                                      float signal,
                                      float depth,
                                      float offset,
                                      int bipolar,
                                      int enabled);
typedef void (*move_mod_clear_source_fn)(void *ctx, const char *source_id);

typedef struct host_api_v1 {
    uint32_t api_version;
    int sample_rate;
    int frames_per_block;
    uint8_t *mapped_memory;
    int audio_out_offset;
    int audio_in_offset;
    void (*log)(const char *msg);
    int (*midi_send_internal)(const uint8_t *msg, int len);
    int (*midi_send_external)(const uint8_t *msg, int len);
    int (*get_clock_status)(void);
    move_mod_emit_value_fn mod_emit_value;
    move_mod_clear_source_fn mod_clear_source;
    void *mod_host_ctx;
    // Appended by newer hosts (unused here; listed so the layout matches).
    float (*get_bpm)(void);
    int (*midi_inject_to_move)(const uint8_t *msg, int len);
    int (*slot_recv_channel)(void *instance);
    double (*get_beat_position)(void);
    void *reserved[8];
} host_api_v1_t;

typedef struct audio_fx_api_v2 {
    uint32_t api_version;
    void* (*create_instance)(const char *module_dir, const char *config_json);
    void  (*destroy_instance)(void *instance);
    void  (*process_block)(void *instance, int16_t *audio_inout, int frames);
    void  (*set_param)(void *instance, const char *key, const char *val);
    int   (*get_param)(void *instance, const char *key, char *buf, int buf_len);
    void  (*on_midi)(void *instance, const uint8_t *msg, int len, int source);
} audio_fx_api_v2_t;

}  // extern "C"

// ---- Buffer sizes scaled for 44.1k (original was 32k) ----
// Stereo recording uses kSmallBufferSize per channel; the rest of the large
// buffer is FX workspace (diffuser 8K + reverb 64K + correlator 1.5K), so the
// large buffer is small + 80K. Was 163840 before the reverb was rescaled.
static const size_t kLargeBufferSize = 172032;
static const size_t kSmallBufferSize = 90112;

// ---- Instance state ----
struct CloudsInstance {
    clouds::GranularProcessor processor;

    uint8_t large_buffer[kLargeBufferSize];
    uint8_t small_buffer[kSmallBufferSize];

    // Parameters (knob-mapped)
    float position;     // 0-1
    float size;         // 0-1
    int   pitch;        // -24 to +24 semitones
    float density;      // 0-1
    float texture;      // 0-1
    float feedback;     // 0-1
    float reverb;       // 0-1
    float dry_wet;      // 0-1

    // Jog menu params
    int   mode;           // 0-3 (Granular/Stretch/Looper/Spectral)
    bool  freeze;         // toggle
    int   quality;        // 0=16-bit, 1=Lo-Fi
    float stereo_spread;  // 0-1

    // Filter params (one-pole 6dB/oct, Mutable Instruments ONE_POLE style)
    float filter_hp;      // HPF cutoff: 0 Hz to 1000 Hz (0 = off)
    float filter_lp;      // LPF cutoff: 20000 Hz to 1000 Hz (20000 = off)
    float hp_state_l;     // HPF state (left)
    float hp_state_r;     // HPF state (right)
    float lp_state_l;     // LPF state (left)
    float lp_state_r;     // LPF state (right)

    // Low-shelf EQ params (before HPF/LPF)
    float low_boost;      // 0 to 6 dB (default 0 = off)
    float low_freq;       // 30 to 400 Hz (default 100)
    float low_q;          // 0.1 to 4.0 (default 0.7)
    // Biquad state (Direct Form II Transposed)
    float ls_z1_l, ls_z2_l;  // left channel state
    float ls_z1_r, ls_z2_r;  // right channel state
    // Cached biquad coefficients
    float ls_b0, ls_b1, ls_b2, ls_a1, ls_a2;
    float ls_last_boost, ls_last_freq, ls_last_q;  // for recalc detection

    // Limiter params
    bool  limiter_on;     // toggle (default OFF)
    float limiter_pre;    // -6 to +6 dB (default 0)
    float limiter_post;   // -6 to +6 dB (default 0)

    // Smoothed parameter state
    float texture_smooth;  // one-pole smoothed texture (100ms)

    // Output stage, smoothed once per block / ramped per sample so no control
    // ever jumps (and no stage is ever bypassed, so none restarts from stale
    // state when it engages).
    float hp_s, lp_s;                 // cutoffs (Hz)
    float boost_s, freq_s, q_s;       // low shelf
    float pre_s, post_s;              // limiter gains (dB)
    float pre_gain, post_gain;        // linear, per-sample ramps
    float limiter_mix;                // 0 = bypass, 1 = limited
    float dry_gain, wet_gain;         // per-sample ramps
};

static const host_api_v1_t *g_host = NULL;

// ---- Enum option names ----
static const char *MODE_NAMES[] = {
    "Granular", "Stretch", "Looper", "Spectral"
};
static const char *QUALITY_NAMES[] = { "16-bit", "Lo-Fi" };

// ---- Helpers ----
static inline float clampf(float x, float lo, float hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}

static inline int clampi(int x, int lo, int hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}

// dB to linear gain: 10^(dB/20)
static inline float db_to_lin(float db) {
    return powf(10.0f, db * 0.05f);
}

// Tape-style soft clip limiter
// Combines Airwindows IronOxide sin(min(|x|,pi/2)) waveshaper with
// Chowdhury DSP cubic soft clip (x - x^3/3) for a smooth tape saturation knee.
// Stateless, no allocation, suitable for render path.
static inline float tape_soft_clip(float x) {
    // Stage 1: Chowdhury-style cubic soft clip — gentle knee, unity slope at origin
    // f(x) = 1.5 * (x - x^3/3) for |x| <= 1, clamped to +/-1 outside
    // (degree-3 polynomial from chowdsp_SoftClipper.h, normalized)
    float ax = fabsf(x);
    if (ax > 1.5f) {
        x = (x > 0.0f) ? 1.0f : -1.0f;
    } else {
        float norm = x * 0.6666667f;  // x * 2/3 (normFactor for degree 3)
        float cn = clampf(norm, -1.0f, 1.0f);
        x = (cn - (cn * cn * cn) * 0.3333333f) * 1.5f;  // * invNormFactor (3/2)
    }

    // Stage 2: Airwindows IronOxide sine waveshaper — tape saturation character
    // sin(min(|x|, pi/2)) preserves sign, soft-clips to exactly +/-1
    float bridge = fabsf(x);
    if (bridge > 1.5707963f) bridge = 1.5707963f;  // pi/2
    bridge = sinf(bridge);
    return (x > 0.0f) ? bridge : -bridge;
}

// Low-shelf biquad coefficient update (Audio EQ Cookbook)
static void update_low_shelf_coeffs(CloudsInstance *inst, float boost, float freq, float q) {
    if (boost == inst->ls_last_boost &&
        freq  == inst->ls_last_freq  &&
        q     == inst->ls_last_q) return;

    inst->ls_last_boost = boost;
    inst->ls_last_freq  = freq;
    inst->ls_last_q     = q;

    float A = powf(10.0f, boost * 0.025f);  // 10^(dB/40); 0 dB = unity
    float w0 = 6.2831853f * freq / 44100.0f;
    float cosw0 = cosf(w0);
    float sinw0 = sinf(w0);
    float alpha = sinw0 / (2.0f * q);
    float sqrtA2alpha = 2.0f * sqrtf(A) * alpha;

    float a0 = (A + 1.0f) + (A - 1.0f) * cosw0 + sqrtA2alpha;
    float inv_a0 = 1.0f / a0;
    inst->ls_b0 = A * ((A + 1.0f) - (A - 1.0f) * cosw0 + sqrtA2alpha) * inv_a0;
    inst->ls_b1 = 2.0f * A * ((A - 1.0f) - (A + 1.0f) * cosw0)       * inv_a0;
    inst->ls_b2 = A * ((A + 1.0f) - (A - 1.0f) * cosw0 - sqrtA2alpha) * inv_a0;
    inst->ls_a1 = -2.0f * ((A - 1.0f) + (A + 1.0f) * cosw0)           * inv_a0;
    inst->ls_a2 = ((A + 1.0f) + (A - 1.0f) * cosw0 - sqrtA2alpha)     * inv_a0;
}

// ARM has no flush-to-zero by default, and -ffast-math only enables it for
// executables (crtfastmath), not for a shared library. Set FPCR.FZ for the
// duration of the block so decaying reverb / filter / feedback tails never
// produce denormals.
struct DenormalGuard {
#if defined(__aarch64__)
    uint64_t saved;
    DenormalGuard() {
        __asm__ __volatile__("mrs %0, fpcr" : "=r"(saved));
        uint64_t fz = saved | (1ull << 24);
        __asm__ __volatile__("msr fpcr, %0" : : "r"(fz));
    }
    ~DenormalGuard() {
        __asm__ __volatile__("msr fpcr, %0" : : "r"(saved));
    }
#endif
};

// ---- API callbacks ----
static void* clouds_create(const char *module_dir, const char *config_json) {
    (void)module_dir;
    (void)config_json;  // state is restored later through set_param("state")
    CloudsInstance *inst = (CloudsInstance*)calloc(1, sizeof(CloudsInstance));
    if (!inst) return NULL;

    // Init DSP
    inst->processor.Init(
        inst->large_buffer, kLargeBufferSize,
        inst->small_buffer, kSmallBufferSize);

    // Defaults
    inst->position = 0.5f;
    inst->size = 0.5f;
    inst->pitch = 0;
    inst->density = 1.0f;
    inst->texture = 0.5f;
    inst->texture_smooth = 0.5f;
    inst->feedback = 0.0f;
    inst->reverb = 0.0f;
    inst->dry_wet = 0.5f;

    inst->mode = 0;
    inst->freeze = false;
    inst->quality = 0;
    inst->stereo_spread = 1.0f;

    inst->filter_hp = 0.0f;       // HPF off (0 Hz)
    inst->filter_lp = 20000.0f;   // LPF off (20 kHz)
    inst->hp_state_l = inst->hp_state_r = 0.0f;
    inst->lp_state_l = inst->lp_state_r = 0.0f;

    inst->low_boost = 0.0f;       // Low-shelf off (0 dB)
    inst->low_freq = 100.0f;      // 100 Hz
    inst->low_q = 0.7f;           // Butterworth-ish
    inst->ls_z1_l = inst->ls_z2_l = 0.0f;
    inst->ls_z1_r = inst->ls_z2_r = 0.0f;
    inst->ls_last_boost = -999.0f; // force coefficient calc on first block

    inst->limiter_on = false;
    inst->limiter_pre = 0.0f;
    inst->limiter_post = 0.0f;

    inst->hp_s = inst->filter_hp;
    inst->lp_s = inst->filter_lp;
    inst->boost_s = inst->low_boost;
    inst->freq_s = inst->low_freq;
    inst->q_s = inst->low_q;
    inst->pre_s = inst->post_s = 0.0f;
    inst->pre_gain = inst->post_gain = 1.0f;
    inst->limiter_mix = 0.0f;
    inst->dry_gain = 1.0f - inst->dry_wet;
    inst->wet_gain = inst->dry_wet;
    update_low_shelf_coeffs(inst, inst->boost_s, inst->freq_s, inst->q_s);

    inst->processor.set_playback_mode(clouds::PLAYBACK_MODE_GRANULAR);
    inst->processor.set_quality(inst->quality * 2);  // 0=stereo 16-bit, 2=stereo lo-fi

    return inst;
}

static void clouds_destroy(void *instance) {
    CloudsInstance *inst = (CloudsInstance*)instance;
    if (inst) {
        free(inst);
    }
}

static void clouds_process(void *instance, int16_t *audio_inout, int frames) {
    CloudsInstance *inst = (CloudsInstance*)instance;
    if (!inst || frames <= 0) return;
    if (frames > 128) frames = 128;

    DenormalGuard denormal_guard;

    // Update processor settings
    inst->processor.set_playback_mode(
        static_cast<clouds::PlaybackMode>(clampi(inst->mode, 0, 3)));
    inst->processor.set_quality(inst->quality * 2);  // 0=stereo 16-bit, 2=stereo lo-fi
    inst->processor.set_freeze(inst->freeze);

    // Fill parameters
    clouds::Parameters *p = inst->processor.mutable_parameters();
    p->position = clampf(inst->position, 0.0f, 1.0f);
    p->size = clampf(inst->size, 0.0f, 1.0f);
    p->pitch = (float)inst->pitch;
    p->density = clampf(inst->density, 0.0f, 1.0f);
    // Smooth texture (one-pole, 100ms time constant at block rate)
    // coeff = 1 - exp(-1 / (tau * block_rate)) where tau=0.1s, block_rate=44100/128
    const float tex_coeff = 0.0286f;
    inst->texture_smooth += tex_coeff * (clampf(inst->texture, 0.0f, 1.0f) - inst->texture_smooth);
    p->texture = inst->texture_smooth;
    p->feedback = clampf(inst->feedback, 0.0f, 1.0f);
    p->reverb = clampf(inst->reverb, 0.0f, 1.0f);
    // Bypass Clouds internal dry/wet — its equal-power LUT attenuates dry by
    // -3dB at mix=0 (lut_xfade_out[0]=0.7071). We do our own mix after Process.
    p->dry_wet = 1.0f;
    p->stereo_spread = clampf(inst->stereo_spread, 0.0f, 1.0f);
    p->freeze = inst->freeze;
    p->trigger = false;
    p->gate = false;

    // Prepare (handles mode changes, buffer allocation, vocoder buffering)
    inst->processor.Prepare();

    // Save dry input before Process (which reads from input at the end)
    int16_t dry_buf[256];  // 128 stereo frames = 256 samples
    memcpy(dry_buf, audio_inout, frames * 2 * sizeof(int16_t));

    // Process — ShortFrame has same layout as interleaved int16 stereo pairs
    clouds::ShortFrame* input = reinterpret_cast<clouds::ShortFrame*>(audio_inout);
    clouds::ShortFrame output[128];
    inst->processor.Process(input, output, (size_t)frames);

    // Output stage, all in float, clamped to int16 once at the end (the old
    // chain clamped after the mix, so loud passages hard-clipped before the
    // limiter could round them off).
    //
    // Dry/wet is linear (unity at both extremes). The wet side compensates
    // Clouds' volume loss at non-zero pitch (grain shrinkage up, spectral bin
    // spreading down, crossfade imbalance), capped at +6 dB.
    const float abs_pitch = fabsf((float)inst->pitch);
    const float pitch_comp = fminf(1.0f + abs_pitch * 0.07f, 2.0f);
    const float mix = clampf(inst->dry_wet, 0.0f, 1.0f);
    const float inv_frames = 1.0f / (float)frames;
    const float dry_step = ((1.0f - mix) - inst->dry_gain) * inv_frames;
    const float wet_step = (mix * pitch_comp - inst->wet_gain) * inv_frames;

    // Block-rate smoothing (~10 ms) of the Filters page.
    const float k = 0.25f;
    inst->hp_s += k * (inst->filter_hp - inst->hp_s);
    inst->lp_s += k * (inst->filter_lp - inst->lp_s);
    inst->boost_s += k * (inst->low_boost - inst->boost_s);
    inst->freq_s += k * (inst->low_freq - inst->freq_s);
    inst->q_s += k * (inst->low_q - inst->q_s);
    inst->pre_s += k * (inst->limiter_pre - inst->pre_s);
    inst->post_s += k * (inst->limiter_post - inst->post_s);
    update_low_shelf_coeffs(inst, inst->boost_s, inst->freq_s, inst->q_s);

    // MI ONE_POLE coefficients: 1 - exp(-2*pi*f/sr). The HPF state always
    // tracks (at 5 Hz when off) and its subtraction fades in over the first
    // 5 Hz, so "off" is bit-transparent and engaging never starts from a
    // stale state. "LPF off" passes straight through.
    const float sr = 44100.0f;
    const float hp_coeff = 1.0f - expf(-6.2831853f * fmaxf(inst->hp_s, 5.0f) / sr);
    const float hp_amount = clampf(inst->hp_s / 5.0f, 0.0f, 1.0f);
    const float lp_coeff = inst->lp_s >= 19999.0f
        ? 1.0f : 1.0f - expf(-6.2831853f * inst->lp_s / sr);
    const float pre_step = (db_to_lin(inst->pre_s) - inst->pre_gain) * inv_frames;
    const float post_step = (db_to_lin(inst->post_s) - inst->post_gain) * inv_frames;
    const float lim_target = inst->limiter_on ? 1.0f : 0.0f;
    const float lim_step = 1.0f / (0.01f * sr);  // 10 ms crossfade

    const int16_t *wet16 = (const int16_t *)output;
    for (int i = 0; i < frames; ++i) {
        inst->dry_gain += dry_step;
        inst->wet_gain += wet_step;
        inst->pre_gain += pre_step;
        inst->post_gain += post_step;
        float lm = inst->limiter_mix;
        lm = lm < lim_target ? fminf(lm + lim_step, lim_target) : fmaxf(lm - lim_step, lim_target);
        inst->limiter_mix = lm;

        float x[2];
        for (int c = 0; c < 2; ++c) {
            float d = (float)dry_buf[2 * i + c] / 32768.0f;
            float w = (float)wet16[2 * i + c] / 32768.0f;
            x[c] = d * inst->dry_gain + w * inst->wet_gain;
        }

        // Low shelf (biquad, Direct Form II Transposed): unity at 0 dB.
        float y = inst->ls_b0 * x[0] + inst->ls_z1_l;
        inst->ls_z1_l = inst->ls_b1 * x[0] - inst->ls_a1 * y + inst->ls_z2_l;
        inst->ls_z2_l = inst->ls_b2 * x[0] - inst->ls_a2 * y;
        x[0] = y;
        y = inst->ls_b0 * x[1] + inst->ls_z1_r;
        inst->ls_z1_r = inst->ls_b1 * x[1] - inst->ls_a1 * y + inst->ls_z2_r;
        inst->ls_z2_r = inst->ls_b2 * x[1] - inst->ls_a2 * y;
        x[1] = y;

        // HPF: state tracks a low-pass, output = input - state.
        inst->hp_state_l += hp_coeff * (x[0] - inst->hp_state_l);
        x[0] -= hp_amount * inst->hp_state_l;
        inst->hp_state_r += hp_coeff * (x[1] - inst->hp_state_r);
        x[1] -= hp_amount * inst->hp_state_r;

        // LPF.
        inst->lp_state_l += lp_coeff * (x[0] - inst->lp_state_l);
        x[0] = inst->lp_state_l;
        inst->lp_state_r += lp_coeff * (x[1] - inst->lp_state_r);
        x[1] = inst->lp_state_r;

        for (int c = 0; c < 2; ++c) {
            float v = x[c];
            if (lm > 0.0f) {
                float limited = tape_soft_clip(v * inst->pre_gain) * inst->post_gain;
                v += (limited - v) * lm;
            }
            int32_t o = (int32_t)lrintf(clampf(v, -1.0f, 1.0f) * 32767.0f);
            audio_inout[2 * i + c] = (int16_t)o;
        }
    }
}

// ---- State persistence ----
// Schwung saves a module by calling get_param("state") and expects a JSON
// snapshot; it restores AFTER create_instance() via set_param("state", json).
// On a successful JSON.parse the host RE-EMITS the state PRETTY-PRINTED
// ("key": "value" with spaces/newlines after each colon) on the next save, so
// the parser must be key-based and whitespace/newline-tolerant. Every user-
// settable param must be listed here or it silently resets to its default on
// reload.
static const char *STATE_KEYS[] = {
    "position", "size", "pitch", "density", "texture", "feedback",
    "reverb", "dry_wet", "mode", "freeze", "quality", "stereo_spread",
    "filter_hp", "filter_lp", "limiter_on", "limiter_pre", "limiter_post",
    "low_boost", "low_freq", "low_q"
};
#define STATE_KEY_COUNT ((int)(sizeof(STATE_KEYS)/sizeof(STATE_KEYS[0])))

// Extract the quoted string value for "key". Searches for the fully-quoted key
// ("size" never matches inside "stereo_spread") and tolerates whitespace or
// newlines after the colon, so a pretty-printed round-trip still parses.
// Returns 0 on success (value copied into out), -1 if the key is absent.
static int clouds_json_get_str(const char *json, const char *key,
                               char *out, int out_len) {
    char search[48];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *p = strstr(json, search);
    if (!p) return -1;
    p += strlen(search);
    while (*p == ':' || *p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p != '"') return -1;
    p++;
    const char *end = strchr(p, '"');
    if (!end) return -1;
    int len = (int)(end - p);
    if (len >= out_len) len = out_len - 1;
    memcpy(out, p, (size_t)len);
    out[len] = '\0';
    return 0;
}

static void clouds_set_param(void *instance, const char *key, const char *val) {
    CloudsInstance *inst = (CloudsInstance*)instance;
    if (!inst || !key || !val) return;

    if (strcmp(key, "position") == 0) {
        inst->position = clampf(atof(val), 0.0f, 1.0f);
    } else if (strcmp(key, "size") == 0) {
        inst->size = clampf(atof(val), 0.0f, 1.0f);
    } else if (strcmp(key, "pitch") == 0) {
        inst->pitch = clampi(atoi(val), -24, 24);
    } else if (strcmp(key, "density") == 0) {
        inst->density = clampf(atof(val), 0.0f, 1.0f);
    } else if (strcmp(key, "texture") == 0) {
        inst->texture = clampf(atof(val), 0.0f, 1.0f);
    } else if (strcmp(key, "feedback") == 0) {
        inst->feedback = clampf(atof(val), 0.0f, 1.0f);
    } else if (strcmp(key, "reverb") == 0) {
        inst->reverb = clampf(atof(val), 0.0f, 1.0f);
    } else if (strcmp(key, "dry_wet") == 0) {
        inst->dry_wet = clampf(atof(val), 0.0f, 1.0f);
    } else if (strcmp(key, "mode") == 0) {
        int found = 0;
        for (int i = 0; i < 4; i++) {
            if (strcmp(val, MODE_NAMES[i]) == 0) { inst->mode = i; found = 1; break; }
        }
        if (!found) inst->mode = clampi(atoi(val), 0, 3);
    } else if (strcmp(key, "freeze") == 0) {
        inst->freeze = (strcmp(val, "On") == 0) ? true : (strcmp(val, "Off") == 0) ? false : (atoi(val) != 0);
    } else if (strcmp(key, "quality") == 0) {
        if (strcmp(val, "Lo-Fi") == 0) inst->quality = 1;
        else if (strcmp(val, "16-bit") == 0) inst->quality = 0;
        else inst->quality = clampi(atoi(val), 0, 1);
    } else if (strcmp(key, "stereo_spread") == 0) {
        inst->stereo_spread = clampf(atof(val), 0.0f, 1.0f);
    } else if (strcmp(key, "low_boost") == 0) {
        inst->low_boost = clampf(atof(val), 0.0f, 6.0f);
    } else if (strcmp(key, "low_freq") == 0) {
        inst->low_freq = clampf(atof(val), 30.0f, 400.0f);
    } else if (strcmp(key, "low_q") == 0) {
        inst->low_q = clampf(atof(val), 0.1f, 4.0f);
    } else if (strcmp(key, "filter_hp") == 0) {
        inst->filter_hp = clampf(atof(val), 0.0f, 1000.0f);
    } else if (strcmp(key, "filter_lp") == 0) {
        inst->filter_lp = clampf(atof(val), 1000.0f, 20000.0f);
    } else if (strcmp(key, "limiter_on") == 0) {
        inst->limiter_on = (strcmp(val, "On") == 0) ? true : (strcmp(val, "Off") == 0) ? false : (atoi(val) != 0);
    } else if (strcmp(key, "limiter_pre") == 0) {
        inst->limiter_pre = clampf(atof(val), -6.0f, 6.0f);
    } else if (strcmp(key, "limiter_post") == 0) {
        inst->limiter_post = clampf(atof(val), -6.0f, 6.0f);
    } else if (strcmp(key, "state") == 0) {
        // Key-based JSON restore: hand each key to the ordinary per-key handler
        // above so clamping + enum/toggle matching are reused. Tolerates
        // whitespace, newlines, reordering, and missing keys — a key that is
        // absent simply leaves that param at its current value (never aborts
        // the whole parse, unlike the old positional sscanf).
        char vb[32];
        for (int i = 0; i < STATE_KEY_COUNT; i++) {
            if (clouds_json_get_str(val, STATE_KEYS[i], vb, sizeof(vb)) == 0)
                clouds_set_param(instance, STATE_KEYS[i], vb);
        }
    }
}

static int clouds_get_param(void *instance, const char *key, char *buf, int buf_len) {
    CloudsInstance *inst = (CloudsInstance*)instance;
    if (!inst || !key || !buf || buf_len < 1) return 0;

    if (strcmp(key, "name") == 0) {
        return snprintf(buf, buf_len, "Verglas");
    } else if (strcmp(key, "ui_hierarchy") == 0 || strcmp(key, "chain_params") == 0) {
        // Single source of truth: module.json (the host falls back to it).
        return -1;
    } else if (strcmp(key, "position") == 0) {
        return snprintf(buf, buf_len, "%.4f", inst->position);
    } else if (strcmp(key, "size") == 0) {
        return snprintf(buf, buf_len, "%.4f", inst->size);
    } else if (strcmp(key, "pitch") == 0) {
        return snprintf(buf, buf_len, "%d", inst->pitch);
    } else if (strcmp(key, "density") == 0) {
        return snprintf(buf, buf_len, "%.4f", inst->density);
    } else if (strcmp(key, "texture") == 0) {
        return snprintf(buf, buf_len, "%.4f", inst->texture);
    } else if (strcmp(key, "feedback") == 0) {
        return snprintf(buf, buf_len, "%.4f", inst->feedback);
    } else if (strcmp(key, "reverb") == 0) {
        return snprintf(buf, buf_len, "%.4f", inst->reverb);
    } else if (strcmp(key, "dry_wet") == 0) {
        return snprintf(buf, buf_len, "%.4f", inst->dry_wet);
    } else if (strcmp(key, "mode") == 0) {
        return snprintf(buf, buf_len, "%s", MODE_NAMES[clampi(inst->mode, 0, 3)]);
    } else if (strcmp(key, "freeze") == 0) {
        return snprintf(buf, buf_len, "%s", inst->freeze ? "On" : "Off");
    } else if (strcmp(key, "quality") == 0) {
        return snprintf(buf, buf_len, "%s", QUALITY_NAMES[clampi(inst->quality, 0, 1)]);
    } else if (strcmp(key, "stereo_spread") == 0) {
        return snprintf(buf, buf_len, "%.4f", inst->stereo_spread);
    } else if (strcmp(key, "low_boost") == 0) {
        return snprintf(buf, buf_len, "%.1f", inst->low_boost);
    } else if (strcmp(key, "low_freq") == 0) {
        return snprintf(buf, buf_len, "%d", (int)inst->low_freq);
    } else if (strcmp(key, "low_q") == 0) {
        return snprintf(buf, buf_len, "%.1f", inst->low_q);
    } else if (strcmp(key, "filter_hp") == 0) {
        return snprintf(buf, buf_len, "%d", (int)inst->filter_hp);
    } else if (strcmp(key, "filter_lp") == 0) {
        return snprintf(buf, buf_len, "%d", (int)inst->filter_lp);
    } else if (strcmp(key, "limiter_on") == 0) {
        return snprintf(buf, buf_len, "%s", inst->limiter_on ? "On" : "Off");
    } else if (strcmp(key, "limiter_pre") == 0) {
        return snprintf(buf, buf_len, "%.4f", (double)inst->limiter_pre);
    } else if (strcmp(key, "limiter_post") == 0) {
        return snprintf(buf, buf_len, "%.4f", (double)inst->limiter_post);
    } else if (strcmp(key, "state") == 0) {
        // Full snapshot for Schwung autosave / set reload. Every value is
        // emitted as a QUOTED string (raw float text, integer, or the enum
        // option name) using the same per-key getters that a live read uses,
        // so it survives any pretty-printed re-serialization unchanged and
        // feeds straight back into set_param via the state branch above.
        if (buf_len < 2) return -1;
        int n = 0;
        buf[n++] = '{';
        for (int i = 0; i < STATE_KEY_COUNT; i++) {
            char vb[32];
            if (clouds_get_param(instance, STATE_KEYS[i], vb, sizeof(vb)) < 0)
                continue;
            int w = snprintf(buf + n, (size_t)(buf_len - n), "%s\"%s\":\"%s\"",
                             (n > 1) ? "," : "", STATE_KEYS[i], vb);
            if (w < 0 || w >= buf_len - n) return -1;   // would overflow
            n += w;
        }
        if (n + 1 >= buf_len) return -1;
        buf[n++] = '}';
        buf[n] = '\0';
        return n;
    }
    return -1;
}

// ---- Static API struct ----
static audio_fx_api_v2_t g_api = {
    .api_version = 2,
    .create_instance = clouds_create,
    .destroy_instance = clouds_destroy,
    .process_block = clouds_process,
    .set_param = clouds_set_param,
    .get_param = clouds_get_param,
    .on_midi = NULL,
};

// ---- Entry point (dlsym) ----
extern "C" {

__attribute__((visibility("default")))
audio_fx_api_v2_t* move_audio_fx_init_v2(const host_api_v1_t *host) {
    g_host = host;
    return &g_api;
}

}  // extern "C"
