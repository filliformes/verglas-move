// Native tests for the Verglas wrapper. The wrapper is included directly so
// its internals can be checked.
//
// Build + run: bash scripts/test.sh

#include "../src/clouds_move.cpp"

#include <algorithm>
#include <string>

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond, ...) do { \
    ++g_checks; \
    if (!(cond)) { \
        ++g_failures; \
        printf("FAIL %s:%d: ", __FILE__, __LINE__); \
        printf(__VA_ARGS__); \
        printf("\n"); \
    } \
} while (0)

static host_api_v1_t g_test_host;

static audio_fx_api_v2_t *api() {
    static audio_fx_api_v2_t *a = NULL;
    if (!a) {
        memset(&g_test_host, 0, sizeof(g_test_host));
        g_test_host.sample_rate = 44100;
        g_test_host.frames_per_block = 128;
        a = move_audio_fx_init_v2(&g_test_host);
    }
    return a;
}

static std::string get(void *inst, const char *key) {
    char buf[4096];
    int n = api()->get_param(inst, key, buf, sizeof(buf));
    if (n < 0) return "<none>";
    return std::string(buf, (size_t)n);
}

static void set(void *inst, const char *key, const char *val) {
    api()->set_param(inst, key, val);
}

struct Source {
    double t = 0;
    void fill(int16_t *buf, int frames, float amp = 0.5f) {
        for (int i = 0; i < frames; ++i) {
            buf[2 * i] = (int16_t)(amp * sin(t * 2 * M_PI * 220.0) * 32767);
            buf[2 * i + 1] = (int16_t)(amp * sin(t * 2 * M_PI * 331.0) * 32767);
            t += 1.0 / 44100.0;
        }
    }
};

struct Stats {
    double sum_sq = 0;
    int peak = 0;
    long n = 0;
    int clipped = 0;  // samples at the int16 rails
    void add(const int16_t *buf, int frames) {
        for (int i = 0; i < 2 * frames; ++i) {
            sum_sq += (double)buf[i] * buf[i];
            peak = std::max(peak, abs(buf[i]));
            if (buf[i] >= 32767 || buf[i] <= -32767) ++clipped;
            ++n;
        }
    }
    double rms() const { return n ? sqrt(sum_sq / n) / 32768.0 : 0.0; }
};

static void run(void *inst, Source *src, float seconds, Stats *st = NULL,
                float amp = 0.5f) {
    int blocks = (int)(seconds * 44100 / 128);
    int16_t buf[256];
    for (int b = 0; b < blocks; ++b) {
        src->fill(buf, 128, amp);
        api()->process_block(inst, buf, 128);
        if (st) st->add(buf, 128);
    }
}

static void test_modes() {
    const char *modes[] = { "Granular", "Stretch", "Looper", "Spectral" };
    const char *quality[] = { "16-bit", "Lo-Fi" };
    for (const char *q : quality) {
        for (const char *m : modes) {
            void *inst = api()->create_instance("", NULL);
            set(inst, "mode", m);
            set(inst, "quality", q);
            set(inst, "dry_wet", "1");
            set(inst, "reverb", "0.5");
            // Position reads across the whole buffer (about 15 s in Lo-Fi):
            // stay near the write head so a short test hears grains.
            set(inst, "position", "0.05");
            Source src;
            Stats st;
            run(inst, &src, 1.0f);
            run(inst, &src, 1.0f, &st);
            CHECK(st.rms() > 0.005, "%s/%s: wet output silent (rms %f)", m, q, st.rms());
            api()->destroy_instance(inst);
        }
    }
}

static void test_mix_zero_is_transparent() {
    void *inst = api()->create_instance("", NULL);
    set(inst, "dry_wet", "0");
    Source a, b;
    int16_t buf[256], ref[256];
    run(inst, &a, 0.3f);
    b = a;
    int max_diff = 0;
    for (int i = 0; i < 200; ++i) {
        a.fill(buf, 128);
        b.fill(ref, 128, 0.5f);
        api()->process_block(inst, buf, 128);
        for (int k = 0; k < 256; ++k) max_diff = std::max(max_diff, abs(buf[k] - ref[k]));
    }
    CHECK(max_diff <= 2, "Mix 0 with the Filters page off should pass the input (max diff %d)",
          max_diff);
    api()->destroy_instance(inst);
}

// The old output stage clamped to int16 right after the mix, so a hot wet
// signal hard-clipped before the limiter could round it off.
static void test_limiter_catches_overs() {
    void *inst = api()->create_instance("", NULL);
    set(inst, "dry_wet", "0.6");
    set(inst, "mode", "Looper");
    set(inst, "pitch", "24");  // +6 dB pitch compensation on the wet side
    set(inst, "limiter_on", "On");
    Source src;
    Stats st;
    run(inst, &src, 1.0f, NULL, 0.95f);
    run(inst, &src, 2.0f, &st, 0.95f);
    printf("  hot input, limiter on: peak %d, %d samples at the rails\n", st.peak, st.clipped);
    CHECK(st.clipped == 0, "limiter on: nothing should reach the int16 rails (%d did)", st.clipped);
    api()->destroy_instance(inst);
}

// Max sample-to-sample step of the left output, to catch clicks.
static int max_step(void *inst, Source *src, int blocks, void (*mid)(void *), int at) {
    int16_t buf[256];
    int prev = 0, worst = 0;
    bool first = true;
    for (int b = 0; b < blocks; ++b) {
        if (b == at && mid) mid(inst);
        src->fill(buf, 128, 0.05f);
        api()->process_block(inst, buf, 128);
        for (int i = 0; i < 128; ++i) {
            if (!first) worst = std::max(worst, abs(buf[2 * i] - prev));
            prev = buf[2 * i];
            first = false;
        }
    }
    return worst;
}

static void hpf_on(void *inst) { set(inst, "filter_hp", "1000"); }
static void lpf_on(void *inst) { set(inst, "filter_lp", "1000"); }
static void limiter_hot(void *inst) { set(inst, "limiter_on", "On"); set(inst, "limiter_pre", "6"); }
static void shelf_on(void *inst) { set(inst, "low_boost", "6"); }
static void post_jump(void *inst) { set(inst, "limiter_on", "On"); set(inst, "limiter_post", "-6"); }

static void test_click_free_output_stage() {
    struct { const char *name; void (*fn)(void *); int limit; } cases[] = {
        { "HPF 0 -> 1 kHz", hpf_on, 120 },
        { "LPF off -> 1 kHz", lpf_on, 120 },
        { "Limiter on, +6 dB pre", limiter_hot, 160 },
        { "Low Boost 0 -> 6 dB", shelf_on, 120 },
        { "Post Gain 0 -> -6 dB", post_jump, 120 },
    };
    for (auto &c : cases) {
        void *inst = api()->create_instance("", NULL);
        set(inst, "dry_wet", "0");
        Source src;
        max_step(inst, &src, 50, NULL, 0);
        int worst = max_step(inst, &src, 200, c.fn, 100);
        // 220 Hz at 0.05 FS: natural slope ~52 per sample.
        printf("  %-24s max step %d\n", c.name, worst);
        CHECK(worst < c.limit, "%s should not click (max step %d)", c.name, worst);
        api()->destroy_instance(inst);
    }
}

static std::string prettify(const std::string &json) {
    std::string out;
    for (char ch : json) {
        out += ch;
        if (ch == ':') out += " ";
        if (ch == ',' || ch == '{') out += "\n    ";
    }
    return out;
}

static void test_state_roundtrip() {
    void *a = api()->create_instance("", NULL);
    set(a, "position", "0.13");
    set(a, "pitch", "-7");
    set(a, "mode", "Spectral");
    set(a, "quality", "Lo-Fi");
    set(a, "freeze", "On");
    set(a, "stereo_spread", "0.4");
    set(a, "filter_hp", "250");
    set(a, "filter_lp", "6000");
    set(a, "limiter_on", "On");
    set(a, "limiter_pre", "3.5");
    set(a, "low_boost", "4.5");
    set(a, "low_freq", "180");
    set(a, "low_q", "1.3");
    std::string state = get(a, "state");
    void *b = api()->create_instance("", NULL);
    set(b, "state", prettify(state).c_str());
    CHECK(get(b, "state") == state, "state round trip:\n%s\n%s", state.c_str(), get(b, "state").c_str());
    const char *garbage[] = { "", "{", "nonsense", "{\"pitch\": 5}" };
    for (const char *g : garbage) set(b, "state", g);
    CHECK(get(b, "pitch") == "-7", "garbage state must not change values");
    api()->destroy_instance(a);
    api()->destroy_instance(b);
}

// Schwung 1.x: metadata from module.json, params by key, no overlay keys.
static void test_param_contract() {
    void *inst = api()->create_instance("", NULL);
    CHECK(get(inst, "chain_params") == "<none>", "chain_params falls back to module.json");
    CHECK(get(inst, "ui_hierarchy") == "<none>", "ui_hierarchy falls back to module.json");
    CHECK(get(inst, "knob_1_name") == "<none>", "knob_N_name is not served");
    std::string before = get(inst, "state");
    set(inst, "_level", "Filters");
    set(inst, "knob_1_adjust", "5");
    CHECK(get(inst, "state") == before, "knob_N_adjust / _level must not change anything");
    set(inst, "mode", "Looper");
    CHECK(get(inst, "mode") == "Looper", "mode by name");
    set(inst, "mode", "3");
    CHECK(get(inst, "mode") == "Spectral", "mode by index");
    CHECK(get(inst, "nonexistent") == "<none>", "unknown keys return -1");
    api()->destroy_instance(inst);
}

int main() {
    printf("Verglas native tests\n");
    test_modes();
    test_mix_zero_is_transparent();
    test_limiter_catches_overs();
    test_click_free_output_stage();
    test_state_roundtrip();
    test_param_contract();
    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
