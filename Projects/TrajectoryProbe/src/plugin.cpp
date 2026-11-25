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
float g_v0     = 52.0f;  // tp_v0: guessed full-draw speed (m/s) for the TEST arc only
float g_g      = 9.0f;   // tp_g: gravity used by the TEST arc
float g_full   = 1.2f;   // tp_full: seconds to full draw (vanilla BowChargeDurationMax)

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
bool g_relPredOk = false; Vec3 g_relPred{}; float g_relPredSpeed = 0.0f;
bool g_predOk = false; Vec3 g_pred{}; float g_predSpeed = 0.0f;   // predicted impact of the last drawn frame

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
        g_relPredOk = g_predOk; g_relPred = g_pred; g_relPredSpeed = g_predSpeed;
        if (g_log && g_predOk) TP_LOG("  predicted impact (%.3f,%.3f,%.3f) v=%.1f", g_pred.x, g_pred.y, g_pred.z, g_predSpeed);
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

// ------------------------------------------------------------------ physics ray --------
// IPhysicalWorld::RayWorldIntersection inline helper of WHGame 1.5.6 (RVA 0x4846E4), the one the
// 3D engine's GetBottomLevel uses: (world, &org, &dir(len=ray length), objtypes, flags, hits,
// nMaxHits, pSkipEnts, nSkipEnts, pForeignData, iForeignData, nameTag).  The prologue bytes are
// checked first; on mismatch raycasting is disabled and the arc is drawn without collision.
struct RayHit { float dist; void* pCollider; int ipart, partid; short surface_idx, idmatOrg;
                int foreignIdx, iNode; Vec3 pt; Vec3 n; int bTerrain, iPrim; void* next; char pad[32]; };
using RwiFn = int(__fastcall*)(void* world, const Vec3* org, const Vec3* dir, int objtypes, unsigned flags,
                               RayHit* hits, int nMax, void* skip, int nSkip, void* fd, int ifd, const char* tag);
RwiFn g_rwi = nullptr;

void InitRay()
{
    const auto addr = REL::Offset(0x4846E4).address();
    static const unsigned char sig[] = { 0x48,0x8B,0xC4,0x48,0x89,0x58,0x08,0x48,0x89,0x70,0x10,0x48,0x89,0x78,0x18 };
    if (std::memcmp(reinterpret_cast<void*>(addr), sig, sizeof(sig)) == 0) g_rwi = reinterpret_cast<RwiFn>(addr);
    TP_LOG("raycast: %s", g_rwi ? "ok" : "signature mismatch, collision off");
}

bool Ray(const Vec3& a, const Vec3& b, Vec3& hit, Vec3& nrm)
{
    auto* env = SSystemGlobalEnvironment::GetInstance();
    if (!g_rwi || !env || !env->pPhysicalWorld) return false;
    const Vec3 dir = b - a;
    RayHit h{};
    int n = 0;
    __try {
        n = g_rwi(env->pPhysicalWorld, &a, &dir, 0x11F /*ent_all*/, 0x2000F, &h, 1, nullptr, 0, nullptr, 0, "TrajectoryProbe");
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_rwi = nullptr; TP_LOG("raycast faulted -- collision off"); return false; }
    if (n <= 0 || h.dist <= 0.0f) return false;
    hit = h.pt; nrm = h.n;
    return true;
}

// ------------------------------------------------------------------ HUD drawing --------
float g_width  = 0.035f;   // tp_width : ribbon width in metres (world space -> perspective thinning)
float g_vmin   = 0.10f;    // tp_vmin  : speed fraction at zero draw
float g_ox = 0.06f, g_oy = 0.35f, g_oz = -0.12f;  // tp_ox/oy/oz: launch point rel. to camera (right, fwd, up)
float g_skip   = 0.8f;     // tp_skip  : metres of path ignored by collision (own body / bow)

std::shared_ptr<Offsets::IFlashPlayer> HudPlayer()
{
    auto* env = SSystemGlobalEnvironment::GetInstance();
    if (!env || !env->pFlashUI) return nullptr;
    _smart_ptr<Offsets::IUIElement> elem;
    env->pFlashUI->GetUIElement(elem, "hud");
    if (!elem) return nullptr;
    return elem->GetFlashPlayer();
}

float g_stageW = 1920.0f, g_stageH = 1080.0f;

bool ToStage(const Vec3& w, float& x, float& y)
{
    auto* env = SSystemGlobalEnvironment::GetInstance();
    float sx, sy, sz;
    if (!env->pRenderer->ProjectToScreen(w.x, w.y, w.z, &sx, &sy, &sz)) return false;
    if (sz > 1.0f || sz < 0.0f) return false;
    // ProjectToScreen gives percent of the (super-resolution) render target; the HUD movie
    // has its own stage size, so map percent -> stage units (v2 used render pixels: 2/3 offset).
    x = sx * 0.01f * g_stageW;
    y = sy * 0.01f * g_stageH;
    return true;
}

bool  g_hadDrawing = false;

void Call(Offsets::FlashVarPtr& mc, const char* fn, const SFlashVarValue* a, unsigned n)
{
    mc->Invoke(fn, a, n, static_cast<SFlashVarValue*>(nullptr));
}

void DrawArc(bool show)
{
    auto fp = HudPlayer();
    if (!fp) return;
    Offsets::FlashVarPtr mc;
    if (!fp->GetVariable("_root.tpProbe", mc.put()) || !mc) {
        Offsets::FlashVarPtr root;
        if (!fp->GetVariable("_root", root.put()) || !root) return;
        if (!root->CreateEmptyMovieClip(mc.put(), "tpProbe", 16000) || !mc) return;
        auto* env = SSystemGlobalEnvironment::GetInstance();
        if (fp->GetWidth() > 0) { g_stageW = static_cast<float>(fp->GetWidth()); g_stageH = static_cast<float>(fp->GetHeight()); }
        TP_LOG("HUD clip created stage=%dx%d render=%dx%d", fp->GetWidth(), fp->GetHeight(),
               env->pRenderer->GetWidth(), env->pRenderer->GetHeight());
    }
    if (g_hadDrawing) { Call(mc, "clear", nullptr, 0); g_hadDrawing = false; }
    if (!show) return;

    Vec3 cam{}, fwd{};
    if (!ViewCamera(cam, fwd)) return;
    const Vec3 up(0, 0, 1);
    Vec3 right = fwd.Cross(up); if (right.GetLengthSquared() < 1e-6f) right = Vec3(1, 0, 0); right.Normalize();
    const Vec3 camUp = right.Cross(fwd);
    const Vec3 p0 = cam + right * g_ox + fwd * g_oy + camUp * g_oz;

    const KeyState& k = g_mouse1.down ? g_mouse1 : g_trigR;
    float ratio = static_cast<float>((Now() - k.since) / (g_full > 0.01f ? g_full : 1.2f));
    ratio = ratio < 0 ? 0 : (ratio > 1 ? 1 : ratio);
    const float speed = g_v0 * (g_vmin + (1.0f - g_vmin) * ratio);
    Vec3 vel = fwd * speed;

    // --- simulate: analytic parabola sampled with growing step, collision per segment ----------
    constexpr int kMax = 72;
    Vec3 pts[kMax + 1]; Vec3 vels[kMax + 1];
    int n = 0; pts[0] = p0; vels[0] = vel;
    float path = 0.0f; bool hit = false; Vec3 hp{}, hn(0, 0, 1);
    float t = 0.0f;
    for (int i = 1; i <= kMax; ++i) {
        const float dt = 0.012f + 0.004f * i;          // dense near the bow, sparse far away
        t += dt;
        const Vec3 p(p0.x + vel.x * t, p0.y + vel.y * t, p0.z + vel.z * t - 0.5f * g_g * t * t);
        const Vec3 v(vel.x, vel.y, vel.z - g_g * t);
        const Vec3& prev = pts[i - 1];
        const float seg = (p - prev).GetLength();
        if (path + seg > g_skip) {
            Vec3 h{}, nn{};
            const Vec3 a = (path < g_skip) ? prev + (p - prev) * ((g_skip - path) / seg) : prev;
            if (Ray(a, p, h, nn)) { pts[i] = h; vels[i] = v; n = i; hit = true; hp = h; hn = nn; break; }
        }
        path += seg; pts[i] = p; vels[i] = v; n = i;
        if (path > 300.0f) break;
    }
    g_predOk = hit; g_pred = hp; g_predSpeed = speed;

    // --- ribbon: world-space strip of width tp_width, edges projected -> perspective thinning ---
    float lx[kMax + 1], ly[kMax + 1], rx[kMax + 1], ry[kMax + 1]; bool ok[kMax + 1];
    float dist[kMax + 1];
    for (int i = 0; i <= n; ++i) {
        Vec3 side = vels[i].Cross(up);
        if (side.GetLengthSquared() < 1e-6f) side = right; side.Normalize();
        const Vec3 L = pts[i] - side * (g_width * 0.5f), R = pts[i] + side * (g_width * 0.5f);
        ok[i] = ToStage(L, lx[i], ly[i]) && ToStage(R, rx[i], ry[i]);
        dist[i] = (pts[i] - cam).GetLength();
    }
    // bands: alpha fades in over the first metres (no fat blob at the bow) and out at range
    const SFlashVarValue outline[3] = { 1.0f, 0x101010u, 45.0f };
    Call(mc, "lineStyle", outline, 3);
    constexpr int kBand = 6;
    for (int b0 = 0; b0 < n; b0 += kBand) {
        const int b1 = (b0 + kBand < n) ? b0 + kBand : n;
        bool all = true; for (int i = b0; i <= b1; ++i) all = all && ok[i];
        if (!all) continue;
        const float d = dist[(b0 + b1) / 2];
        float a = 80.0f;
        if (d < 3.0f) a *= (d - 0.4f) / 2.6f;
        if (d > 60.0f) a *= 60.0f / d;
        if (a < 4.0f) continue;
        const SFlashVarValue fill[2] = { 0xF2EEDCu, a };
        Call(mc, "beginFill", fill, 2);
        { const SFlashVarValue p[2] = { lx[b0], ly[b0] }; Call(mc, "moveTo", p, 2); }
        for (int i = b0 + 1; i <= b1; ++i) { const SFlashVarValue p[2] = { lx[i], ly[i] }; Call(mc, "lineTo", p, 2); }
        for (int i = b1; i >= b0; --i)     { const SFlashVarValue p[2] = { rx[i], ry[i] }; Call(mc, "lineTo", p, 2); }
        Call(mc, "endFill", nullptr, 0);
    }
    // --- impact marker: ring lying on the hit surface (world circle -> perspective ellipse) ----
    if (hit) {
        Vec3 nn = hn; if (nn.GetLengthSquared() < 1e-4f) nn = up; nn.Normalize();
        Vec3 u = nn.Cross(std::fabs(nn.z) < 0.9f ? up : Vec3(1, 0, 0)); u.Normalize();
        const Vec3 w = nn.Cross(u);
        const Vec3 c = hp + nn * 0.02f;
        const float ro = 0.22f, ri = 0.15f;
        float ox[25], oy[25], ix[25], iy[25]; bool good = true;
        for (int i = 0; i <= 24 && good; ++i) {
            const float th = 6.2831853f * i / 24.0f, cs = std::cos(th), sn = std::sin(th);
            good = ToStage(c + u * (cs * ro) + w * (sn * ro), ox[i], oy[i]) &&
                   ToStage(c + u * (cs * ri) + w * (sn * ri), ix[i], iy[i]);
        }
        if (good) {
            const SFlashVarValue fill[2] = { 0xFFD27Au, 85.0f };
            Call(mc, "beginFill", fill, 2);
            { const SFlashVarValue p[2] = { ox[0], oy[0] }; Call(mc, "moveTo", p, 2); }
            for (int i = 1; i <= 24; ++i) { const SFlashVarValue p[2] = { ox[i], oy[i] }; Call(mc, "lineTo", p, 2); }
            { const SFlashVarValue p[2] = { ix[0], iy[0] }; Call(mc, "moveTo", p, 2); }   // inner path -> hole
            for (int i = 1; i <= 24; ++i) { const SFlashVarValue p[2] = { ix[i], iy[i] }; Call(mc, "lineTo", p, 2); }
            Call(mc, "endFill", nullptr, 0);
        }
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
                if (g_relPredOk) {
                    const Vec3 e = h - g_relPred;
                    TP_LOG("  pred=(%.3f,%.3f,%.3f) err=%.2f (dz=%.2f) v=%.1f", g_relPred.x, g_relPred.y, g_relPred.z,
                           e.GetLength(), e.z, g_relPredSpeed);
                }
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
    DrawArc(g_draw && (g_mouse1.down || g_trigR.down));
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
            env->pConsole->RegisterCVarFloat("tp_width", &g_width, g_width, VF_NULL, "TrajectoryProbe: ribbon width (m)");
            env->pConsole->RegisterCVarFloat("tp_vmin", &g_vmin, g_vmin, VF_NULL, "TrajectoryProbe: speed fraction at zero draw");
            env->pConsole->RegisterCVarFloat("tp_ox", &g_ox, g_ox, VF_NULL, "TrajectoryProbe: launch offset right (m)");
            env->pConsole->RegisterCVarFloat("tp_oy", &g_oy, g_oy, VF_NULL, "TrajectoryProbe: launch offset forward (m)");
            env->pConsole->RegisterCVarFloat("tp_oz", &g_oz, g_oz, VF_NULL, "TrajectoryProbe: launch offset up (m)");
            env->pConsole->RegisterCVarFloat("tp_skip", &g_skip, g_skip, VF_NULL, "TrajectoryProbe: path metres ignored by collision");
        }
        LoadXInput();
        InitRay();
        KCSE::GetTaskInterface()->AddTask(&Tick);
        TP_LOG("started");
    });
    TP_LOG("loaded (hit hook %s)", hooked ? "ok" : "FAILED");
    return true;
}
