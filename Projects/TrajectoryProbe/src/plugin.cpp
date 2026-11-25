// TrajectoryProbe v4 -- real-time ranged trajectory preview for KCD2 1.5.6 (Steam, WHGame TimeDateStamp 0x6A350E20).
//
// How it works (all addresses reverse-engineered from WHGame 1.5.6, see FINDINGS):
//   * Runs ONLY inside the game's own shooting-action updates (C_ActorActionShootingCharging / Aiming /
//     InstantCharging ::Update, vtable slot 41).  Those exist only while a ranged weapon is being drawn or
//     held, so nothing ever runs in menus, loading screens, melee, dialogue...
//   * Draw progress = Charging+0xA8 (0..1, exactly what the game passes to the shot on release); Aiming = 1.
//   * Launch point + direction = the game's GetFireFrame (0x289B28C) -> the arrow tip, no guessing.
//   * Launch speed = the game's own speed function (0x289B594): Power, limb, arrow weight, draw, perks.
//   * Path = analytic parabola (arrow.xml: gravity 9.0, no drag); collision = the same RayWorldIntersection
//     helper and flags (objtypes 0x11F, flags 0x40F) the game's fire routine uses.
//   * Drawing = engine aux geometry, world-space triangles: a horizontal ribbon of fixed metric width, so it
//     thins with distance by itself and is hidden behind geometry (depth test on).  Impact = ring lying on
//     the hit surface.
// The build refuses to run on any other WHGame build (TimeDateStamp check).
#include <MinHook.h>
#include <windows.h>
#include <cmath>
#include <cstring>
#include <cstdint>

#include "KCSE/KCSEAPI.h"
#include "crysystem/CCryAction.h"
#include "crysystem/SSystemGlobalEnvironment.h"
#include "REL.h"

namespace {

#define TP_LOG(fmt, ...) do { \
    if (auto* _env = SSystemGlobalEnvironment::GetInstance(); _env && _env->pLog) \
        _env->pLog->LogAlways("[TP] " fmt, ##__VA_ARGS__); \
} while (0)

constexpr std::uint32_t kGameStamp = 0x6A350E20;

// ---- game RVAs (WHGame 1.5.6) ----------------------------------------------------------------
constexpr std::size_t kChargingUpdate = 0x28A1044;   // C_ActorActionShootingCharging vt[41]
constexpr std::size_t kAimingUpdate   = 0x28A0E70;   // C_ActorActionShootingAiming vt[41]
constexpr std::size_t kInstantUpdate  = 0x28A126C;   // C_ActorActionShootingInstantCharging vt[41]
constexpr std::size_t kShootCtx       = 0x7F3310;    // (actor+0x280 obj) -> ctx {+0x40 weapon, +0x48 ammo}
constexpr std::size_t kGetFireFrame   = 0x289B28C;   // (_, out{pos,axA,dir}, actor, weapon)
constexpr std::size_t kLaunchSpeed    = 0x289B594;   // (_, float draw, weapon, ammo, stats) -> m/s
constexpr std::size_t kRwiHelper      = 0x4846E4;    // RayWorldIntersection inline helper

// ---- settings (console cvars) -----------------------------------------------------------------
int   g_on     = 1;        // tp_on
int   g_log    = 1;        // tp_log
float g_width  = 0.035f;   // tp_width : ribbon width (m)
float g_grav   = 9.0f;     // tp_g     : gravity (arrow.xml)
float g_ring   = 0.18f;    // tp_ring  : impact ring radius (m)

bool g_ready = false;      // all checks passed

// ---- game function types -----------------------------------------------------------------------
using UpdateFn     = std::int64_t(__fastcall*)(void* self, float dt);
using ShootCtxFn   = void*(__fastcall*)(void* holder);
using FireFrameFn  = void*(__fastcall*)(void* unused, float* out, void* actor, void* weapon);
using LaunchSpdFn  = float(__fastcall*)(void* unused, float draw, void* weapon, void* ammo, void* stats);
struct RayHit { float dist; void* pCollider; int ipart, partid; short surface_idx, idmatOrg;
                int foreignIdx, iNode; float pt[3]; float n[3]; int bTerrain, iPrim; void* next; char pad[32]; };
using RwiFn = int(__fastcall*)(void* world, const float* org, const float* dir, int objtypes, unsigned flags,
                               RayHit* hits, int nMax, void* skip, int nSkip, void* fd, int ifd, const char* tag);

UpdateFn    o_charging = nullptr, o_aiming = nullptr, o_instant = nullptr;
ShootCtxFn  f_shootCtx = nullptr;
FireFrameFn f_fireFrame = nullptr;
LaunchSpdFn f_speed = nullptr;
RwiFn       f_rwi = nullptr;

template <class T> T At(const void* p, std::size_t off) { return *reinterpret_cast<const T*>(static_cast<const char*>(p) + off); }
template <class R, class... A> R VCall(void* obj, std::size_t slot, A... a)
{
    auto vt = *reinterpret_cast<void***>(obj);
    return reinterpret_cast<R(__fastcall*)(void*, A...)>(vt[slot])(obj, a...);
}

// ---- small vector helpers ------------------------------------------------------------------------
struct V3 { float x, y, z; };
inline V3 operator+(V3 a, V3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
inline V3 operator-(V3 a, V3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
inline V3 operator*(V3 a, float s) { return { a.x * s, a.y * s, a.z * s }; }
inline float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 Cross(V3 a, V3 b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
inline float Len(V3 a) { return std::sqrt(Dot(a, a)); }
inline V3 Norm(V3 a, V3 fb = { 1, 0, 0 }) { const float l = Len(a); return l > 1e-6f ? a * (1.0f / l) : fb; }
struct ColorB { std::uint8_t r, g, b, a; };

// ---- aux geometry (renderer vt+0x638 -> CAuxGeomCB) ----------------------------------------------
constexpr std::uint32_t kFlags = 0x40000000u /*AlphaBlended*/ | 0x00800000u /*DepthWriteOff*/;  // 3D, cull none, depth test on
void* Aux()
{
    auto* env = SSystemGlobalEnvironment::GetInstance();
    if (!env || !env->pRenderer) return nullptr;
    return VCall<void*>(env->pRenderer, 0x638 / 8);
}

constexpr int kMaxTri = 2400;          // vertices
V3     g_vtx[kMaxTri];
ColorB g_col[kMaxTri];
int    g_nv = 0;
inline void Tri(V3 a, ColorB ca, V3 b, ColorB cb, V3 c, ColorB cc)
{
    if (g_nv + 3 > kMaxTri) return;
    g_vtx[g_nv] = a; g_col[g_nv++] = ca;
    g_vtx[g_nv] = b; g_col[g_nv++] = cb;
    g_vtx[g_nv] = c; g_col[g_nv++] = cc;
}

// ---- prediction ----------------------------------------------------------------------------------
constexpr int kMaxPts = 220;
V3    g_pts[kMaxPts];
V3    g_vel[kMaxPts];
float g_s[kMaxPts];        // path length
int   g_np = 0;
bool  g_hit = false;
V3    g_hp{}, g_hn{ 0, 0, 1 };

// last prediction, for the calibration log on release / hit
float g_lastSpeed = 0, g_lastDraw = 0;
V3    g_lastPos{}, g_lastDir{}, g_lastImpact{};
bool  g_lastHitOk = false;
double g_lastLog = 0;

bool Ray(V3 a, V3 b, V3& hp, V3& hn)
{
    auto* env = SSystemGlobalEnvironment::GetInstance();
    if (!f_rwi || !env || !env->pPhysicalWorld) return false;
    const V3 d = b - a;
    RayHit h{};
    const int n = f_rwi(env->pPhysicalWorld, &a.x, &d.x, 0x11F, 0x40F, &h, 1, nullptr, 0, nullptr, 0, "TrajectoryPreview");
    if (n <= 0 || h.dist <= 0.0f) return false;
    hp = { h.pt[0], h.pt[1], h.pt[2] }; hn = { h.n[0], h.n[1], h.n[2] };
    return true;
}

void Simulate(V3 p0, V3 v0)
{
    g_np = 0; g_hit = false;
    g_pts[0] = p0; g_vel[0] = v0; g_s[0] = 0; g_np = 1;
    float t = 0, s = 0;
    const float sp = Len(v0) > 1.0f ? Len(v0) : 1.0f;
    while (g_np < kMaxPts) {
        const float ds = (0.12f + 0.025f * s) > 4.0f ? 4.0f : (0.12f + 0.025f * s);   // dense near, sparse far
        t += ds / sp;
        const V3 p = { p0.x + v0.x * t, p0.y + v0.y * t, p0.z + v0.z * t - 0.5f * g_grav * t * t };
        const V3 v = { v0.x, v0.y, v0.z - g_grav * t };
        const V3 prev = g_pts[g_np - 1];
        const float seg = Len(p - prev);
        if (s > 0.5f) {                                     // skip own bow / hands
            V3 hp{}, hn{};
            if (Ray(prev, p, hp, hn)) {
                s += Len(hp - prev);
                g_pts[g_np] = hp; g_vel[g_np] = v; g_s[g_np] = s; ++g_np;
                g_hit = true; g_hp = hp; g_hn = hn;
                return;
            }
        }
        s += seg;
        g_pts[g_np] = p; g_vel[g_np] = v; g_s[g_np] = s; ++g_np;
        if (s > 320.0f || t > 9.0f) return;
    }
}

void Render(float draw)
{
    void* aux = Aux();
    if (!aux || g_np < 2) return;
    g_nv = 0;
    const V3 up{ 0, 0, 1 };
    const bool full = draw >= 0.999f;
    // colour: still drawing = cool white, full draw = warm gold
    const std::uint8_t cr = full ? 255 : 225, cg = full ? 214 : 235, cb = full ? 120 : 255;
    auto alphaAt = [&](float s, float base) -> std::uint8_t {
        float a = base;
        if (s < 1.6f) a *= (s / 1.6f);                       // fade in from the arrow tip
        const float far = g_s[g_np - 1];
        if (!g_hit && s > far - 20.0f) a *= (far - s) / 20.0f; // fade out where nothing is hit
        if (a < 0) a = 0;
        return static_cast<std::uint8_t>(a);
    };
    auto ribbon = [&](float width, std::uint8_t r, std::uint8_t g, std::uint8_t b, float baseA) {
        for (int i = 0; i + 1 < g_np; ++i) {
            const V3 s0 = Norm(Cross(g_vel[i], up)) * (width * 0.5f);
            const V3 s1 = Norm(Cross(g_vel[i + 1], up)) * (width * 0.5f);
            const V3 a = g_pts[i] - s0, b2 = g_pts[i] + s0, c = g_pts[i + 1] - s1, d = g_pts[i + 1] + s1;
            const ColorB k0{ r, g, b, alphaAt(g_s[i], baseA) }, k1{ r, g, b, alphaAt(g_s[i + 1], baseA) };
            Tri(a, k0, b2, k0, c, k1);
            Tri(b2, k0, d, k1, c, k1);
        }
    };
    ribbon(g_width * 1.9f, 0, 0, 0, 70.0f);                // soft dark edge for contrast
    ribbon(g_width, cr, cg, cb, 170.0f);
    if (g_hit) {                                            // ring on the hit surface
        const V3 n = Norm(g_hn, up);
        const V3 u = Norm(Cross(n, std::fabs(n.z) < 0.9f ? up : V3{ 1, 0, 0 }));
        const V3 w = Cross(n, u);
        const V3 c = g_hp + n * 0.015f;
        const float ro = g_ring, ri = g_ring * 0.66f;
        const ColorB k{ cr, cg, cb, 210 };
        constexpr int seg = 28;
        for (int i = 0; i < seg; ++i) {
            const float a0 = 6.2831853f * i / seg, a1 = 6.2831853f * (i + 1) / seg;
            const V3 o0 = c + u * (std::cos(a0) * ro) + w * (std::sin(a0) * ro);
            const V3 o1 = c + u * (std::cos(a1) * ro) + w * (std::sin(a1) * ro);
            const V3 i0 = c + u * (std::cos(a0) * ri) + w * (std::sin(a0) * ri);
            const V3 i1 = c + u * (std::cos(a1) * ri) + w * (std::sin(a1) * ri);
            Tri(o0, k, o1, k, i0, k);
            Tri(o1, k, i1, k, i0, k);
        }
    }
    if (g_nv == 0) return;
    std::uint32_t oldF = 0, tmp = 0, newF = kFlags;
    VCall<void*>(aux, 0x10 / 8, &oldF);                        // GetRenderFlags(&out)
    VCall<void*>(aux, 0x08 / 8, &tmp, &newF);                  // SetRenderFlags(&old, &new)
    VCall<void>(aux, 0xA0 / 8, static_cast<const V3*>(g_vtx), static_cast<std::uint32_t>(g_nv),
                static_cast<const ColorB*>(g_col));          // DrawTriangles(v, n, colours*)
    VCall<void*>(aux, 0x08 / 8, &tmp, &oldF);                  // restore
}

double Now()
{
    LARGE_INTEGER f, c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) / static_cast<double>(f.QuadPart);
}

void Preview(void* action, float draw)
{
    if (!g_on || !g_ready) return;
    auto* fw = CCryAction::GetInstance();
    if (!fw) return;
    void* player = fw->GetClientActor();
    if (!player) return;
    void* owner = At<void*>(action, 0x60);
    if (!owner) return;
    void* actor = VCall<void*>(owner, 2);                      // [action+0x60]->vt[2]()
    if (actor != player) return;                               // only the player's own shot
    void* holder = At<void*>(actor, 0x280);
    if (!holder) return;
    void* ctx = f_shootCtx(holder);
    if (!ctx) return;
    void* weapon = At<void*>(ctx, 0x40);
    void* ammo   = At<void*>(ctx, 0x48);
    void* stats  = At<void*>(actor, 0x668);
    if (!weapon || !ammo || !stats) return;

    draw = draw < 0 ? 0 : (draw > 1 ? 1 : draw);
    float frame[9] = {};
    f_fireFrame(nullptr, frame, actor, weapon);
    const V3 pos{ frame[0], frame[1], frame[2] };
    const V3 dir = Norm(V3{ frame[6], frame[7], frame[8] }, V3{ 0, 1, 0 });
    const float speed = f_speed(nullptr, draw, weapon, ammo, stats);
    if (!(speed > 0.5f && speed < 2000.0f)) return;

    Simulate(pos, dir * speed);
    Render(draw);

    g_lastSpeed = speed; g_lastDraw = draw; g_lastPos = pos; g_lastDir = dir;
    g_lastHitOk = g_hit; g_lastImpact = g_hp;
    if (g_log && Now() - g_lastLog > 1.0) {
        g_lastLog = Now();
        TP_LOG("draw=%.3f v=%.2f pos=(%.2f,%.2f,%.2f) dir=(%.3f,%.3f,%.3f) pts=%d hit=%d (%.2f,%.2f,%.2f) path=%.1f",
               draw, speed, pos.x, pos.y, pos.z, dir.x, dir.y, dir.z, g_np, g_hit ? 1 : 0, g_hp.x, g_hp.y, g_hp.z,
               g_s[g_np - 1]);
    }
}

bool g_faulted = false;
void SafePreview(void* action, float draw)
{
    if (g_faulted) return;
    __try { Preview(action, draw); }
    __except (EXCEPTION_EXECUTE_HANDLER) { g_faulted = true; TP_LOG("preview faulted -- disabled for this session"); }
}

std::int64_t __fastcall hkCharging(void* self, float dt)
{
    const std::int64_t r = o_charging(self, dt);
    if (r != 0) return r;                                      // action finishing / switching: do not touch it
    float p = 0.0f;
    __try { p = At<float>(self, 0xA8); } __except (EXCEPTION_EXECUTE_HANDLER) { return r; }
    SafePreview(self, p);
    return r;
}
std::int64_t __fastcall hkAiming(void* self, float dt)
{
    const std::int64_t r = o_aiming(self, dt);
    if (r != 0) return r;
    SafePreview(self, 1.0f);
    return r;
}
std::int64_t __fastcall hkInstant(void* self, float dt)
{
    const std::int64_t r = o_instant(self, dt);
    if (r != 0) return r;
    SafePreview(self, 1.0f);
    return r;
}

// ---- calibration: actual impacts vs. last prediction ----------------------------------------------
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
                const V3 hv{ h.x, h.y, h.z };
                TP_LOG("hit (%.2f,%.2f,%.2f) lastPred draw=%.3f v=%.2f predImpact=%d (%.2f,%.2f,%.2f) err=%.2f",
                       h.x, h.y, h.z, g_lastDraw, g_lastSpeed, g_lastHitOk ? 1 : 0,
                       g_lastImpact.x, g_lastImpact.y, g_lastImpact.z, g_lastHitOk ? Len(hv - g_lastImpact) : -1.0f);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        return ret;
    }
    static inline REL::Relocation<decltype(&thunk)> orig;
} hkDamage;

bool HookAt(std::size_t rva, void* detour, void** orig)
{
    void* t = reinterpret_cast<void*>(REL::Offset(rva).address());
    return MH_CreateHook(t, detour, orig) == MH_OK && MH_EnableHook(t) == MH_OK;
}

bool g_started = false;

}  // namespace

KCSE_PLUGIN_INFO("TrajectoryProbe", "zz_levelcap modpack", 4);
KCSE_PLUGIN_LOAD(kcse)
{
    const std::uint32_t stamp = REL::Module::get().timestamp();
    if (stamp != kGameStamp) {
        TP_LOG("v4: WHGame build %08X is not 1.5.6 (%08X) -- staying off, nothing hooked", stamp, kGameStamp);
        return true;
    }
    if (MH_Initialize() != MH_OK && MH_Initialize() != MH_ERROR_ALREADY_INITIALIZED) return false;
    f_shootCtx  = reinterpret_cast<ShootCtxFn>(REL::Offset(kShootCtx).address());
    f_fireFrame = reinterpret_cast<FireFrameFn>(REL::Offset(kGetFireFrame).address());
    f_speed     = reinterpret_cast<LaunchSpdFn>(REL::Offset(kLaunchSpeed).address());
    f_rwi       = reinterpret_cast<RwiFn>(REL::Offset(kRwiHelper).address());
    const bool h1 = HookAt(kChargingUpdate, reinterpret_cast<void*>(&hkCharging), reinterpret_cast<void**>(&o_charging));
    const bool h2 = HookAt(kAimingUpdate, reinterpret_cast<void*>(&hkAiming), reinterpret_cast<void**>(&o_aiming));
    const bool h3 = HookAt(kInstantUpdate, reinterpret_cast<void*>(&hkInstant), reinterpret_cast<void**>(&o_instant));
    const bool h4 = hkDamage.Install();
    g_ready = h1 && h2 && h3;
    kcse->GetMessagingInterface()->RegisterListener([](KCSE::Message* msg) {
        auto* env = SSystemGlobalEnvironment::GetInstance();
        if (!env || msg->type != KCSE::IMessagingInterface::kMessage_DataLoaded || g_started) return;
        g_started = true;
        if (env->pConsole) {
            env->pConsole->RegisterCVarInt("tp_on", &g_on, g_on, VF_NULL, "Trajectory preview on/off");
            env->pConsole->RegisterCVarInt("tp_log", &g_log, g_log, VF_NULL, "Trajectory preview: calibration log");
            env->pConsole->RegisterCVarFloat("tp_width", &g_width, g_width, VF_NULL, "Trajectory preview: ribbon width (m)");
            env->pConsole->RegisterCVarFloat("tp_g", &g_grav, g_grav, VF_NULL, "Trajectory preview: gravity");
            env->pConsole->RegisterCVarFloat("tp_ring", &g_ring, g_ring, VF_NULL, "Trajectory preview: impact ring radius (m)");
        }
    });
    TP_LOG("v4 loaded: hooks charging=%d aiming=%d instant=%d hit=%d -> %s", h1, h2, h3, h4, g_ready ? "ready" : "OFF");
    return true;
}
