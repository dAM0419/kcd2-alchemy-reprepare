// TrajectoryProbe -- measurement build for a ranged-weapon trajectory preview (KCD2 1.5.6).
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
#include "Offsets/vtables/IInputEventListener.h"

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

// ------------------------------------------------------------------ input -------------
struct KeyState { bool down = false; double since = 0.0; };
KeyState g_mouse1, g_mouse2, g_trigR, g_trigL;
double   g_lastRelease = -100.0;
const char* g_lastReleaseKey = "";
double   g_lastHeld = 0.0;

KeyState* KeyFor(const char* name)
{
    if (!name) return nullptr;
    if (!_stricmp(name, "mouse1")) return &g_mouse1;
    if (!_stricmp(name, "mouse2")) return &g_mouse2;
    if (!_stricmp(name, "xi_triggerr")) return &g_trigR;
    if (!_stricmp(name, "xi_triggerl")) return &g_trigL;
    return nullptr;
}

struct InputListener final : Offsets::IInputEventListener {
    bool OnInputEvent(const Offsets::SInputEvent& e) override
    {
        KeyState* k = KeyFor(e.keyName);
        if (!k) return false;
        const int st = static_cast<int>(e.state);
        const double t = Now();
        if (st == 1 && !k->down) {            // pressed
            k->down = true; k->since = t;
            if (g_log) TP_LOG("press %s t=%.3f", e.keyName, t);
        } else if (st == 2 && k->down) {      // released
            k->down = false;
            const double held = t - k->since;
            Vec3 p{}, d{};
            ViewCamera(p, d);
            if (g_log) TP_LOG("release %s t=%.3f held=%.3f cam=(%.3f,%.3f,%.3f) dir=(%.4f,%.4f,%.4f)",
                              e.keyName, t, held, p.x, p.y, p.z, d.x, d.y, d.z);
            g_lastRelease = t; g_lastReleaseKey = e.keyName; g_lastHeld = held;
        }
        return false;   // never consume
    }
    bool OnInputEventUI(const void*) override { return false; }
    int  GetPriority() const override { return 0; }
    bool _vf3(const void*) override { return false; }
} g_input;

// ------------------------------------------------------------------ projectile tracks ---
struct Track { uint32_t id; double t0; Vec3 last; double tLast; int n; bool fast; };
std::vector<Track> g_tracks;
std::unordered_set<uint32_t> g_nearBefore;   // entities near the camera just before release

int Proximity(const Vec3& c, float r, Offsets::IEntity** out, int cap)
{
    auto* env = SSystemGlobalEnvironment::GetInstance();
    if (!env || !env->pEntitySystem) return 0;
    struct ProximityQuery {           // CE3 SEntityProximityQuery layout (KCD1-proven slot [25])
        AABB box; void* pEntityClass; uint32_t nEntityFlags; void** pEntities; int nCount;
    } q{};
    q.box = AABB(Vec3(c.x - r, c.y - r, c.z - r), Vec3(c.x + r, c.y + r, c.z + r));
    int n = 0;
    __try {
        n = env->pEntitySystem->QueryProximity(&q);
        if (n > cap) n = cap;
        for (int i = 0; i < n; ++i) out[i] = reinterpret_cast<Offsets::IEntity*>(q.pEntities[i]);
    } __except (EXCEPTION_EXECUTE_HANDLER) { n = -1; }
    return n;
}

bool EntityPos(uint32_t id, Vec3& pos, const char** name)
{
    auto* env = SSystemGlobalEnvironment::GetInstance();
    if (!env || !env->pEntitySystem) return false;
    __try {
        auto* e = env->pEntitySystem->GetEntity(id);
        if (!e) return false;
        e->GetWorldPos(pos);
        if (name) *name = e->GetName();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void ScanNear(bool baseline)
{
    Vec3 c{}, d{};
    if (!ViewCamera(c, d)) return;
    Offsets::IEntity* ents[256];
    const int n = Proximity(c, 4.0f, ents, 256);
    if (n < 0) { static bool once = false; if (!once) { once = true; TP_LOG("QueryProximity faulted -- scan disabled"); } return; }
    for (int i = 0; i < n; ++i) {
        uint32_t id = 0;
        __try { id = ents[i]->GetId(); } __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
        if (baseline) { g_nearBefore.insert(id); continue; }
        if (g_nearBefore.count(id)) continue;
        bool known = false;
        for (auto& t : g_tracks) if (t.id == id) { known = true; break; }
        if (known) continue;
        Vec3 p{}; const char* nm = "";
        if (!EntityPos(id, p, &nm)) continue;
        g_tracks.push_back(Track{ id, Now(), p, Now(), 0, false });
        if (g_log) TP_LOG("new id=%u name=%s t=%.3f pos=(%.3f,%.3f,%.3f) dtRelease=%.3f held=%.3f key=%s",
                          id, nm ? nm : "?", Now(), p.x, p.y, p.z, Now() - g_lastRelease, g_lastHeld, g_lastReleaseKey);
    }
}

void UpdateTracks()
{
    const double t = Now();
    for (size_t i = 0; i < g_tracks.size();) {
        auto& tr = g_tracks[i];
        Vec3 p{};
        const bool alive = EntityPos(tr.id, p, nullptr);
        const double dt = t - tr.tLast;
        if (alive && dt > 0.0) {
            const float dx = p.x - tr.last.x, dy = p.y - tr.last.y, dz = p.z - tr.last.z;
            const float sp = std::sqrt(dx * dx + dy * dy + dz * dz) / static_cast<float>(dt);
            if (sp > 8.0f) tr.fast = true;
            if (tr.fast && g_log)
                TP_LOG("trk id=%u t=%.4f pos=(%.3f,%.3f,%.3f) v=%.2f", tr.id, t, p.x, p.y, p.z, sp);
            tr.last = p; tr.tLast = t; ++tr.n;
        }
        const bool done = !alive || (t - tr.t0) > 4.0 || (!tr.fast && (t - tr.t0) > 0.5);
        if (done) {
            if (tr.fast && g_log) TP_LOG("end id=%u alive=%d samples=%d", tr.id, alive ? 1 : 0, tr.n);
            g_tracks.erase(g_tracks.begin() + i);
        } else ++i;
    }
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
    const KeyState& k = g_mouse1.down ? g_mouse1 : g_trigR;
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
                TP_LOG("hit t=%.3f pos=(%.3f,%.3f,%.3f) missile=%llu dtRelease=%.3f held=%.3f",
                       Now(), h.x, h.y, h.z, static_cast<unsigned long long>(ctx->m_missileWuid.m_value),
                       Now() - g_lastRelease, g_lastHeld);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        return ret;
    }
    static inline REL::Relocation<decltype(&thunk)> orig;
} hkDamage;

// ------------------------------------------------------------------ tick ---------------
bool g_baselined = false;

void Tick()
{
    const double t = Now();
    const bool holding = g_mouse1.down || g_trigR.down;
    if (holding && !g_baselined) { g_nearBefore.clear(); ScanNear(true); g_baselined = true; }
    if (!holding) g_baselined = false;
    if (t - g_lastRelease < 0.35) ScanNear(false);
    UpdateTracks();
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
        if (env->pInput) env->pInput->AddEventListener(&g_input);
        KCSE::GetTaskInterface()->AddTask(&Tick);
        TP_LOG("started");
    });
    TP_LOG("loaded (hit hook %s)", hooked ? "ok" : "FAILED");
    return true;
}
