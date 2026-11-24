// AlchemyReprepare -- KCSE plugin for KCD2 1.5.6
// Remembers the last recipe opened in the alchemy book and adds a table action
// ("alch_reprepare", keyboard G / pad Y by default) that lays that recipe's herbs out on the
// ingredient stations again -- the same thing the book's Prepare does -- without walking back
// to the book.  Brewing itself stays fully manual (XP, quality, perks untouched).
//
// Based on the public reverse-engineering in libKCD2 (Projects/Autobrew, GPL-3.0, JerryYOJ):
// session hooks, open-recipe resolution and the station stocking routine are adapted from it.
#include <MinHook.h>
#include <cstring>
#include <vector>

#include "KCSE/KCSEAPI.h"
#include "crysystem/SSystemGlobalEnvironment.h"
#include "crysystem/CCryAction.h"
#include "entitymodule/C_Actor.h"
#include "entitymodule/C_DocumentItem.h"
#include "entitymodule/C_Inventory.h"
#include "entitymodule/C_ItemDatabase.h"
#include "entitymodule/S_HerbItemClass.h"
#include "game/S_GameContext.h"
#include "playermodule/C_PlayerModule.h"
#include "entitymodule/C_Item.h"
#include "entitymodule/E_ItemType.h"
#include "entitymodule/S_ItemClass.h"
#include "playermodule/I_ActionSets.h"
#include "playermodule/C_Alchemy.h"
#include "playermodule/C_AlchemyRecipeDatabase.h"

using namespace wh::playermodule;

namespace {

C_Alchemy* g_session = nullptr;
uint32_t   g_lastRecipeId = 0;     // survives table sessions for the whole game run
bool       g_pressed = false;
bool       g_registered = false;
int        g_enable = 1;

constexpr const char* kCtx = "alchemy";
constexpr const char* kAction = "alch_reprepare";

I_ActionSets* ActionSets()
{
    auto* ctx = wh::game::S_GameContext::GetInstance();
    auto* pm = ctx ? ctx->m_pPlayerModule : nullptr;
    return pm ? pm->m_pActionSets : nullptr;
}

uint32_t OpenRecipeId(C_Alchemy* alc)
{
    const EntityId bookId = EntityId(alc->m_brewState.m_kindEntityId[E_AlchemyVerb::OpenBook]);
    if (!bookId) return 0;
    auto* fw = CCryAction::GetInstance();
    auto* items = fw ? fw->GetIItemSystem() : nullptr;
    auto* book = reinterpret_cast<wh::entitymodule::C_DocumentItem*>(items ? items->GetItem(bookId) : nullptr);
    if (!book || book->m_bookKind != wh::entitymodule::E_DocumentBookKind::AlchemyBook ||
        book->m_useGuidPageNavigation)
        return 0;
    return book->m_currentNumericPageId;
}

CryGUID DriedVariant(const CryGUID& cls)
{
    using namespace wh::entitymodule;
    auto* def = C_ItemDatabase::GetInstance()->FindClassByGuid(cls);
    if (!def || !def->IsType(E_ItemType::Herb)) return CryGUID{};
    return def->GetAsHerbItemClass()->m_driedItemId;
}

// Same as the book's Prepare: clear the stations, then place one stack per recipe row
// (fresh herb preferred, dried variant as fallback).  Nothing is consumed here.
bool Stock(C_Alchemy* alc, C_AlchemyRecipe* recipe)
{
    auto* actor = alc->m_pPlayerActor;
    auto* inv = actor ? actor->GetInventory() : nullptr;
    if (!inv) return false;
    std::vector<wh::framework::WUID> stock;
    for (auto& row : recipe->m_ingredients) {
        auto* item = inv->FindItemByClass(row.m_ingredientItemId);
        if (!item) {
            const CryGUID dried = DriedVariant(row.m_ingredientItemId);
            if (dried != CryGUID{}) item = inv->FindItemByClass(dried);
        }
        if (!item) return false;
        stock.push_back(item->m_wuid);
    }
    alc->ClearIngredientSlots();
    for (auto& w : stock) alc->ApplyIngredient(w);
    return true;
}

void Register()
{
    if (g_registered) return;
    auto* as = ActionSets();
    if (!as) return;
    as->RegisterAction(kCtx, kAction, [] {
        g_pressed = true;
        if (auto* env = SSystemGlobalEnvironment::GetInstance(); env && env->pLog)
            env->pLog->LogAlways("[AlchemyReprepare] action callback fired");
    }, 1);
    as->SetActionVisible(kCtx, kAction, 0, 1);
    as->SetActionEnabled(kCtx, kAction, false, 1);
    g_registered = true;
}

void Unregister()
{
    if (!g_registered) return;
    if (auto* as = ActionSets()) as->UnregisterAction(kCtx, kAction, 1);
    g_registered = false;
}

void Tick()
{
    if (auto* alc = g_session) {
        Register();
        auto& bs = alc->m_brewState;
        const char* context = alc->m_currentContext.c_str();
        const bool inReading = std::strcmp(context, "alchemy_reading") == 0;
        const bool atTable   = std::strcmp(context, kCtx) == 0;

        if (inReading && bs.m_mode == E_AlchemyMode::Reading)
            if (uint32_t id = OpenRecipeId(alc)) g_lastRecipeId = id;

        const bool idle = atTable && bs.m_mode == E_AlchemyMode::Brewing &&
                          alc->GetEffectiveState() == E_AlchemyInteractionState::Idle &&
                          !alc->m_director.IsSlotOccupied(E_AlchemyDirectorSlot::Verb);
        auto* recipe = g_lastRecipeId ? C_AlchemyRecipeDatabase::GetRecipeById(g_lastRecipeId) : nullptr;
        auto* actor = alc->m_pPlayerActor;
        auto* inv = actor ? actor->GetInventory() : nullptr;
        const bool visible = g_enable && recipe != nullptr;
        const bool lit = visible && idle && inv && recipe->CanBrewFrom(inv);

        if (auto* as = ActionSets()) {
            as->SetActionVisible(kCtx, kAction, visible, 1);
            as->SetActionEnabled(kCtx, kAction, lit, 1);
            as->SetActionDisableReason(kCtx, kAction,
                (visible && !lit && idle) ? "dlg_alch_not_all_ingredients" : "", 1);
        }

        // ---- diagnostics: log every state change ----
        {
            static int last = -1;
            const int st = (int)visible | ((int)lit << 1) | ((int)idle << 2) | ((int)atTable << 3) |
                           ((int)inReading << 4) | ((int)(bs.m_mode == E_AlchemyMode::Brewing) << 5) |
                           ((int)(alc->GetEffectiveState() == E_AlchemyInteractionState::Idle) << 6) |
                           ((int)alc->m_director.IsSlotOccupied(E_AlchemyDirectorSlot::Verb) << 7) |
                           ((int)(inv && recipe && recipe->CanBrewFrom(inv)) << 8);
            if (st != last) {
                last = st;
                if (auto* env = SSystemGlobalEnvironment::GetInstance(); env && env->pLog)
                    env->pLog->LogAlways("[AlchemyReprepare] ctx=%s mode=%d eff=%d verbBusy=%d recipe=%u canBrew=%d visible=%d lit=%d",
                        context, (int)bs.m_mode, (int)alc->GetEffectiveState(),
                        (int)alc->m_director.IsSlotOccupied(E_AlchemyDirectorSlot::Verb),
                        g_lastRecipeId, (int)(inv && recipe && recipe->CanBrewFrom(inv)), (int)visible, (int)lit);
            }
        }
        const bool pressed = g_pressed;
        g_pressed = false;
        if (pressed && !lit)
            if (auto* env = SSystemGlobalEnvironment::GetInstance(); env && env->pLog)
                env->pLog->LogAlways("[AlchemyReprepare] pressed but not lit");
        if (pressed && lit) {
            const bool ok = Stock(alc, recipe);
            if (auto* env = SSystemGlobalEnvironment::GetInstance(); env && env->pLog)
                env->pLog->LogAlways("[AlchemyReprepare] recipe %u restock %s", g_lastRecipeId, ok ? "ok" : "failed");
        }
    } else {
        g_pressed = false;
    }
    KCSE::GetTaskInterface()->AddTask(&Tick);
}

// ---- session hooks (C_Alchemy::Start / End; Address Library ids from libKCD2) ----
using StartFn = char (*)(C_Alchemy*, uint32_t);
using EndFn = uint64_t (*)(C_Alchemy*);
StartFn g_origStart = nullptr;
EndFn g_origEnd = nullptr;

char HookStart(C_Alchemy* self, uint32_t table)
{
    const char ok = g_origStart(self, table);
    if (ok) g_session = self;
    return ok;
}

uint64_t HookEnd(C_Alchemy* self)
{
    if (g_session == self) { Unregister(); g_session = nullptr; }
    return g_origEnd(self);
}

bool Hook(uint64_t id, void* detour, void** orig)
{
    void* target = reinterpret_cast<void*>(REL::ID(id).address());
    return MH_CreateHook(target, detour, orig) == MH_OK && MH_EnableHook(target) == MH_OK;
}

}  // namespace

KCSE_PLUGIN_INFO("AlchemyReprepare", "zz_levelcap modpack", 1);
KCSE_PLUGIN_LOAD(kcse)
{
    if (MH_Initialize() != MH_OK && MH_Initialize() != MH_ERROR_ALREADY_INITIALIZED)
        return false;
    if (!Hook(47131, reinterpret_cast<void*>(&HookStart), reinterpret_cast<void**>(&g_origStart)) ||
        !Hook(54603, reinterpret_cast<void*>(&HookEnd), reinterpret_cast<void**>(&g_origEnd)))
        return false;
    KCSE::GetTaskInterface()->AddTask(&Tick);
    kcse->GetMessagingInterface()->RegisterListener([](KCSE::Message* msg) {
        if (msg->type != KCSE::IMessagingInterface::kMessage_DataLoaded) return;
        SSystemGlobalEnvironment::GetInstance()->pConsole->RegisterCVarInt(
            "kcse_reprepare_enable", &g_enable, g_enable, VF_NULL,
            "1 = show the 'Prepare last recipe' action at the alchemy table.");
    });
    return true;
}
