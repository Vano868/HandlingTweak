// HandlingTweak - AML mod for GTA SA / SA-MP Android (32-bit & 64-bit)
// Mengalikan nilai: traksi (grip), traction loss, rem, dan steering lock
// untuk semua kendaraan. Nilai dibaca dari HandlingTweak.ini.
//
// Aman by design: semua alamat dicari otomatis & divalidasi. Kalau ada yang
// tidak cocok, mod TIDAK menulis apa pun (lihat logcat tag "HandlingTweak").

#include <mod/amlmod.h>
#include <mod/config.h>
#include <android/log.h>
#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <vector>

MYMODCFG(net.local.handlingtweak, HandlingTweak, 1.0, Local)

#define TAG "HandlingTweak"
#define LOG(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)

// ---- Offset field di dalam 1 entry handling (layout PC, divalidasi runtime) ----
static int OFF_MASS      = 0x04;
static int OFF_TRACTION  = 0x28;  // fTractionMultiplier
static int OFF_BRAKE     = 0x94;  // fBrakeDeceleration
static int OFF_STEER     = 0xA0;  // fSteeringLock
static int OFF_TRAC_LOSS = 0xA4;  // fTractionLoss

static const int MAX_ENTRIES = 210;

struct TweakConfig {
    bool  enabled = true;
    float traction = 1.0f;
    float tractionLoss = 1.0f;
    float brake = 1.0f;
    float steer = 1.0f;
    // override manual (0 = auto-detect)
    int   arrayOffset = -1;
    int   stride = 0;
    bool  managerIsPointer = false;
};

static TweakConfig g_cfg;
static char   g_cfgPath[512] = {0};
static time_t g_cfgMtime = 0;

struct Orig { float tr, tl, br, st; bool valid; };
static std::vector<Orig> g_orig(MAX_ENTRIES);

static const char* TEMPLATE =
"; HandlingTweak - nilai 1.0 = bawaan game\n"
"enabled=1\n"
"traction=1.20        ; grip ban (fTractionMultiplier)\n"
"traction_loss=1.00   ; >1 = lebih licin saat slide lebih baik, <1 sebaliknya\n"
"brake=1.30           ; kekuatan rem\n"
"steering_lock=1.00   ; sudut belok maksimal\n"
"\n"
"; --- opsional, biarkan kalau tidak perlu ---\n"
"; array_offset=0x4   ; offset array handling di cHandlingDataMgr\n"
"; stride=0xD8        ; ukuran 1 entry handling\n"
"; manager_is_pointer=0\n";

static bool GetPackageName(char* out, size_t n) {
    FILE* f = fopen("/proc/self/cmdline", "r");
    if (!f) return false;
    size_t r = fread(out, 1, n - 1, f);
    fclose(f);
    out[r] = 0;
    return r > 0;
}

static bool FileExists(const char* p, time_t* mtime) {
    struct stat st;
    if (stat(p, &st) != 0) return false;
    if (mtime) *mtime = st.st_mtime;
    return true;
}

static void FindConfigPath() {
    char pkg[256];
    if (!GetPackageName(pkg, sizeof(pkg))) strcpy(pkg, "com.rockstargames.gtasa");
    const char* fmts[] = {
        "/storage/emulated/0/Android/data/%s/files/HandlingTweak.ini",
        "/sdcard/Android/data/%s/files/HandlingTweak.ini",
    };
    for (auto f : fmts) {
        char p[512];
        snprintf(p, sizeof(p), f, pkg);
        if (FileExists(p, nullptr)) { strcpy(g_cfgPath, p); return; }
    }
    // belum ada: buat template di lokasi pertama
    snprintf(g_cfgPath, sizeof(g_cfgPath), fmts[0], pkg);
    FILE* f = fopen(g_cfgPath, "w");
    if (f) { fputs(TEMPLATE, f); fclose(f); LOG("Template config dibuat: %s", g_cfgPath); }
    else   { LOG("Gagal membuat config di %s", g_cfgPath); }
}

static void LoadConfig() {
    TweakConfig c;
    FILE* f = fopen(g_cfgPath, "r");
    if (!f) { LOG("Config tidak terbaca, pakai default (1.0)"); g_cfg = c; return; }
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char* cm = strpbrk(line, ";#");
        if (cm) *cm = 0;
        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char* key = line;
        char* val = eq + 1;
        while (*key == ' ' || *key == '\t') key++;
        for (char* e = key + strlen(key); e > key && (e[-1] == ' ' || e[-1] == '\t'); ) *--e = 0;
        if      (!strcmp(key, "enabled"))             c.enabled = atoi(val) != 0;
        else if (!strcmp(key, "traction"))            c.traction = (float)atof(val);
        else if (!strcmp(key, "traction_loss"))       c.tractionLoss = (float)atof(val);
        else if (!strcmp(key, "brake"))               c.brake = (float)atof(val);
        else if (!strcmp(key, "steering_lock"))       c.steer = (float)atof(val);
        else if (!strcmp(key, "array_offset"))        c.arrayOffset = (int)strtol(val, nullptr, 0);
        else if (!strcmp(key, "stride"))              c.stride = (int)strtol(val, nullptr, 0);
        else if (!strcmp(key, "manager_is_pointer"))  c.managerIsPointer = atoi(val) != 0;
    }
    fclose(f);
    g_cfg = c;
    LOG("Config: enabled=%d traction=%.2f loss=%.2f brake=%.2f steer=%.2f",
        c.enabled, c.traction, c.tractionLoss, c.brake, c.steer);
}

static inline float RdF(uint8_t* base, int off) { float v; memcpy(&v, base + off, 4); return v; }
static inline void  WrF(uint8_t* base, int off, float v) { memcpy(base + off, &v, 4); }
static inline int32_t RdI(uint8_t* base, size_t off) { int32_t v; memcpy(&v, base + off, 4); return v; }

static bool Sane(float v) { return isfinite(v) && fabsf(v) > 1e-7f && fabsf(v) < 1000.0f; }

// Cari array handling di dalam cHandlingDataMgr:
// entry pertama berisi ID 0,1,2,3 berurutan, dengan massa masuk akal.
static bool FindArray(uint8_t* mgr, uint8_t*& arr, size_t& stride) {
    if (g_cfg.arrayOffset >= 0 && g_cfg.stride > 0) {
        arr = mgr + g_cfg.arrayOffset;
        stride = (size_t)g_cfg.stride;
        return true;
    }
    static const size_t strides[] = {0xD8, 0xE0, 0xE8, 0xD0, 0xDC, 0xE4, 0xF0};
    for (size_t s : strides) {
        for (size_t base = 0; base <= 0x40; base += 4) {
            bool ok = true;
            for (int i = 0; i < 4 && ok; i++) {
                if (RdI(mgr, base + s * i) != i) ok = false;
                float mass = RdF(mgr + base + s * i, OFF_MASS);
                if (!(mass > 50.0f && mass < 200000.0f)) ok = false;
            }
            if (ok) { arr = mgr + base; stride = s; return true; }
        }
    }
    return false;
}

static void Apply(uint8_t* arr, size_t stride) {
    int applied = 0, skipped = 0;
    for (int i = 0; i < MAX_ENTRIES; i++) {
        uint8_t* e = arr + stride * i;
        float mass = RdF(e, OFF_MASS);
        float tr = RdF(e, OFF_TRACTION), tl = RdF(e, OFF_TRAC_LOSS);
        float br = RdF(e, OFF_BRAKE),    st = RdF(e, OFF_STEER);
        if (!(mass > 10.0f && mass < 200000.0f) || !Sane(tr) || !Sane(tl) || !Sane(br) || !Sane(st)) {
            skipped++;
            continue;
        }
        if (!g_orig[i].valid) g_orig[i] = {tr, tl, br, st, true};
        const Orig& o = g_orig[i];
        float m = g_cfg.enabled ? 1.0f : 0.0f; // disabled -> kembali ke nilai asli
        auto mix = [&](float mult) { return g_cfg.enabled ? mult : 1.0f; };
        (void)m;
        WrF(e, OFF_TRACTION,  o.tr * mix(g_cfg.traction));
        WrF(e, OFF_TRAC_LOSS, o.tl * mix(g_cfg.tractionLoss));
        WrF(e, OFF_BRAKE,     o.br * mix(g_cfg.brake));
        WrF(e, OFF_STEER,     o.st * mix(g_cfg.steer));
        applied++;
    }
    LOG("Handling diterapkan: %d entry, dilewati: %d", applied, skipped);
}

static void* WorkerThread(void*) {
    FindConfigPath();
    LoadConfig();
    FileExists(g_cfgPath, &g_cfgMtime);

    void* lib = nullptr;
    for (int i = 0; i < 120 && !lib; i++) {
        lib = dlopen("libGTASA.so", RTLD_NOLOAD | RTLD_LAZY);
        if (!lib) sleep(1);
    }
    if (!lib) { LOG("libGTASA.so tidak ditemukan, berhenti"); return nullptr; }

    void* sym = dlsym(lib, "mod_HandlingManager");
    if (!sym) {
        LOG("Simbol mod_HandlingManager tidak ada di libGTASA.so versi ini. "
            "Butuh offset manual / hook (lihat README).");
        return nullptr;
    }

    uint8_t* mgr = (uint8_t*)sym;
    if (g_cfg.managerIsPointer) mgr = *(uint8_t**)sym;
    LOG("cHandlingDataMgr @ %p", (void*)mgr);

    uint8_t* arr = nullptr;
    size_t stride = 0;
    bool applied = false;
    bool loggedFail = false;

    for (;;) {
        // reload config kalau file berubah
        time_t mt = 0;
        bool changed = false;
        if (FileExists(g_cfgPath, &mt) && mt != g_cfgMtime) {
            g_cfgMtime = mt;
            LoadConfig();
            changed = true;
        }

        if (!arr) {
            if (FindArray(mgr, arr, stride)) {
                LOG("Array handling ditemukan: base=+0x%zx stride=0x%zx", (size_t)(arr - mgr), stride);
            } else if (!loggedFail) {
                LOG("Array handling belum ketemu (game masih loading / layout beda). Mencoba lagi...");
                loggedFail = true;
            }
        }

        if (arr && (!applied || changed)) {
            Apply(arr, stride);
            applied = true;
        }
        sleep(2);
    }
    return nullptr;
}

extern "C" void OnModLoad() {
    LOG("HandlingTweak dimuat");
    pthread_t t;
    pthread_create(&t, nullptr, WorkerThread, nullptr);
    pthread_detach(t);
}
