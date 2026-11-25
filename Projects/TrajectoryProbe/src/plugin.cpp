// TrajectoryProbe -- measurement build for a ranged-weapon trajectory preview (KCD2 1.5.6).
// v2: input polled (mouse GetAsyncKeyState + XInput pad), proximity scan removed.
// Logs (to kcd.log, prefix [TP]):
//   * every press/release of the fire/aim keys with the game time (draw duration),
//   * the view camera position/direction at each release,
//   * the flight track of every fast-moving entity that appears near the camera right after a
//     release (projectile spawn point, initial velocity, gravity -- fitted offline),
//   * the exact impact point of the player's missile hits on actors (S_DamageEventData).
// Draws a TEST arc on the HUD while the fire key is held, to verify the drawing pipeline
// (HUD movie clip + Scaleform drawing API).  The arc uses guessed values (cvars tp_v0, tp_g);
// it is NOT the final prediction.
#include <MinHook.h>
#include <windows.h>
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>
#include <unordered_map>
#include <unordered_set>

#include "KCSE/KCSEAPI.h"
#include "crysystem/CCryAction.h"
#include "crysystem/SSystemGlobalEnvironment.h"
#include "REL.h"
#include "Offsets/vtables/ITimer.h"

namespace {

#define TP_LOG(fmt, ...) do { \
    if (auto* _env = SSystemGlobalEnvironment::GetInstance(); _env && _env->pLog) \
        _env->pLog->LogAlways("[TP] " fmt, ##__VA_ARGS__); \
} while (0)

int   g_draw   = 1;      // tp_draw: test arc on/off
int   g_log    = 1;      // tp_log
float g_v0     = 50.0f;  // tp_v0: guessed full-draw speed (m/s) for the TEST arc only
float g_g      = 9.0f;   // tp_g: gravity used by the TEST arc
float g_full   = 2.0f;   // tp_full: seconds to full draw (vanilla BowChargeDurationMax)

double Now()
{
    auto* env = SSystemGlobalEnvironment::GetInstance();
    return (env && env->pTimer) ? env->pTimer->GetCurrTimeD(Offsets::ITimer::ETIMER_GAME) : 0.0;
}

bool ViewCamera(Vec3& pos, Vec3& dir)
{
    auto* env = SSystemGlobalEnvironment::GetInstance();
    if (!env || !env->pSystem) return false;
    const CCamera& cam = env->pSystem->GetViewCamera();
    pos = cam.m_Matrix.GetTranslation();
    dir = cam.GetViewdir();
    return true;
}

// ------------------------------------------------------------------ input (polled) ------
// v2: the game's action maps consume fire/aim input before IInputEventListener sees it, so the
// buttons are polled every frame straight from Windows: mouse via GetAsyncKeyState, pad via XInput
// (analog triggers 0..255, also logged so the draw input is known).
struct KeyState { const char* name; bool down = false; double since = 0.0; int peak = 0; };
KeyState g_mouse1{ "mouse1" }, g_mouse2{ "mouse2" }, g_trigR{ "pad_RT" }, g_trigL{ "pad_LT" };
double   g_lastRelease = -100.0;
const char* g_lastReleaseKey = "";
double   g_lastHeld = 0.0;
Vec3     g_relCam{}, g_relDir{};

struct XPad { WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; };
struct XState { DWORD packet; XPad pad; };
using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XState*);
XInputGetStateFn g_xget = nullptr;

void LoadXInput()
{
    const char* dlls[] = { "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll" };
    for (auto* n : dlls) {
        if (HMODULE h = LoadLibraryA(n)) {
            g_xget = reinterpret_cast<XInputGetStateFn>(GetProcAddress(h, "XInputGetState"));
            if (g_xget) { TP_LOG("xinput: %s", n); return; }
        }
    }
    TP_LOG("xinput: not found (pad polling off)");
}

bool GameFocused()
{
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

void Step(KeyState& k, bool down, int value)
{
    const double t = Now();
    if (down && !k.down) {
        k.down = true; k.since = t; k.peak = value;
        if (g_log) TP_LOG("press %s t=%.3f val=%d", k.name, t, value);
    } else if (down) {
        if (value > k.peak) k.peak = value;
    } else if (k.down) {
        k.down = false;
        const double held = t - k.since;
        Vec3 p{}, d{};
        ViewCamera(p, d);
        if (g_log) TP_LOG("release %s t=%.3f held=%.3f peak=%d cam=(%.3f,%.3f,%.3f) dir=(%.4f,%.4f,%.4f)",
                          k.name, t, held, k.peak, p.x, p.y, p.z, d.x, d.y, d.z);
        g_lastRelease = t; g_lastReleaseKey = k.name; g_lastHeld = held; g_relCam = p; g_relDir = d;
    }
}

void PollInput()
{
    const bool focus = GameFocused();
    Step(g_mouse1, focus && (GetAsyncKeyState(VK_LBUTTON) & 0x8000), 255);
    Step(g_mouse2, focus && (GetAsyncKeyState(VK_RBUTTON) & 0x8000), 255);
    int lt = 0, rt = 0;
    if (g_xget) {
        for (DWORD i = 0; i < 4; ++i) {
            XState st{};
            if (g_xget(i, &st) == ERROR_SUCCESS) { lt = st.pad.lt; rt = st.pad.rt; break; }
        }
    }
    Step(g_trigR, rt > 30, rt);
    Step(g_trigL, lt > 30, lt);
}

// periodic sample while something is held: camera + trigger, so draw/aim behaviour is visible
double g_lastSample = 0.0;
void SampleHeld()
{
    const double t = Now();
    if (t - g_lastSample < 0.25) return;
    g_lastSample = t;
    Vec3 p{}, d{};
    ViewCamera(p, d);
    TP_LOG("hold t=%.3f m1=%d m2=%d RT=%d LT=%d cam=(%.3f,%.3f,%.3f) dir=(%.4f,%.4f,%.4f)",
           t, g_mouse1.down ? 1 : 0, g_mouse2.down ? 1 : 0, g_trigR.down ? g_trigR.peak : 0, g_trigL.down ? g_trigL.peak : 0,
           p.x, p.y, p.z, d.x, d.y, d.z);
}

// ------------------------------------------------------------------ HUD drawing --------
std::shared_ptr<Offsets::IFlashPlayer> HudPlayer()
{
    auto* env = SSystemGlobalEnvironment::GetInstance();
    if (!env || !env->pFlashUI) return nullptr;
    _smart_ptr<Offsets::IUIElement> elem;
    env->pFlashUI->GetUIElement(elem, "hud");
    if (!elem) return nullptr;
    return elem->GetFlashPlayer();
}

bool ToScreen(const Vec3& w, float& x, float& y)
{
    auto* env = SSystemGlobalEnvironment::GetInstance();
    float sx, sy, sz;
    if (!env->pRenderer->ProjectToScreen(w.x, w.y, w.z, &sx, &sy, &sz)) return false;
    if (sz > 1.0f || sz < 0.0f) return false;
    x = sx * 0.01f * env->pRenderer->GetWidth();
    y = sy * 0.01f * env->pRenderer->GetHeight();
    return true;
}

bool g_clipReady = false;
bool g_hadDrawing = false;

void DrawArc(bool show)
{
    auto fp = HudPlayer();
    if (!fp) { g_clipReady = false; return; }
    Offsets::FlashVarPtr mc;
    if (!fp->GetVariable("_root.tpProbe", mc.put()) || !mc) {
        Offsets::FlashVarPtr root;
        if (!fp->GetVariable("_root", root.put()) || !root) return;
        if (!root->CreateEmptyMovieClip(mc.put(), "tpProbe", 16000) || !mc) return;
        TP_LOG("HUD clip created");
    }
    if (!show) {
        if (g_hadDrawing) { mc->Invoke("clear", static_cast<const SFlashVarValue*>(nullptr), 0u, static_cast<SFlashVarValue*>(nullptr)); g_hadDrawing = false; }
        return;
    }
    mc->Invoke("clear", static_cast<const SFlashVarValue*>(nullptr), 0u, static_cast<SFlashVarValue*>(nullptr));
    Vec3 p0{}, d{};
    if (!ViewCamera(p0, d)) return;
    const KeyState& k = g_mouse1.down ? g_mouse1 : (g_trigR.down ? g_trigR : (g_trigL.down ? g_trigL : g_mouse2));
    float ratio = static_cast<float>((Now() - k.since) / (g_full > 0.01f ? g_full : 2.0f));
    if (ratio > 1.0f) ratio = 1.0f;
    const float v = g_v0 * (0.3f + 0.7f * ratio);
    const Vec3 v0(d.x * v, d.y * v, d.z * v);
    SFlashVarValue ls[3] = { 2.0f, 0xFFFFFFu, 55.0f };
    mc->Invoke("lineStyle", ls, 3u, static_cast<SFlashVarValue*>(nullptr));
    bool pen = false;
    for (int i = 1; i <= 60; ++i) {
        const float t = 0.02f * i * (1.0f + i * 0.05f);
        const Vec3 p(p0.x + v0.x * t, p0.y + v0.y * t, p0.z + v0.z * t - 0.5f * g_g * t * t);
        float x, y;
        if (!ToScreen(p, x, y)) { pen = false; continue; }
        SFlashVarValue a[2] = { x, y };
        mc->Invoke(pen ? "lineTo" : "moveTo", a, 2u, static_cast<SFlashVarValue*>(nullptr));
        pen = true;
    }
    g_hadDrawing = true;
}

// ------------------------------------------------------------------ missile hits -------
class {
public:
    static bool Install()
    {
        void* target = reinterpret_cast<void*>(REL::ID(20).address());
        return MH_CreateHook(target, reinterpret_cast<void*>(&thunk), reinterpret_cast<void**>(&orig)) == MH_OK &&
               MH_EnableHook(target) == MH_OK;
    }
protected:
    static int64_t thunk(wh::rpgmodule::S_DamageEventData* ctx)
    {
        const int64_t ret = orig(ctx);
        __try {
            if (g_log && ctx->m_type == wh::rpgmodule::E_DamageEventType::MissileHit) {
                const Vec3& h = ctx->m_hitPosition;
                const float dx = h.x - g_relCam.x, dy = h.y - g_relCam.y, dz = h.z - g_relCam.z;
                TP_LOG("hit t=%.3f pos=(%.3f,%.3f,%.3f) missile=%llu dtRelease=%.3f held=%.3f key=%s dist=%.2f",
                       Now(), h.x, h.y, h.z, static_cast<unsigned long long>(ctx->m_missileWuid.m_value),
                       Now() - g_lastRelease, g_lastHeld, g_lastReleaseKey, std::sqrt(dx*dx+dy*dy+dz*dz));
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        return ret;
    }
    static inline REL::Relocation<decltype(&thunk)> orig;
} hkDamage;

// ------------------------------------------------------------------ tick ---------------
void Tick()
{
    PollInput();
    const bool holding = g_mouse1.down || g_mouse2.down || g_trigR.down || g_trigL.down;
    if (holding && g_log) SampleHeld();
    DrawArc(g_draw && holding);
    KCSE::GetTaskInterface()->AddTask(&Tick);
}

bool g_started = false;

}  // namespace

KCSE_PLUGIN_INFO("TrajectoryProbe", "zz_levelcap modpack", 1);
KCSE_PLUGIN_LOAD(kcse)
{
    if (MH_Initialize() != MH_OK && MH_Initialize() != MH_ERROR_ALREADY_INITIALIZED) return false;
    const bool hooked = hkDamage.Install();
    kcse->GetMessagingInterface()->RegisterListener([](KCSE::Message* msg) {
        auto* env = SSystemGlobalEnvironment::GetInstance();
        if (!env || msg->type != KCSE::IMessagingInterface::kMessage_DataLoaded || g_started) return;
        g_started = true;
        if (env->pConsole) {
            env->pConsole->RegisterCVarInt("tp_draw", &g_draw, g_draw, VF_NULL, "TrajectoryProbe: test arc on/off");
            env->pConsole->RegisterCVarInt("tp_log", &g_log, g_log, VF_NULL, "TrajectoryProbe: logging on/off");
            env->pConsole->RegisterCVarFloat("tp_v0", &g_v0, g_v0, VF_NULL, "TrajectoryProbe: TEST arc full-draw speed");
            env->pConsole->RegisterCVarFloat("tp_g", &g_g, g_g, VF_NULL, "TrajectoryProbe: TEST arc gravity");
            env->pConsole->RegisterCVarFloat("tp_full", &g_full, g_full, VF_NULL, "TrajectoryProbe: seconds to full draw");
        }
        LoadXInput();
        KCSE::GetTaskInterface()->AddTask(&Tick);
        TP_LOG("started");
    });
    TP_LOG("loaded (hit hook %s)", hooked ? "ok" : "FAILED");
    return true;
}
