/* AmbientVoiceSubtitles -- NPC随口台词头顶字幕
 * Floating subtitles above NPCs for their ambient voice lines ("barks"),
 * for Risen (2009).
 *
 * The plugin ships no text of its own: it reads the subtitle strings out of
 * the game's own data\compiled\strings.pak (svms table) and draws them with
 * the engine's own GUI renderer. Loaded as an ASI (a plain DLL exported with
 * InitializeASI) by Ultimate ASI Loader.
 *
 * This file is the source of the v1.0 release build. Development history,
 * hook inventory and debugging notes: docs/ (start with docs/开发日志.md).
 *
 * License: MIT (see LICENSE).
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "ini_template.h"   /* v20：由 gen_ini_template.py 从发布 ini 生成 */

static HINSTANCE g_hinst = NULL;
static const char* lookup_text(const char* key);
static void ensure_text(void);
static int  pak_load(void);
static void lang_to_col(char* out, int cap);
static char g_loaded_col[64];        /* 当前 g_kv 实际来自哪一列 */
/* v17：真正跟随游戏语言（Language=auto 时调引擎取当前语音语言） */
typedef void* (*fn_locinst)(void);
/* 成员函数返回结构体：RCX = this，RDX = 返回缓冲；RAX 回填缓冲地址 */
typedef void (*fn_getvoice)(void* self, void* ret);
static fn_locinst g_locinst = NULL;
static fn_getvoice g_getvoice = NULL;
static int g_lang_tried = 0;

/* ---------- 日志 ---------- */
static HANDLE           g_log = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION g_cs;

static void logf_(const char* fmt, ...)
{
    char buf[2048], ts[64];
    int  n;
    va_list ap;
    SYSTEMTIME st;
    DWORD w;
    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf) - 8, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof(buf) - 8) n = (int)sizeof(buf) - 8;
    GetLocalTime(&st);
    _snprintf(ts, sizeof(ts), "%02d:%02d:%02d.%03d ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    EnterCriticalSection(&g_cs);
    if (g_log != INVALID_HANDLE_VALUE) {
        WriteFile(g_log, ts, (DWORD)strlen(ts), &w, NULL);
        WriteFile(g_log, buf, (DWORD)n, &w, NULL);
        WriteFile(g_log, "\r\n", 2, &w, NULL);
    }
    LeaveCriticalSection(&g_cs);
}

static int u8_to_w(const char* s, wchar_t* out, int cap)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, out, cap);
    if (n <= 0 && cap > 0) out[0] = 0;
    return n;
}

/* ---------- 模块归属判断 ---------- */
typedef struct { HMODULE h; ULONG_PTR base; SIZE_T size; char name[64]; } ModInfo;
static ModInfo g_mods[16];
static int     g_nmods = 0;

static void snapshot_modules(void)
{
    HMODULE hs[256];
    DWORD need = 0, i;
    if (!EnumProcessModules(GetCurrentProcess(), hs, sizeof(hs), &need)) return;
    g_nmods = 0;
    for (i = 0; i < need / sizeof(HMODULE) && g_nmods < 16; i++) {
        MODULEINFO mi;
        char name[MAX_PATH];
        if (!GetModuleInformation(GetCurrentProcess(), hs[i], &mi, sizeof(mi))) continue;
        if (!GetModuleBaseNameA(GetCurrentProcess(), hs[i], name, sizeof(name))) name[0] = 0;
        g_mods[g_nmods].h = hs[i];
        g_mods[g_nmods].base = (ULONG_PTR)mi.lpBaseOfDll;
        g_mods[g_nmods].size = mi.SizeOfImage;
        strncpy(g_mods[g_nmods].name, name, sizeof(g_mods[0].name) - 1);
        g_mods[g_nmods].name[sizeof(g_mods[0].name) - 1] = 0;
        g_nmods++;
    }
}

static const char* which_module(ULONG_PTR p)
{
    int i;
    static char buf[96];
    for (i = 0; i < g_nmods; i++)
        if (p >= g_mods[i].base && p < g_mods[i].base + g_mods[i].size) return g_mods[i].name;
    _snprintf(buf, sizeof(buf), "heap?");
    return buf;
}

/* ---------- 引擎函数 ---------- */
typedef void (*fn_ctor_uni)(void* self, const wchar_t* s);
typedef void* (*fn_getinst)(void);
typedef char (*fn_addsmsg)(void* renderer, const void* geom_entity, const float* v1, const float* v2,
                           const void* text, char always, unsigned int col, float dur);
typedef struct { float x, y, z; } vec3;
typedef vec3 (*fn_getpos)(void* entity);
typedef int (*fn_pscount)(void* entity);
typedef const void* (*fn_psat)(void* entity, int idx);
typedef const void* (*fn_psgeom)(const void* ps);
typedef const void* (*fn_worldmat)(const void* geom_entity);
typedef void* (*fn_hudsys)(void);
typedef void (*fn_showsub)(void* hud, const void* uni);
/* v14：距离淡出用到的相机三件套（Engine.dll）
   eCApplication::GetInstance()          -> eCApplication&  (x64 引用返回即指针)
   eCApplication::GetCurrentCamera()     -> eCCameraBase&
   eCCameraBase::AccessPosition()        -> bCVector&  (x,y,z 三个 float) */
typedef void* (*fn_appinst)(void);
typedef void* (*fn_getcam)(void* app);
typedef const float* (*fn_campos)(void* cam);

static fn_getinst  g_getinst = NULL;
typedef char (*fn_drawmsg)(void* self, void* device, void* sprite, const float* matrix,
                           const float* v1, const float* v2, const void* text,
                           unsigned int col, float alpha, char flag);

static fn_drawmsg  g_drawmsg = NULL;

static fn_addsmsg  g_addsmsg = NULL;
static fn_ctor_uni g_uni_ctor = NULL;
static fn_getpos   g_getpos = NULL;
static fn_pscount  g_pscount = NULL;
static fn_psat     g_psat = NULL;
static fn_psgeom   g_psgeom = NULL;
static fn_worldmat g_worldmat = NULL;
static fn_hudsys   g_hudsys = NULL;
static fn_showsub  g_showsub = NULL;
static fn_appinst  g_appinst = NULL;
static fn_getcam   g_getcam = NULL;
static fn_getcam   g_getmain = NULL;
static fn_campos   g_campos = NULL;
static float g_cam[3] = {0.0f, 0.0f, 0.0f};
static int   g_cam_ok = 0;
static int   g_cam_probe = 0;

#define UNI_SLOTS 128
#define UNI_SIZE  512
static unsigned char g_uni_pool[UNI_SLOTS][UNI_SIZE];
static int           g_uni_used = 0;
static int           g_drawn = 0;

/* 每句 bark 的诊断只做前 N 次，避免日志爆炸 */
static int g_diag_budget = 6;

/* Script 层 Entity 对象 +0 处疑似就是引擎 eCEntity*（v2 实测 +0 随 NPC 变化、+18/+20 是栈指针） */
static void* engine_entity(void* ent)
{
    void* p = NULL;
    SIZE_T got = 0;
    if (!ent) return NULL;
    if (!ReadProcessMemory(GetCurrentProcess(), ent, &p, sizeof(p), &got) || got != sizeof(p)) return NULL;
    if ((ULONG_PTR)p < 0x10000) return NULL;
    return p;
}

static void diagnose_entity(void* ent, const char* svm)
{
    ULONG_PTR q[8];
    void* eng = NULL;
    int i;

    if (g_diag_budget <= 0) return;
    g_diag_budget--;

    __try {
        memcpy(q, ent, sizeof(q));
        for (i = 0; i < 8; i++)
            logf_("ENT[%s] +%02X = %p (%s)", svm, i * 8, (void*)q[i],
                  q[i] > 0x10000 ? which_module((ULONG_PTR)q[i]) : "-");
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        logf_("ENT[%s] dump EXCEPTION code=%lu", svm, GetExceptionCode());
        return;
    }

    eng = engine_entity(ent);
    logf_("ENG[%s] = %p (%s)", svm, eng, eng ? which_module((ULONG_PTR)eng) : "-");
    if (!eng) return;

    {
        ULONG_PTR vt = 0;
        __try { vt = *(ULONG_PTR*)eng; } __except (EXCEPTION_EXECUTE_HANDLER) { vt = 0; }
        logf_("ENGVT[%s] = %p (%s)", svm, (void*)vt, vt > 0x10000 ? which_module(vt) : "-");
    }

    if (g_pscount) {
        int cnt = -1;
        __try { cnt = g_pscount(eng); } __except (EXCEPTION_EXECUTE_HANDLER) {
            logf_("PSCOUNT EXCEPTION code=%lu", GetExceptionCode());
        }
        logf_("PSCOUNT[%s] = %d", svm, cnt);
        if (cnt > 0 && cnt < 64 && g_psat && g_psgeom) {
            int k;
            for (k = 0; k < cnt; k++) {
                const void* ps = NULL;
                const void* ge = NULL;
                __try { ps = g_psat(eng, k); } __except (EXCEPTION_EXECUTE_HANDLER) { ps = NULL; }
                if (!ps) { logf_("  ps[%d] = NULL", k); continue; }
                __try { ge = g_psgeom(ps); } __except (EXCEPTION_EXECUTE_HANDLER) { ge = NULL; }
                logf_("  ps[%d] = %p (%s) geom=%p", k, ps, which_module((ULONG_PTR)ps), ge);
            }
        }
    }
}

static void* pick_geom(void* eng)
{
    int cnt, k;
    if (!eng || !g_pscount || !g_psat || !g_psgeom) return NULL;
    __try { cnt = g_pscount(eng); } __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
    if (cnt <= 0 || cnt > 64) return NULL;
    for (k = 0; k < cnt; k++) {
        const void* ps = NULL;
        const void* ge = NULL;
        __try { ps = g_psat(eng, k); } __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
        if (!ps) continue;
        __try { ge = g_psgeom(ps); } __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
        if (ge) return (void*)ge;
    }
    return NULL;
}

/* ---------- 每帧重投递的消息队列 ----------
   AddScreenMessage 只是往一个数组里追加，DrawScreenMessages 画完就把数组清空（实测）,
   所以一条消息只活一帧 —— 必须自己保存、每帧重新投递。 */
#define MSG_MAX 8
typedef struct {
    void*   geom;
    wchar_t text[512];
    DWORD   expire;
    int     used;
} Msg;

static Msg   g_msgs[MSG_MAX];
static DWORD g_msg_dur = 3000;     /* v13：默认停留 3 秒（用户在 2K/1080p 实测 4 秒略长） */
static float g_lift    = 230.0f;   /* 字幕相对 NPC 原点抬高的厘米数（世界单位 = 厘米） */
static unsigned int g_color = 0xFFFFFFFFu;
static int   g_max_msg = 8;
static int   g_enabled = 1;
static DWORD g_fade_ms = 800;      /* v13：最后 N 毫秒淡出到全透明；0 = 关闭 */
static int   g_dist_fade = 1;      /* v14：按相机距离淡出 */
static float g_fade_start = 5.0f;  /* v16：按用户实测默认 5 米开始变淡 */
static float g_fade_end   = 15.0f; /* v16：按用户实测默认 15 米完全不画 */
static char  g_lang[64] = "auto";  /* v16：字幕语言，auto = 读 English 列；也可填 German / French_Text 等 */
static int   g_pak_mode = 0;       /* v16：1 = 文本来自游戏 strings.pak，0 = 来自 json */
static char  g_conf_dir[MAX_PATH] = {0};
static FILETIME g_ini_ft = {0, 0};
static DWORD g_ini_check = 0;

static void* g_onb_addr = NULL;   /* gCGUIEntityRenderer::OnBeginFrame 真身 */
static void* g_onb_orig = NULL;
static int   g_vt_hooked = 0;
static int   g_frame_draws = 0;
static int   g_dbg_draws = 0;

static void write_default_ini(const char* path)
{
    /* v20：模板来自 ini_template.h（由 gen_ini_template.py 从发布 ini 逐字节生成），
     *      不再手写在本文件里 —— 以前手写的那份会跟 ini 漂移（FadeStartM=5 等）。 */
    HANDLE h;
    DWORD written = 0;
    if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) return;
    h = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    WriteFile(h, AVS_INI_TEMPLATE, (DWORD)AVS_INI_TEMPLATE_LEN, &written, NULL);
    CloseHandle(h);
}

static void load_config(void)
{
    char path[MAX_PATH], buf[64], sec[] = "AmbientVoiceSubtitles";
    if (!g_conf_dir[0]) return;
    _snprintf(path, sizeof(path), "%s\\AmbientVoiceSubtitles.ini", g_conf_dir);
    write_default_ini(path);

    g_enabled = GetPrivateProfileIntA(sec, "Enabled",      1,    path) ? 1 : 0;
    g_lift    = (float)GetPrivateProfileIntA(sec, "HeightCm", 230, path);
    g_msg_dur = (DWORD)GetPrivateProfileIntA(sec, "DurationMs", 3000, path);
    g_fade_ms = (DWORD)GetPrivateProfileIntA(sec, "FadeMs",     800,  path);
    g_dist_fade = GetPrivateProfileIntA(sec, "DistanceFade", 1, path) ? 1 : 0;
    g_fade_start = (float)GetPrivateProfileIntA(sec, "FadeStartM", 5, path);
    g_fade_end   = (float)GetPrivateProfileIntA(sec, "FadeEndM",   15, path);
    g_max_msg = (int)GetPrivateProfileIntA(sec, "MaxMessages", 8, path);
    GetPrivateProfileStringA(sec, "Language", "auto", g_lang, sizeof(g_lang), path);
    GetPrivateProfileStringA(sec, "Color", "FFFFFFFF", buf, sizeof(buf), path);
    g_color = (unsigned int)strtoul(buf, NULL, 16);
    /* 只写 6 位 RGB 时补上不透明 alpha，否则 alpha=00 会显示成全黑/不可见 */
    if (g_color <= 0x00FFFFFFu) g_color |= 0xFF000000u;

    if (g_lift < 0.0f)      g_lift = 0.0f;
    if (g_lift > 1000.0f)   g_lift = 1000.0f;
    if (g_msg_dur < 500)    g_msg_dur = 500;
    if (g_msg_dur > 30000)  g_msg_dur = 30000;
    if (g_fade_ms > 5000)   g_fade_ms = 5000;
    if (g_max_msg < 1)      g_max_msg = 1;
    if (g_max_msg > MSG_MAX) g_max_msg = MSG_MAX;
    if (g_fade_start < 1.0f) g_fade_start = 1.0f;
    if (g_fade_end <= g_fade_start) g_fade_end = g_fade_start + 5.0f;
    if (!g_lang[0]) strcpy(g_lang, "auto");

    logf_("CONFIG enabled=%d lift=%.0f dur=%lu fade=%lu max=%d color=%08lX dist=%d %.0f/%.0fm lang=%s",
          g_enabled, (double)g_lift, (unsigned long)g_msg_dur,
          (unsigned long)g_fade_ms, g_max_msg, (unsigned long)g_color,
          g_dist_fade, (double)g_fade_start, (double)g_fade_end, g_lang);
}

static void maybe_reload_config(void)
{
    char path[MAX_PATH];
    WIN32_FILE_ATTRIBUTE_DATA fa;
    DWORD now = GetTickCount();

    if (!g_conf_dir[0]) return;
    if (now - g_ini_check < 2000) return;
    g_ini_check = now;
    _snprintf(path, sizeof(path), "%s\\AmbientVoiceSubtitles.ini", g_conf_dir);
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &fa)) return;
    if (fa.ftLastWriteTime.dwLowDateTime == g_ini_ft.dwLowDateTime &&
        fa.ftLastWriteTime.dwHighDateTime == g_ini_ft.dwHighDateTime) return;
    g_ini_ft = fa.ftLastWriteTime;
    load_config();
    /* Language 改了要重新读 pak 的对应列，否则只是改了变量、文本还是旧语言 */
    if (g_pak_mode) {
        char col[64];
        lang_to_col(col, sizeof(col));
        if (_stricmp(col, g_loaded_col) != 0) {
            logf_("LANG changed (%s -> %s), reloading pak", g_loaded_col, col);
            if (!pak_load()) logf_("LANG reload failed, keeping previous text");
        }
    }
}

static void* geom_of(void* ent)
{
    return pick_geom(engine_entity(ent));
}

static void queue_add_entity(void* ent, const char* utf8)
{
    wchar_t w[512];
    void* geom;
    int i, slot = -1, oldest = 0;
    DWORD now = GetTickCount();

    if (u8_to_w(utf8, w, 512) <= 0) return;
    if (!g_enabled) return;
    geom = geom_of(ent);
    if (!geom) { logf_("Q-NOGEOM text=\"%s\"", utf8); return; }

    EnterCriticalSection(&g_cs);
    for (i = 0; i < MSG_MAX; i++) {
        if (g_msgs[i].used && g_msgs[i].geom == geom) { slot = i; break; }
    }
    if (slot < 0) {
        for (i = 0; i < MSG_MAX; i++) if (!g_msgs[i].used) { slot = i; break; }
    }
    if (slot < 0) {
        slot = 0;
        for (i = 1; i < MSG_MAX; i++) if (g_msgs[i].expire < g_msgs[oldest].expire) oldest = i;
        slot = oldest;
    }
    g_msgs[slot].geom = geom;
    wcsncpy(g_msgs[slot].text, w, 511);
    g_msgs[slot].text[511] = 0;
    g_msgs[slot].expire = now + g_msg_dur;
    g_msgs[slot].used = 1;
    LeaveCriticalSection(&g_cs);
    logf_("QUEUED slot=%d geom=%p text=\"%s\"", slot, geom, utf8);
}

static void draw_queue(void* dev, void* sprite)
{
    int i;
    DWORD now = GetTickCount();
    void* renderer;
    int alive = 0;

    if (!g_drawmsg || !g_getinst || !g_uni_ctor || !g_worldmat) return;
    maybe_reload_config();
    if (!g_enabled) return;
    renderer = g_getinst();
    if (!renderer || !dev || !sprite) return;

    /* v15：每帧取一次当前相机位置。v14 实测 GetCurrentCamera + AccessPosition 恒为
       (0,0,0)（该对象在 OnBeginFrame 阶段还没就绪，不是渲染相机），距离被算成两万多
       厘米，字幕全被过滤掉 → v14 一句都不显示。现在：
       ① 先试 GetCurrentCamera，再试 GetMainCamera；
       ② 位置三个分量都 < 1 视为无效 → g_cam_ok=0，距离淡出整体停用（fail-open，宁可不淡出也不能不显示）；
       ③ 世界单位是厘米，ini 写的是米，比较前 /100。 */
    if (g_dist_fade && g_appinst && g_campos) {
        int got = 0;
        __try {
            void* app = g_appinst();
            if (app) {
                void* cam;
                const float* cp;
                cam = g_getcam ? g_getcam(app) : NULL;
                if (cam) {
                    cp = g_campos(cam);
                    if (cp && (fabsf(cp[0]) > 1.0f || fabsf(cp[1]) > 1.0f || fabsf(cp[2]) > 1.0f)) {
                        g_cam[0] = cp[0]; g_cam[1] = cp[1]; g_cam[2] = cp[2]; got = 1;
                    }
                }
                if (!got && g_getmain) {
                    cam = g_getmain(app);
                    if (cam) {
                        cp = g_campos(cam);
                        if (cp && (fabsf(cp[0]) > 1.0f || fabsf(cp[1]) > 1.0f || fabsf(cp[2]) > 1.0f)) {
                            g_cam[0] = cp[0]; g_cam[1] = cp[1]; g_cam[2] = cp[2]; got = 1;
                        }
                    }
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            got = 0;
        }
        g_cam_ok = got;
        if (g_cam_probe < 5) {
            g_cam_probe++;
            logf_("CAM pos=(%.1f, %.1f, %.1f) valid=%d", (double)g_cam[0], (double)g_cam[1], (double)g_cam[2], g_cam_ok);
        }
    }

    EnterCriticalSection(&g_cs);
    for (i = 0; i < MSG_MAX; i++) {
        Msg* m = &g_msgs[i];
        const float* wm;
        float mat[16];
        float zero[3];
        void* txt;
        char r;
        DWORD left;
        float alpha = 1.0f;
        if (!m->used) continue;
        if (now >= m->expire) { m->used = 0; continue; }
        alive++;

        /* v16：只做时间淡出（缩小功能已被移除：引擎按距离统一补缩放，缩小无效） */
        left = m->expire - now;
        if (g_fade_ms > 0 && left < g_fade_ms)
            alpha = (float)left / (float)g_fade_ms;
        __try {
            /* v10：上一版实测 +Z 抬升把字移到了"脚底左边" → 竖直轴是 Y，不是 Z。世界单位是厘米（实测矩阵平移 6568/12549/20775 是坐标不是米），
               之前抬 1.85 / 2.0 只等于 2 厘米，肉眼看就是"没动"。这次抬 180 单位 = 1.8 米。 */
            wm = (const float*)g_worldmat(m->geom);
            /* v15：距离淡出 —— 厘米换算成米再比；超距只跳过本帧，不删消息（v14 用 m->used=0
               会因一次误判永久丢句） */
            if (g_dist_fade && g_cam_ok && g_fade_end > g_fade_start) {
                float dx = wm[12] - g_cam[0];
                float dy = wm[13] - g_cam[1];
                float dz = wm[14] - g_cam[2];
                float d = (float)sqrt((double)(dx * dx + dy * dy + dz * dz)) / 100.0f;
                if (d >= g_fade_end) continue;
                if (d > g_fade_start) alpha *= (g_fade_end - d) / (g_fade_end - g_fade_start);
            }
            if (g_dbg_draws == 0) {
                logf_("MAT16 %p = %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f",
                      wm, wm[0], wm[1], wm[2], wm[3], wm[4], wm[5], wm[6], wm[7],
                      wm[8], wm[9], wm[10], wm[11], wm[12], wm[13], wm[14], wm[15]);
            }
            memcpy(mat, wm, sizeof(mat));
            mat[13] += g_lift;   /* 实测竖直轴是 Y，世界单位 = 厘米；数值来自 AmbientVoiceSubtitles.ini */
            zero[0] = 0.0f; zero[1] = 0.0f; zero[2] = 0.0f;
            txt = g_uni_pool[g_uni_used % UNI_SLOTS];
            g_uni_used++;
            memset(txt, 0, UNI_SIZE);
            g_uni_ctor(txt, m->text);
            r = g_drawmsg(renderer, dev, sprite, mat, zero, zero, txt, g_color, alpha, 1);
            if (g_dbg_draws < 3) {
                logf_("DRAWMSG ret=%d alpha=%.2f base=(%.2f,%.2f,%.2f) lifted=(%.2f,%.2f,%.2f)", (int)r,
                      (double)alpha,
                      wm[12], wm[13], wm[14], mat[12], mat[13], mat[14]);
            }
            g_dbg_draws++;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            if (g_dbg_draws < 2) logf_("DRAWMSG EXCEPTION code=%lu", GetExceptionCode());
        }
    }
    LeaveCriticalSection(&g_cs);

    if (g_frame_draws < 5) {
        g_frame_draws++;
        logf_("FRAME alive=%d renderer=%p", alive, renderer);
    }
}

static void det_onbegin(void* self, void* dev, void* sprite)
{
    draw_queue(dev, sprite);
    if (g_onb_orig) ((void (*)(void*, void*, void*))g_onb_orig)(self, dev, sprite);
}

static void hook_vtable(void* renderer)
{
    void** vt;
    int i;
    DWORD old;

    if (g_vt_hooked || !renderer || !g_onb_addr) return;
    vt = *(void***)renderer;
    if (!vt) return;
    __try {
        for (i = 0; i < 128; i++) {
            if (vt[i] == g_onb_addr) {
                if (VirtualProtect(&vt[i], sizeof(void*), PAGE_READWRITE, &old)) {
                    g_onb_orig = vt[i];
                    vt[i] = (void*)det_onbegin;
                    VirtualProtect(&vt[i], sizeof(void*), old, &old);
                    g_vt_hooked = 1;
                    logf_("VT hooked OnBeginFrame slot=%d orig=%p", i, g_onb_orig);
                }
                break;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        logf_("VT hook EXCEPTION code=%lu", GetExceptionCode());
    }
    if (!g_vt_hooked) logf_("VT NOT hooked (addr=%p)", g_onb_addr);
}

static void hud_fallback(const char* utf8)
{
    wchar_t w[1024];
    void* hud;
    void* txt;

    if (!g_hudsys || !g_showsub || !g_uni_ctor) { logf_("HUDFB missing api"); return; }
    if (u8_to_w(utf8, w, 1024) <= 0) return;
    __try {
        hud = g_hudsys();
        logf_("HUDFB hud=%p", hud);
        if (!hud) return;
        txt = g_uni_pool[g_uni_used % UNI_SLOTS];
        g_uni_used++;
        memset(txt, 0, UNI_SIZE);
        g_uni_ctor(txt, w);
        g_showsub(hud, txt);
        logf_("HUDFB shown text=\"%s\"", utf8);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        logf_("HUDFB EXCEPTION code=%lu", GetExceptionCode());
    }
}

/* 兼容旧调用点：入队 + 立刻尝试装 vtable 钩子（真正绘制在每帧回调里） */
static void show_text(void* ent, const char* utf8)
{
    void* eng = engine_entity(ent);
    void* geom = pick_geom(eng);
    logf_("ENGgeom eng=%p geom=%p", eng, geom);
    if (!geom) { logf_("DRAW-SKIP no geometry entity -> hud fallback"); hud_fallback(utf8); return; }
    {
        void* renderer = g_getinst ? g_getinst() : NULL;
        if (renderer) hook_vtable(renderer);
    }
    queue_add_entity(ent, utf8);
}

/* ---------- svm / voice 捕获 ---------- */
typedef char (*fn_fmt)(void* self, const void* voice, const void* svm, void* out);
static fn_fmt g_title_orig = NULL;

typedef void (*fn_saysvm)(void* self, void* listener, const void* svm, char a4, char a5);
static fn_saysvm g_saysvm_orig = NULL;

typedef struct { DWORD tick; void* entity; char svm[96]; } Recent;
static Recent g_recent[16];
static volatile LONG g_recent_pos = 0;

static void read_bcstr(const void* p, char* out, int cap)
{
    const char* s = NULL;
    SIZE_T got = 0;
    out[0] = 0;
    if (!p) { strncpy(out, "<null>", cap - 1); return; }
    if (!ReadProcessMemory(GetCurrentProcess(), p, &s, sizeof(s), &got) || got != sizeof(s) || !s) {
        strncpy(out, "<empty>", cap - 1);
        return;
    }
    if (!ReadProcessMemory(GetCurrentProcess(), s, out, (SIZE_T)(cap - 1), &got) || got == 0) {
        strncpy(out, "<unreadable>", cap - 1);
        return;
    }
    out[got < (SIZE_T)(cap - 1) ? got : (SIZE_T)(cap - 1)] = 0;
}

static void push_recent(void* entity, const char* svm)
{
    LONG i = InterlockedIncrement(&g_recent_pos) - 1;
    Recent* r = &g_recent[i & 15];
    r->tick = GetTickCount();
    r->entity = entity;
    strncpy(r->svm, svm, sizeof(r->svm) - 1);
    r->svm[sizeof(r->svm) - 1] = 0;
}

static void* find_recent(const char* svm, DWORD max_age_ms)
{
    DWORD now = GetTickCount();
    int i;
    for (i = 0; i < 16; i++) {
        Recent* r = &g_recent[i];
        if (r->entity && strcmp(r->svm, svm) == 0 && (now - r->tick) <= max_age_ms)
            return r->entity;
    }
    return NULL;
}

static void det_saysvm(void* self, void* listener, const void* svm, char a4, char a5)
{
    char s[96];
    read_bcstr(svm, s, sizeof(s));
    push_recent(self, s);
    logf_("SAYSVM speaker=%p svm=\"%s\"", self, s);
    g_saysvm_orig(self, listener, svm, a4, a5);
}

static char det_title(void* self, const void* voice, const void* svm, void* out)
{
    char r = g_title_orig(self, voice, svm, out);
    char v[96], s[96], key[256];
    void* ent;
    const char* zh;

    read_bcstr(voice, v, sizeof(v));
    read_bcstr(svm, s, sizeof(s));
    _snprintf(key, sizeof(key), "SVM_%s_%s", v, s);
    key[sizeof(key) - 1] = 0;
    ensure_text();
    zh = lookup_text(key);
    ent = find_recent(s, 1500);
    if (!zh) { logf_("NOKEY key=%s", key); return r; }
    if (!ent) { logf_("NOSPEAKER key=%s", key); return r; }
    logf_("BARK key=%s speaker=%p text=\"%s\"", key, ent, zh);
    if (g_diag_budget > 0) diagnose_entity(ent, s);
    show_text(ent, zh);
    return r;
}

/* ---------- 文本库 ---------- */
/* ======================================================================
 * v16：从游戏自带 data\compiled\strings.pak 直接读 svms 文本
 *   不发布任何游戏文本，语言列由 ini 的 Language 决定。
 *   任一步失败返回 0，调用方回退到 json。
 * ====================================================================== */

/* ---------- 最小 zlib inflate（RFC1950 包装 + RFC1951 deflate） ----------
   只用 stored / fixed / dynamic 三种块，足够解 pak 里的 .tab。
   代码按 puff 的 canonical Huffman 算法写，无外部依赖。 */
typedef struct {
    const unsigned char* in;
    unsigned int inlen;
    unsigned int pos;
    unsigned long long bits;
    int nbits;
} InfState;

static int inf_bits(InfState* s, int need)
{
    while (s->nbits < need) {
        if (s->pos >= s->inlen) return -1;
        s->bits |= (unsigned long long)s->in[s->pos++] << s->nbits;
        s->nbits += 8;
    }
    {
        int v = (int)(s->bits & ((1ull << need) - 1ull));
        s->bits >>= need;
        s->nbits -= need;
        return v;
    }
}

typedef struct {
    short count[16];
    short sym[288];
} InfHuff;

static int inf_huff_build(InfHuff* t, const unsigned char* len, int n)
{
    int i, left;
    short offs[16];
    memset(t->count, 0, sizeof(t->count));
    for (i = 0; i < n; i++) {
        if (len[i] > 15) return -1;              /* 码长越界（损坏数据） */
        t->count[len[i]]++;
    }
    if (t->count[0] == n) return 0;          /* 空树：合法（例如没有距离码） */
    left = 1;
    for (i = 1; i <= 15; i++) {
        left <<= 1;
        left -= t->count[i];
        if (left < 0) return -1;             /* 超订 */
    }
    offs[1] = 0;
    for (i = 1; i < 15; i++) offs[i + 1] = (short)(offs[i] + t->count[i]);
    for (i = 0; i < n; i++)
        if (len[i]) t->sym[offs[len[i]]++] = (short)i;
    return 0;
}

static int inf_huff_dec(InfState* s, const InfHuff* t)
{
    int len, code = 0, first = 0, index = 0;
    for (len = 1; len <= 15; len++) {
        int b = inf_bits(s, 1);
        if (b < 0) return -1;
        code |= b;
        {
            int cnt = t->count[len];
            if (code - first < cnt) return t->sym[index + (code - first)];
            index += cnt;
            first = (first + cnt) << 1;
            code <<= 1;
        }
    }
    return -1;
}

static const short INF_LBASE[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
static const short INF_LEXT[29]  = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
static const short INF_DBASE[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
static const short INF_DEXT[30]  = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};

static int inf_block_codes(InfState* s, unsigned char* dst, unsigned int cap,
                           unsigned int* out, const InfHuff* lit, const InfHuff* dist)
{
    for (;;) {
        int sym = inf_huff_dec(s, lit);
        if (sym < 0) return 0;
        if (sym < 256) {
            if (*out >= cap) return 0;
            dst[(*out)++] = (unsigned char)sym;
        } else if (sym == 256) {
            return 1;
        } else {
            int li = sym - 257;
            int len, di, b;
            if (li >= 29) return 0;
            b = inf_bits(s, INF_LEXT[li]);
            if (b < 0) return 0;
            len = INF_LBASE[li] + b;
            di = inf_huff_dec(s, dist);
            if (di < 0 || di >= 30) return 0;
            b = inf_bits(s, INF_DEXT[di]);
            if (b < 0) return 0;
            di = INF_DBASE[di] + b;
            if ((unsigned int)di > *out || *out + (unsigned int)len > cap) return 0;
            while (len--) { dst[*out] = dst[*out - di]; (*out)++; }
        }
    }
}

static void inf_fixed(InfHuff* lit, InfHuff* dist)
{
    unsigned char l[288];
    int i;
    for (i = 0; i < 144; i++) l[i] = 8;
    for (i = 144; i < 256; i++) l[i] = 9;
    for (i = 256; i < 280; i++) l[i] = 7;
    for (i = 280; i < 288; i++) l[i] = 8;
    inf_huff_build(lit, l, 288);
    for (i = 0; i < 30; i++) l[i] = 5;
    inf_huff_build(dist, l, 30);
}

static int inf_zlib(const unsigned char* src, unsigned int srclen,
                    unsigned char* dst, unsigned int cap, unsigned int* outlen)
{
    InfState s;
    unsigned int out = 0;
    int last;
    InfHuff fixlit, fixdist;
    static const unsigned char ORD[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};

    if (srclen < 6) return 0;
    if ((src[0] & 0x0F) != 8) return 0;                    /* CM = deflate */
    if ((((unsigned int)src[0] << 8) | src[1]) % 31 != 0) return 0;
    if (src[1] & 0x20) return 0;                           /* 不支持预置字典 */

    memset(&s, 0, sizeof(s));
    s.in = src + 2;                                        /* 跳过 zlib 头 */
    s.inlen = srclen - 2;
    inf_fixed(&fixlit, &fixdist);

    do {
        int type;
        last = inf_bits(&s, 1);
        if (last < 0) return 0;
        type = inf_bits(&s, 2);
        if (type < 0) return 0;

        if (type == 0) {
            unsigned int len, nlen;
            s.bits = 0; s.nbits = 0;                       /* stored 块按字节对齐 */
            if (s.pos + 4 > s.inlen) return 0;
            len  = (unsigned int)s.in[s.pos] | ((unsigned int)s.in[s.pos + 1] << 8);
            nlen = (unsigned int)s.in[s.pos + 2] | ((unsigned int)s.in[s.pos + 3] << 8);
            s.pos += 4;
            if ((len ^ 0xFFFF) != nlen) return 0;
            if (s.pos + len > s.inlen) return 0;
            if (out + len > cap) return 0;
            memcpy(dst + out, s.in + s.pos, len);
            s.pos += len;
            out += len;
        } else if (type == 1) {
            if (!inf_block_codes(&s, dst, cap, &out, &fixlit, &fixdist)) return 0;
        } else if (type == 2) {
            unsigned char lens[320];
            InfHuff cl, lit, dist;
            int hlit, hdist, hclen, i, n;
            hlit = inf_bits(&s, 5); if (hlit < 0) return 0; hlit += 257;
            hdist = inf_bits(&s, 5); if (hdist < 0) return 0; hdist += 1;
            hclen = inf_bits(&s, 4); if (hclen < 0) return 0; hclen += 4;
            if (hlit > 286 || hdist > 30) return 0;
            memset(lens, 0, sizeof(lens));
            for (i = 0; i < hclen; i++) {
                int v = inf_bits(&s, 3);
                if (v < 0) return 0;
                lens[ORD[i]] = (unsigned char)v;
            }
            if (inf_huff_build(&cl, lens, 19) != 0) return 0;
            n = 0;
            while (n < hlit + hdist) {
                int sym = inf_huff_dec(&s, &cl);
                int rep, val = 0, b;
                if (sym < 0) return 0;
                if (sym < 16) {
                    lens[n++] = (unsigned char)sym;
                    continue;
                }
                if (sym == 16) {
                    if (n == 0) return 0;
                    val = lens[n - 1];
                    b = inf_bits(&s, 2); if (b < 0) return 0;
                    rep = 3 + b;
                } else if (sym == 17) {
                    b = inf_bits(&s, 3); if (b < 0) return 0;
                    rep = 3 + b;
                } else {
                    b = inf_bits(&s, 7); if (b < 0) return 0;
                    rep = 11 + b;
                }
                if (n + rep > hlit + hdist) return 0;
                while (rep--) lens[n++] = (unsigned char)val;
            }
            if (inf_huff_build(&lit, lens, hlit) != 0) return 0;
            if (inf_huff_build(&dist, lens + hlit, hdist) != 0) return 0;
            if (!inf_block_codes(&s, dst, cap, &out, &lit, &dist)) return 0;
        } else {
            return 0;
        }
    } while (!last);

    *outlen = out;
    return 1;
}

/* ---------- G3V0 pak 目录解析 ---------- */
static unsigned int pk_u32(const unsigned char* p)
{
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8) |
           ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}
static unsigned long long pk_u64(const unsigned char* p)
{
    return (unsigned long long)pk_u32(p) | ((unsigned long long)pk_u32(p + 4) << 32);
}

typedef struct {
    unsigned int off;       /* 文件内数据偏移 */
    unsigned int stored;
    unsigned int raw;
} PkFile;

static int pk_walk(const unsigned char* t, unsigned int tlen, unsigned int* pp,
                   const char* want, PkFile* out)
{
    unsigned int p = *pp;
    unsigned int attr, name_len, count, i;
    char name[256];

    if (p + 8 > tlen) return 0;
    attr = pk_u32(t + p); p += 4;
    name_len = pk_u32(t + p); p += 4;
    if (name_len > 255 || p + name_len + 1 > tlen) return 0;
    memcpy(name, t + p, name_len);
    name[name_len] = 0;
    p += name_len + 1;

    if (attr & 0x10) {                       /* 目录 */
        count = 0;
        if (p + 32 > tlen) return 0;         /* t1..t3 + attr_a + count */
        p += 24;                             /* t1,t2,t3 */
        p += 4;                              /* attr_a */
        count = pk_u32(t + p); p += 4;
        if (count > 4096) return 0;
        for (i = 0; i < count; i++) {
            if (pk_walk(t, tlen, &p, want, out)) { *pp = p; return 1; }
        }
    } else {                                 /* 文件 */
        unsigned int data_off, stored, raw;
        if (p + 52 > tlen) return 0;
        data_off = pk_u32(t + p); p += 4;
        p += 4;                              /* unk0 */
        p += 24;                             /* t1,t2,t3 */
        p += 4;                              /* attr_a */
        p += 4;                              /* v1 */
        p += 4;                              /* v2 */
        stored = pk_u32(t + p); p += 4;
        raw    = pk_u32(t + p); p += 4;
        if (_stricmp(name, want) == 0) {
            out->off = data_off; out->stored = stored; out->raw = raw;
            *pp = p;
            return 1;
        }
    }
    *pp = p;
    return 0;
}

static int pk_find(const unsigned char* pak, unsigned int paklen,
                   unsigned int table_off, unsigned int total_size,
                   const char* want, PkFile* out)
{
    unsigned int p = 0, nchild, i;
    const unsigned char* t;
    unsigned int tlen;

    if (table_off >= total_size || total_size > paklen) return 0;
    t = pak + table_off;
    tlen = total_size - table_off;

    if (tlen < 36) return 0;
    p = 4;                                   /* root attr */
    p += 24;                                 /* t1,t2,t3 */
    p += 4;                                  /* attr_a */
    nchild = pk_u32(t + p); p += 4;
    for (i = 0; i < nchild; i++)
        if (pk_walk(t, tlen, &p, want, out)) return 1;
    return 0;
}

typedef struct { char* key; char* val; } KV;
static KV*  g_kv = NULL;
static int  g_kv_n = 0;

static void free_kv_arr(KV* kv, int n)
{
    int i;
    if (!kv) return;
    for (i = 0; i < n; i++) { free(kv[i].key); free(kv[i].val); }
    free(kv);
}

static void free_kv(void)
{
    free_kv_arr(g_kv, g_kv_n);
    g_kv = NULL;
    g_kv_n = 0;
}

static void json_unescape(const char* s, char* out, int cap)
{
    int o = 0;
    while (*s && o < cap - 8) {
        if (*s == '\\') {
            s++;
            switch (*s) {
            case 'n': out[o++] = '\n'; s++; break;
            case 'r': out[o++] = '\r'; s++; break;
            case 't': out[o++] = '\t'; s++; break;
            case '"': out[o++] = '"'; s++; break;
            case '\\': out[o++] = '\\'; s++; break;
            case '/': out[o++] = '/'; s++; break;
            case 'u': {
                unsigned v = 0; int i;
                s++;
                for (i = 0; i < 4 && *s; i++, s++) {
                    char c = *s;
                    v <<= 4;
                    if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
                    else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
                    else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
                }
                if (v < 0x80) out[o++] = (char)v;
                else if (v < 0x800) { out[o++] = (char)(0xC0 | (v >> 6)); out[o++] = (char)(0x80 | (v & 0x3F)); }
                else { out[o++] = (char)(0xE0 | (v >> 12)); out[o++] = (char)(0x80 | ((v >> 6) & 0x3F)); out[o++] = (char)(0x80 | (v & 0x3F)); }
                break;
            }
            default: out[o++] = *s++; break;
            }
        } else {
            out[o++] = *s++;
        }
    }
    out[o] = 0;
}

static int load_text_db(const char* path)
{
    HANDLE h;
    DWORD size = 0, got = 0;
    char* p;
    int cap = 4096, n = 0;
    char* json;

    h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) { logf_("ERROR open json err=%lu", GetLastError()); return 0; }
    size = GetFileSize(h, NULL);
    json = (char*)malloc(size + 1);
    if (!json) { CloseHandle(h); return 0; }
    ReadFile(h, json, size, &got, NULL);
    CloseHandle(h);
    json[got] = 0;

    free_kv();
    g_kv = (KV*)calloc(cap, sizeof(KV));
    if (!g_kv) return 0;

    p = json;
    while (*p) {
        char kbuf[512], vbuf[4096];
        char* kstart;
        if (*p != '"') { p++; continue; }
        p++;
        kstart = p;
        while (*p && *p != '"') { if (*p == '\\') p++; p++; }
        if (*p != '"') break;
        {
            int len = (int)(p - kstart);
            if (len >= (int)sizeof(kbuf)) len = (int)sizeof(kbuf) - 1;
            memcpy(kbuf, kstart, (size_t)len);
            kbuf[len] = 0;
            json_unescape(kbuf, kbuf, sizeof(kbuf));
        }
        p++;
        while (*p && *p != ':') p++;
        if (*p == ':') p++;
        while (*p && *p != '"') p++;
        if (*p != '"') break;
        p++;
        {
            char* vstart = p;
            while (*p && *p != '"') { if (*p == '\\') p++; p++; }
            if (*p != '"') break;
            {
                int len = (int)(p - vstart);
                if (len >= (int)sizeof(vbuf)) len = (int)sizeof(vbuf) - 1;
                memcpy(vbuf, vstart, (size_t)len);
                vbuf[len] = 0;
                json_unescape(vbuf, vbuf, sizeof(vbuf));
            }
            p++;
        }
        if (n >= cap - 1) {
            KV* nk = (KV*)realloc(g_kv, sizeof(KV) * (size_t)(cap * 2));
            if (!nk) break;
            g_kv = nk;
            cap *= 2;
        }
        g_kv[n].key = _strdup(kbuf);
        g_kv[n].val = _strdup(vbuf);
        n++;
    }
    g_kv_n = n;
    logf_("text db loaded: %d keys", n);
    return n;
}

/* ---------- TAB0 字符串表解析 ---------- */
/* TAB0 里所有字符串都是 UTF-16LE：name_len / entry len 都是"字符数"，
   实际字节数是 2 倍（tools\tabtool.py:86/97 的 2*name_len、2*ln）。 */
#define TAB_MAX_BLOCKS 32

/* UTF-16LE → UTF-8；chars = 字符数；返回写入字节数 */
static int w2u8(const unsigned char* s, int chars, char* out, int cap)
{
    int i, o = 0;
    for (i = 0; i < chars; i++) {
        unsigned int c = (unsigned int)s[2 * i] | ((unsigned int)s[2 * i + 1] << 8);
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < chars) {
            unsigned int c2 = (unsigned int)s[2 * (i + 1)] | ((unsigned int)s[2 * (i + 1) + 1] << 8);
            if (c2 >= 0xDC00 && c2 <= 0xDFFF) {
                c = 0x10000u + ((c - 0xD800u) << 10) + (c2 - 0xDC00u);
                i++;
            }
        }
        if (c < 0x80u) {
            if (o + 1 >= cap) break;
            out[o++] = (char)c;
        } else if (c < 0x800u) {
            if (o + 2 >= cap) break;
            out[o++] = (char)(0xC0u | (c >> 6));
            out[o++] = (char)(0x80u | (c & 0x3Fu));
        } else if (c < 0x10000u) {
            if (o + 3 >= cap) break;
            out[o++] = (char)(0xE0u | (c >> 12));
            out[o++] = (char)(0x80u | ((c >> 6) & 0x3Fu));
            out[o++] = (char)(0x80u | (c & 0x3Fu));
        } else {
            if (o + 4 >= cap) break;
            out[o++] = (char)(0xF0u | (c >> 18));
            out[o++] = (char)(0x80u | ((c >> 12) & 0x3Fu));
            out[o++] = (char)(0x80u | ((c >> 6) & 0x3Fu));
            out[o++] = (char)(0x80u | (c & 0x3Fu));
        }
    }
    out[o] = 0;
    return o;
}

static int is_svm_prefix(const unsigned char* s, int chars)
{
    if (chars < 4) return 0;
    return s[0] == 'S' && s[1] == 0 && s[2] == 'V' && s[3] == 0 &&
           s[4] == 'M' && s[5] == 0 && s[6] == '_' && s[7] == 0;
}

typedef struct {
    char name[64];
    unsigned int count;
    const unsigned char** ent;
    unsigned short* len;
} TabBlock;

static int tab_parse(const unsigned char* d, unsigned int n, TabBlock* blk, int* nblk)
{
    unsigned int p = 20, b, i;
    if (n < 20 || memcmp(d, "TAB0", 4) != 0) return 0;
    {
        unsigned int bc = pk_u32(d + 16);
        if (bc == 0 || bc > TAB_MAX_BLOCKS) return 0;
        *nblk = (int)bc;
    }
    for (b = 0; b < (unsigned int)*nblk; b++) {
        unsigned int nl, cnt;
        if (p + 3 > n || d[p] != 1 || d[p + 1] != 1 || d[p + 2] != 0) return 0;
        p += 3;
        if (p + 2 > n) return 0;
        nl = (unsigned int)d[p] | ((unsigned int)d[p + 1] << 8); p += 2;
        if (nl >= sizeof(blk[b].name) || p + 2 * nl + 4 > n) return 0;
        w2u8(d + p, (int)nl, blk[b].name, (int)sizeof(blk[b].name));
        p += 2 * nl;
        cnt = (unsigned int)d[p] | ((unsigned int)d[p + 1] << 8); p += 2;
        p += 2;                                  /* zero */
        blk[b].count = cnt;
        blk[b].ent = (const unsigned char**)malloc(sizeof(void*) * (cnt ? cnt : 1));
        blk[b].len = (unsigned short*)malloc(sizeof(unsigned short) * (cnt ? cnt : 1));
        if (!blk[b].ent || !blk[b].len) return 0;
        for (i = 0; i < cnt; i++) {
            unsigned int l;
            if (p + 2 > n) return 0;
            l = (unsigned int)d[p] | ((unsigned int)d[p + 1] << 8); p += 2;
            if (p + 2 * l > n) return 0;
            blk[b].ent[i] = d + p;
            blk[b].len[i] = (unsigned short)l;
            p += 2 * l;
        }
    }
    return 1;
}

static TabBlock* tab_block(TabBlock* blk, int n, const char* name)
{
    int i;
    for (i = 0; i < n; i++)
        if (_stricmp(blk[i].name, name) == 0) return &blk[i];
    return NULL;
}

/* 语言名/短码 → svms.tab 的列名前缀 */
static const char* lang_norm(const char* l)
{
    if (!_stricmp(l, "en") || !_stricmp(l, "english")) return "English";
    if (!_stricmp(l, "de") || !_stricmp(l, "german"))  return "German";
    if (!_stricmp(l, "fr") || !_stricmp(l, "french"))  return "French";
    if (!_stricmp(l, "it") || !_stricmp(l, "italian")) return "Italian";
    if (!_stricmp(l, "es") || !_stricmp(l, "spanish")) return "Spanish";
    if (!_stricmp(l, "pl") || !_stricmp(l, "polish"))  return "Polish";
    if (!_stricmp(l, "ru") || !_stricmp(l, "russian")) return "Russian";
    return l;
}

/* 引擎当前语音语言；取不到返回 NULL（调用方回退 English_Text） */
static const char* engine_lang(char* buf, int cap)
{
    int ok = 0, i;
    buf[0] = 0;
    if (!g_locinst || !g_getvoice) return NULL;
    __try {
        void* inst = g_locinst();
        if (inst) {
            char obj[128];
            memset(obj, 0, sizeof(obj));
            g_getvoice(inst, obj);
            read_bcstr(obj, buf, cap);
            ok = 1;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = 0;
    }
    if (!ok || !buf[0] || buf[0] == '<') return NULL;
    for (i = 0; buf[i]; i++) {
        char c = buf[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))) return NULL;
    }
    return buf;
}

/* Language=auto / English / French_Text ... → 表里的列名 */
static void lang_to_col(char* out, int cap)
{
    const char* l = g_lang;
    if (!l[0] || _stricmp(l, "auto") == 0) {
        char buf[64];
        const char* e = engine_lang(buf, sizeof(buf));
        l = e ? lang_norm(e) : "English";
    } else {
        if (strstr(l, "_Text") || strstr(l, "_text")) {
            strncpy(out, l, (size_t)(cap - 1));
            out[cap - 1] = 0;
            return;
        }
        l = lang_norm(l);
    }
    _snprintf(out, (size_t)cap, "%s_Text", l);
    out[cap - 1] = 0;
}

/* 找 <游戏根>\data\compiled\strings.pak：从 asi 所在目录逐级向上试 */
static int pak_path(char* out, int cap)
{
    char base[MAX_PATH];
    int up;
    strncpy(base, g_conf_dir, sizeof(base) - 1);
    base[sizeof(base) - 1] = 0;
    for (up = 0; up < 4; up++) {
        _snprintf(out, (size_t)cap, "%s\\data\\compiled\\strings.pak", base);
        out[cap - 1] = 0;
        if (GetFileAttributesA(out) != INVALID_FILE_ATTRIBUTES) return 1;
        {
            char* p = strrchr(base, '\\');
            if (!p) break;
            *p = 0;
        }
    }
    return 0;
}

static int pak_load(void)
{
    char path[MAX_PATH];
    HANDLE h;
    DWORD size = 0, got = 0;
    unsigned char* pak = NULL;
    unsigned char* tab = NULL;
    unsigned char* tab_own = NULL;
    unsigned int tablen = 0;
    PkFile pf;
    TabBlock blk[TAB_MAX_BLOCKS];
    int nblk = 0, i, ok = 0;
    TabBlock* kcol;
    TabBlock* tcol;
    char col[64];

    memset(blk, 0, sizeof(blk));
    if (!pak_path(path, sizeof(path))) {
        logf_("PAK not found (looked for data\\compiled\\strings.pak)");
        return 0;
    }
    logf_("PAK path = %s", path);

    h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) { logf_("PAK open failed err=%lu", GetLastError()); return 0; }
    size = GetFileSize(h, NULL);
    pak = (unsigned char*)malloc(size ? size : 1);
    if (!pak) { CloseHandle(h); return 0; }
    if (!ReadFile(h, pak, size, &got, NULL) || got != size) {
        logf_("PAK read failed got=%lu size=%lu", (unsigned long)got, (unsigned long)size);
        CloseHandle(h); free(pak); return 0;
    }
    CloseHandle(h);

    if (size < 48 || memcmp(pak + 4, "G3V0", 4) != 0) {
        logf_("PAK bad magic (size=%lu)", (unsigned long)size);
        free(pak); return 0;
    }
    {
        unsigned int table_off = (unsigned int)pk_u64(pak + 32);
        unsigned int total = (unsigned int)pk_u64(pak + 40);
        if (!pk_find(pak, size, table_off, total, "svms.tab", &pf)) {
            logf_("PAK svms.tab not found (table=0x%X total=0x%X)", table_off, total);
            free(pak); return 0;
        }
        logf_("PAK svms.tab off=0x%X stored=%u raw=%u", pf.off, pf.stored, pf.raw);

        if (pf.stored == pf.raw) {
            tab = pak + pf.off;
            tablen = pf.raw;
        } else {
            tab_own = (unsigned char*)malloc(pf.raw ? pf.raw : 1);
            if (!tab_own) { free(pak); return 0; }
            if (!inf_zlib(pak + pf.off, pf.stored, tab_own, pf.raw, &tablen) || tablen != pf.raw) {
                logf_("PAK inflate failed (got=%u want=%u)", tablen, pf.raw);
                free(tab_own); free(pak); return 0;
            }
            logf_("PAK inflate ok raw=%u", tablen);
            tab = tab_own;
        }
    }

    if (!tab_parse(tab, tablen, blk, &nblk)) {
        logf_("PAK TAB0 parse failed (len=%u)", tablen);
        goto done;
    }
    logf_("PAK TAB0 ok blocks=%d rows=%u", nblk, blk[0].count);

    kcol = tab_block(blk, nblk, "Key");
    lang_to_col(col, sizeof(col));
    tcol = tab_block(blk, nblk, col);
    if (!tcol && _stricmp(col, "English_Text") != 0) {
        logf_("PAK column %s missing, fallback English_Text", col);
        strcpy(col, "English_Text");
        tcol = tab_block(blk, nblk, col);
    }
    if (!kcol || !tcol) { logf_("PAK missing columns (Key=%p %s=%p)", (void*)kcol, col, (void*)tcol); goto done; }
    if (kcol->count != tcol->count) {
        logf_("PAK row mismatch Key=%u %s=%u", kcol->count, col, tcol->count);
        goto done;
    }

    {
        KV* nkv = NULL;
        int k = 0, n = 0;
        unsigned int r;
        for (r = 0; r < kcol->count; r++) {
            if (is_svm_prefix(kcol->ent[r], kcol->len[r]) && tcol->len[r] > 0) n++;
        }
        if (n <= 0) { logf_("PAK no SVM_ rows"); goto done; }
        nkv = (KV*)calloc((size_t)n, sizeof(KV));
        if (!nkv) goto done;
        for (r = 0; r < kcol->count; r++) {
            if (is_svm_prefix(kcol->ent[r], kcol->len[r]) && tcol->len[r] > 0) {
                int kl = (int)kcol->len[r], vl = (int)tcol->len[r];
                nkv[k].key = (char*)malloc((size_t)kl * 4 + 1);
                nkv[k].val = (char*)malloc((size_t)vl * 4 + 1);
                if (!nkv[k].key || !nkv[k].val) break;
                w2u8(kcol->ent[r], kl, nkv[k].key, kl * 4 + 1);
                w2u8(tcol->ent[r], vl, nkv[k].val, vl * 4 + 1);
                k++;
            }
        }
        if (k <= 0) { logf_("PAK build kv failed"); free_kv_arr(nkv, k); goto done; }
        free_kv();
        g_kv = nkv;
        g_kv_n = k;
        strncpy(g_loaded_col, col, sizeof(g_loaded_col) - 1);
        g_loaded_col[sizeof(g_loaded_col) - 1] = 0;
        g_pak_mode = 1;
        logf_("PAK loaded: %d keys, column=%s", g_kv_n, col);
        ok = 1;
    }

done:
    for (i = 0; i < nblk; i++) {
        if (blk[i].ent) free((void*)blk[i].ent);
        if (blk[i].len) free(blk[i].len);
    }
    if (tab_own) free(tab_own);
    free(pak);
    return ok;
}

static const char* lookup_text(const char* key)
{
    int i;
    for (i = 0; i < g_kv_n; i++)
        if (g_kv[i].key && strcmp(g_kv[i].key, key) == 0) return g_kv[i].val;
    return NULL;
}

/* v19：懒加载 —— 等游戏真正跑起来（第一次需要字幕时）再读 pak。
   原因：在 DllMain 线程里读 pak 会在引擎文本子系统就绪前抢占 strings.pak
   句柄、并提前触碰 eCLocAdmin2，导致主菜单文本回退成 HUD2_xxx 之类的 ID。 */
static int  g_text_inited = 0;
static char g_json_path[MAX_PATH];

static void ensure_text(void)
{
    if (g_text_inited) return;
    g_text_inited = 1;
    if (!pak_load()) {
        logf_("pak load failed, fallback to json");
        logf_("json path = %s", g_json_path);
        load_text_db(g_json_path);
    }
}

/* ---------- IAT 挂钩 ---------- */
static void* patch_iat(HMODULE mod, const char* dll_wanted, const char* name, void* repl, int* ok)
{
    IMAGE_DOS_HEADER* dos;
    IMAGE_NT_HEADERS64* nt;
    IMAGE_IMPORT_DESCRIPTOR* imp;
    DWORD old;
    int i;
    *ok = 0;
    if (!mod) return NULL;
    dos = (IMAGE_DOS_HEADER*)mod;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return NULL;
    nt = (IMAGE_NT_HEADERS64*)((BYTE*)mod + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != 0x20B) return NULL;
    imp = (IMAGE_IMPORT_DESCRIPTOR*)((BYTE*)mod + nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);
    if (!imp) return NULL;
    for (; imp->Name; imp++) {
        const char* dll = (const char*)((BYTE*)mod + imp->Name);
        IMAGE_THUNK_DATA64* ints;
        IMAGE_THUNK_DATA64* iat;
        if (dll_wanted && _stricmp(dll, dll_wanted) != 0) continue;
        ints = (IMAGE_THUNK_DATA64*)((BYTE*)mod + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        iat  = (IMAGE_THUNK_DATA64*)((BYTE*)mod + imp->FirstThunk);
        for (i = 0; ints[i].u1.AddressOfData; i++) {
            const char* fname;
            void* orig;
            if (ints[i].u1.Ordinal & IMAGE_ORDINAL_FLAG64) continue;
            fname = (const char*)((BYTE*)mod + ints[i].u1.AddressOfData + 2);
            if (strcmp(fname, name) != 0) continue;
            orig = (void*)iat[i].u1.Function;
            if (!VirtualProtect(&iat[i], sizeof(void*), PAGE_READWRITE, &old)) return NULL;
            iat[i].u1.Function = (ULONGLONG)repl;
            VirtualProtect(&iat[i], sizeof(void*), old, &old);
            *ok = 1;
            return orig;
        }
    }
    return NULL;
}

static DWORD WINAPI init_thread(LPVOID param)
{
    char dir[MAX_PATH], path[MAX_PATH];
    HMODULE game = NULL, shared = NULL, script = NULL, sgame = NULL, engine = NULL;
    int tick = 0;
    (void)param;

    GetModuleFileNameA(g_hinst, dir, sizeof(dir));
    {
        char* p = strrchr(dir, '\\');
        if (p) *p = 0;
    }
    strncpy(g_conf_dir, dir, MAX_PATH - 1);
    load_config();

    /* v17：先取语言 API —— 否则 pak_load 时 auto 还不知道玩家在游戏里设的语言 */
    {
        HMODULE eng0 = GetModuleHandleA("Engine.dll");
        if (eng0) {
            g_locinst  = (fn_locinst)GetProcAddress(eng0, "?GetInstance@eCLocAdmin2@@SAAEAV1@XZ");
            g_getvoice = (fn_getvoice)GetProcAddress(eng0, "?GetVoiceLanguage@eCLocAdmin2@@QEBA?AVbCString@@XZ");
            logf_("api locinst=%p getvoice=%p", (void*)g_locinst, (void*)g_getvoice);
        } else {
            logf_("Engine.dll not loaded yet, language API deferred");
        }
    }

    _snprintf(g_json_path, sizeof(g_json_path), "%s\\AmbientVoiceSubtitles.zh.json", dir);
    if (GetFileAttributesA(g_json_path) == INVALID_FILE_ATTRIBUTES)
        _snprintf(g_json_path, sizeof(g_json_path), "%s\\svms_zh.json", dir);   /* 兼容旧文件名 */
    logf_("=== AmbientVoiceSubtitles v20 loaded pid=%lu ===", (unsigned long)GetCurrentProcessId());
    logf_("text load deferred to first subtitle request (v19 lazy)");

    while (tick < 7200) {
        if (!game) game = GetModuleHandleA("Game.dll");
        if (!shared) shared = GetModuleHandleA("SharedBase.dll");
        if (!script) script = GetModuleHandleA("Script.dll");
        if (!sgame) sgame = GetModuleHandleA("Script_Game.dll");
        if (!engine) engine = GetModuleHandleA("Engine.dll");

        if (game && !g_addsmsg) {
            g_addsmsg = (fn_addsmsg)GetProcAddress(game, "?AddScreenMessage@gCGUIEntityRenderer@@QEAA_NPEBVeCGeometryEntity@@AEBVbCVector@@1AEBVbCUnicodeString@@_NKM@Z");
            g_getinst = (fn_getinst)GetProcAddress(game, "?GetInstance@gCGUIEntityRenderer@@SAAEAV1@XZ");
            if (g_addsmsg) logf_("api addsmsg=%p getinst=%p", g_addsmsg, g_getinst);
        }
        if (shared && !g_uni_ctor) {
            g_uni_ctor = (fn_ctor_uni)GetProcAddress(shared, "??0bCUnicodeString@@QEAA@PEB_W@Z");
            if (g_uni_ctor) logf_("api uni_ctor=%p", g_uni_ctor);
        }
        if (script && !g_getpos) {
            g_getpos = (fn_getpos)GetProcAddress(script, "?GetPosition@Entity@@QEBA?AVbCVector@@XZ");
            if (g_getpos) logf_("api GetPosition=%p", g_getpos);
        }
        if (engine && !g_pscount) {
            g_pscount = (fn_pscount)GetProcAddress(engine, "?GetPropertySetCount@eCEntity@@QEBAHXZ");
            g_psat = (fn_psat)GetProcAddress(engine, "?GetPropertySetAt@eCEntity@@QEBAPEBVeCEntityPropertySet@@H@Z");
            g_psgeom = (fn_psgeom)GetProcAddress(engine, "?GetGeometryEntity@eCEntityPropertySet@@QEBAPEBVeCGeometryEntity@@XZ");
            if (g_pscount) logf_("api pscount=%p psat=%p psgeom=%p", g_pscount, g_psat, g_psgeom);
        }
        if (engine && !g_worldmat) {
            g_worldmat = (fn_worldmat)GetProcAddress(engine, "?GetWorldMatrix@eCGeometryEntity@@QEBAAEBVbCMatrix@@XZ");
            if (g_worldmat) logf_("api worldmat=%p", g_worldmat);
        }
        if (engine && !g_locinst && !g_lang_tried) {
            g_lang_tried = 1;
            g_locinst  = (fn_locinst)GetProcAddress(engine, "?GetInstance@eCLocAdmin2@@SAAEAV1@XZ");
            g_getvoice = (fn_getvoice)GetProcAddress(engine, "?GetVoiceLanguage@eCLocAdmin2@@QEBA?AVbCString@@XZ");
            logf_("api locinst=%p getvoice=%p (late)", (void*)g_locinst, (void*)g_getvoice);
            if (g_locinst && g_getvoice && g_pak_mode) {
                char col[64];
                lang_to_col(col, sizeof(col));
                if (_stricmp(col, g_loaded_col) != 0) {
                    logf_("LANG now=%s (loaded %s), reloading pak", col, g_loaded_col);
                    pak_load();
                }
            }
        }
        if (engine && !g_getcam) {
            g_appinst = (fn_appinst)GetProcAddress(engine, "?GetInstance@eCApplication@@SAAEAV1@XZ");
            g_getcam  = (fn_getcam)GetProcAddress(engine, "?GetCurrentCamera@eCApplication@@QEAAAEAVeCCameraBase@@XZ");
            g_getmain = (fn_getcam)GetProcAddress(engine, "?GetMainCamera@eCApplication@@QEAAAEAVeCCameraBase@@XZ");
            g_campos  = (fn_campos)GetProcAddress(engine, "?AccessPosition@eCCameraBase@@QEAAAEAVbCVector@@XZ");
            if (g_appinst) logf_("api appinst=%p getcam=%p getmain=%p campos=%p", g_appinst, g_getcam, g_getmain, g_campos);
        }
        if (game && !g_hudsys) {
            g_hudsys = (fn_hudsys)GetProcAddress(game, "?GetSystem@gCHudModule2@@SAPEAVgCHudSystem2@@XZ");
            g_showsub = (fn_showsub)GetProcAddress(game, "?ShowSubtitle@gCHudSystem2@@QEAAXAEBVbCUnicodeString@@@Z");
            if (g_hudsys) logf_("api hudsys=%p showsub=%p", g_hudsys, g_showsub);
        }
        if (game && !g_drawmsg) {
            g_drawmsg = (fn_drawmsg)GetProcAddress(game, "?DrawScreenMessage@gCGUIEntityRenderer@@IEAA_NPEAVeCGuiDevice2@@PEAVeCSprite@@AEBVbCMatrix@@AEBVbCVector@@3AEBVbCUnicodeString@@KM_N@Z");
            if (g_drawmsg) logf_("api drawmsg=%p", g_drawmsg);
        }
        if (game && !g_onb_addr) {
            g_onb_addr = GetProcAddress(game, "?OnBeginFrame@gCGUIEntityRenderer@@UEAAXPEAVeCGuiDevice2@@PEAVeCSprite@@@Z");
            if (g_onb_addr) logf_("api onbegin=%p", g_onb_addr);
        }
        if (script && !g_title_orig) {
            int ok = 0;
            void* o = patch_iat(script, "Game.dll",
                "?FormatSampleTitle@gCSvmAdmin@@QEBA_NAEBVbCString@@0AEAV2@@Z", (void*)det_title, &ok);
            if (ok) { g_title_orig = (fn_fmt)o; logf_("hooked FormatSampleTitle orig=%p", o); }
        }
        if (sgame && !g_saysvm_orig) {
            int ok = 0;
            void* o = patch_iat(sgame, "Script.dll",
                "?StartSaySVM@Entity@@QEAAXAEAV1@VbCString@@_N2@Z", (void*)det_saysvm, &ok);
            if (ok) { g_saysvm_orig = (fn_saysvm)o; logf_("hooked StartSaySVM orig=%p", o); }
        }
        if (g_title_orig && g_saysvm_orig && g_addsmsg && g_uni_ctor && g_pscount) {
            snapshot_modules();
            logf_("=== observation start (mods=%d getpos=%p) ===", g_nmods, g_getpos);
            if (g_getinst) {
                void* r = g_getinst();
                logf_("renderer=%p", r);
                if (r) hook_vtable(r);
            }
            break;
        }
        Sleep(300);
        tick++;
    }
    if (!(g_title_orig && g_saysvm_orig)) logf_("WARN hooks not armed");
    return 0;
}

__declspec(dllexport) void InitializeASI(void) { logf_("InitializeASI() called"); }

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        char path[MAX_PATH], *p;
        g_hinst = hinst;
        DisableThreadLibraryCalls(hinst);
        InitializeCriticalSection(&g_cs);
        if (GetModuleFileNameA(hinst, path, sizeof(path))) {
            p = strrchr(path, '\\');
            if (p) strcpy(p + 1, "AmbientVoiceSubtitles.log"); else strcpy(path, "AmbientVoiceSubtitles.log");
            g_log = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        }
        logf_("DllMain ATTACH");
        CreateThread(NULL, 0, init_thread, NULL, 0, NULL);
    } else if (reason == DLL_PROCESS_DETACH) {
        logf_("DllMain DETACH drawn=%d", g_drawn);
    }
    return TRUE;
}
