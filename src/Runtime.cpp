#include <AbstractTypeClass.h>
#include <CCINIClass.h>
#include <RulesClass.h>
#include <SessionClass.h>
#include <Memory.h>

#include <windows.h>

#include <cstdio>
#include <cstdarg>
#include <cstring>

#include "Config.h"
#include "IniOverlay.h"
#include "Logger.h"
#include "Runtime.h"
#include "RuntimeProtocol.h"
#include "RuntimeWatcher.h"

namespace Runtime {
namespace {

    constexpr int kQueueCapacity = 32;
    constexpr int kMaxStateEntries = kMaxItems;
    constexpr int kMaxTypeSections = 256;

    enum GlobalRoute : unsigned int {
        RouteNone = 0,
        RouteGeneral = 1u << 0,
        RouteCombatDamage = 1u << 1,
        RouteAudioVisual = 1u << 2,
        RouteCrateRules = 1u << 3,
        RouteRadiation = 1u << 4,
        RouteElevation = 1u << 5,
        RouteWall = 1u << 6,
        RouteSpecialWeapons = 1u << 7,
        RoutePowerups = 1u << 8,
        RouteLand = 1u << 9,
        RouteIQ = 1u << 10,
        RouteJumpjet = 1u << 11,
        RouteCommandBar = 1u << 12,
        RouteMultiplayerDialog = 1u << 13,
        RouteDifficulties = 1u << 14,
        RouteAI = 1u << 15
    };

    struct QueuedCommand {
        Command command = Command::Reload;
        bool argument = false;
    };

    struct StateEntry {
        char section[64] = {};
        char key[96] = {};
        char value[512] = {};
    };

    struct State {
        StateEntry entries[kMaxStateEntries] = {};
        int entryCount = 0;
        char typeSections[kMaxTypeSections][64] = {};
        int typeCount = 0;
        unsigned int globalRoutes = RouteNone;
    };

    CRITICAL_SECTION s_lock;
    bool s_lockInitialized = false;
    bool s_initialized = false;
    bool s_wasAllowed = false;

    QueuedCommand s_queue[kQueueCapacity] = {};
    int s_queueHead = 0;
    int s_queueTail = 0;
    int s_queueCount = 0;

    Snapshot s_snapshot;
    Item s_items[kMaxItems] = {};
    int s_itemCount = 0;
    Item s_candidateItems[kMaxItems] = {};
    int s_candidateItemCount = 0;

    State s_current;
    State s_candidate;
    State s_previous;
    State s_baselineTypes;
    CCINIClass* s_typeBaseline = nullptr;

    bool Equals(const char* left, const char* right)
    {
        return left && right && _stricmp(left, right) == 0;
    }

    bool StartsWith(const char* text, const char* prefix)
    {
        if (!text || !prefix) return false;
        return _strnicmp(text, prefix, std::strlen(prefix)) == 0;
    }

    void SetMessage(const char* format, ...)
    {
        EnterCriticalSection(&s_lock);
        va_list args;
        va_start(args, format);
        std::vsnprintf(s_snapshot.lastMessage, sizeof(s_snapshot.lastMessage),
                       format, args);
        va_end(args);
        LeaveCriticalSection(&s_lock);
    }

    void PublishItems()
    {
        EnterCriticalSection(&s_lock);
        s_itemCount = s_candidateItemCount;
        if (s_itemCount > 0) {
            std::memcpy(s_items, s_candidateItems,
                        sizeof(Item) * static_cast<size_t>(s_itemCount));
        }
        LeaveCriticalSection(&s_lock);
    }

    void EnsureDirectoryTree(const char* path)
    {
        if (!path || !path[0]) return;
        char partial[IniOverlay::kPathMax] = {};
        std::snprintf(partial, sizeof(partial), "%s", path);
        for (char* p = partial; *p; ++p) {
            if ((*p == '\\' || *p == '/') && p != partial && p[-1] != ':') {
                const char saved = *p;
                *p = '\0';
                CreateDirectoryA(partial, nullptr);
                *p = saved;
            }
        }
        CreateDirectoryA(partial, nullptr);
    }

    const char* ModeName()
    {
        const auto& session = SessionClass::Instance;
        if (session.Play || session.Attract) return "Replay";
        if (!session.CurrentlyInGame) return "Menu";
        if (SessionClass::IsCampaign()) return "Campaign";
        if (SessionClass::IsSkirmish()) return "Skirmish";
        if (session.GameMode == GameMode::LAN) return "LAN";
        if (session.GameMode == GameMode::Internet) return "Internet";
        return "Other";
    }

    bool IsReplay()
    {
        const auto& session = SessionClass::Instance;
        return session.Play || session.Record || session.Attract;
    }

    bool IsAllowedSession()
    {
        const auto& session = SessionClass::Instance;
        return session.CurrentlyInGame &&
               (SessionClass::IsCampaign() || SessionClass::IsSkirmish()) &&
               !IsReplay();
    }

    void UpdateSessionSnapshot()
    {
        const auto& session = SessionClass::Instance;
        EnterCriticalSection(&s_lock);
        s_snapshot.currentlyInGame = session.CurrentlyInGame;
        s_snapshot.singlePlayer = SessionClass::IsSingleplayer();
        s_snapshot.replay = IsReplay();
        std::snprintf(s_snapshot.mode, sizeof(s_snapshot.mode), "%s", ModeName());
        s_snapshot.pendingCommands = s_queueCount;
        LeaveCriticalSection(&s_lock);
    }

    INIClass::INISection* FindSection(INIClass* ini, const char* name)
    {
        if (!ini || !name) return nullptr;
        for (auto* section = ini->Sections.First();
             section && section->IsValid(); section = section->Next()) {
            if (section->Name && Equals(section->Name, name)) return section;
        }
        return nullptr;
    }

    const char* FindValue(INIClass* ini, const char* sectionName,
                          const char* key)
    {
        auto* section = FindSection(ini, sectionName);
        if (!section) return "";
        for (auto* node = section->Entries.GenericList::First();
             node && node->IsValid(); node = node->Next()) {
            auto* entry = static_cast<INIClass::INIEntry*>(node);
            if (entry->Key && Equals(entry->Key, key))
                return entry->Value ? entry->Value : "";
        }
        return "";
    }

    AbstractTypeClass* FindType(const char* id)
    {
        if (!id || !id[0]) return nullptr;
        for (auto* type : AbstractTypeClass::Array) {
            if (type && Equals(type->ID, id)) return type;
        }
        return nullptr;
    }

    bool IsListOrRegistrySection(const char* section)
    {
        static const char* const sections[] = {
            "Maximums", "InfantryTypes", "VehicleTypes", "AircraftTypes",
            "BuildingTypes", "TerrainTypes", "SmudgeTypes", "OverlayTypes",
            "Animations", "VoxelAnims", "Warheads", "Particles",
            "ParticleSystems", "WeaponTypes", "Projectiles", "Projectile",
            "SuperWeaponTypes", "Countries", "Sides",
            "AITriggerTypes", "TeamTypes", "TaskForces", "ScriptTypes",
            "TriggerTypes", "Tags", "Colors", "ColorAdd"
        };
        for (const char* value : sections) {
            if (Equals(section, value)) return true;
        }
        return false;
    }

    unsigned int RouteForSection(const char* section)
    {
        if (Equals(section, "General")) return RouteGeneral;
        if (Equals(section, "CombatDamage")) return RouteCombatDamage;
        if (Equals(section, "AudioVisual")) return RouteAudioVisual;
        if (Equals(section, "CrateRules")) return RouteCrateRules;
        if (Equals(section, "Radiation")) return RouteRadiation;
        if (Equals(section, "ElevationModel")) return RouteElevation;
        if (Equals(section, "WallModel")) return RouteWall;
        if (Equals(section, "SpecialWeapons")) return RouteSpecialWeapons;
        if (Equals(section, "Powerups")) return RoutePowerups;
        if (Equals(section, "LandCharacteristics")) return RouteLand;
        if (Equals(section, "IQ")) return RouteIQ;
        if (Equals(section, "JumpjetControls")) return RouteJumpjet;
        if (Equals(section, "AdvancedCommandBar")) return RouteCommandBar;
        if (Equals(section, "MultiplayerDialogSettings")) return RouteMultiplayerDialog;
        if (Equals(section, "Easy") || Equals(section, "Normal") ||
            Equals(section, "Difficult")) return RouteDifficulties;
        if (Equals(section, "AI")) return RouteAI;
        return RouteNone;
    }

    bool IsWholeTypeRestartRequired(AbstractType type)
    {
        switch (type) {
        case AbstractType::AnimType:
        case AbstractType::IsotileType:
        case AbstractType::OverlayType:
        case AbstractType::SmudgeType:
        case AbstractType::TerrainType:
        case AbstractType::VoxelAnimType:
        case AbstractType::AITriggerType:
        case AbstractType::ScriptType:
        case AbstractType::TeamType:
        case AbstractType::TaskForce:
        case AbstractType::TriggerType:
        case AbstractType::TagType:
            return true;
        default:
            return false;
        }
    }

    bool IsResourceOrLayoutKey(const char* key)
    {
        static const char* const exact[] = {
            "Image", "AlphaImage", "Cameo", "AltCameo", "Voxel",
            "Foundation", "Locomotor", "MovementZone", "SpeedType",
            "Theater", "NewTheater", "AlternateArcticArt", "Palette",
            "CustomPalette", "SHP", "VXL", "HVA", "TurretAnim",
            "BarrelAnim", "PBarrelLength", "PBarrelThickness"
        };
        for (const char* value : exact) {
            if (Equals(key, value)) return true;
        }
        return StartsWith(key, "Foundation.") || StartsWith(key, "Image.") ||
               StartsWith(key, "Voxel.") || StartsWith(key, "TurretAnim") ||
               StartsWith(key, "BarrelAnim");
    }

    Safety Classify(const char* section, const char* key,
                    AbstractTypeClass** outputType, unsigned int* outputRoute,
                    const char** reason)
    {
        *outputType = nullptr;
        *outputRoute = RouteForSection(section);
        *reason = "native rules reload";
        if (*outputRoute != RouteNone) return Safety::ControlledReload;

        if (IsListOrRegistrySection(section)) {
            *reason = "type/list registry requires restart";
            return Safety::RestartRequired;
        }

        AbstractTypeClass* type = FindType(section);
        *outputType = type;
        if (!type) {
            *reason = "unsupported or extension-only section";
            return Safety::RestartRequired;
        }

        const AbstractType kind = type->WhatAmI();
        if (IsWholeTypeRestartRequired(kind)) {
            *reason = "resource or structural type requires restart";
            return Safety::RestartRequired;
        }
        if (IsResourceOrLayoutKey(key)) {
            *reason = "resource/layout field requires restart";
            return Safety::RestartRequired;
        }

        switch (kind) {
        case AbstractType::WeaponType:
        case AbstractType::WarheadType:
        case AbstractType::BulletType:
        case AbstractType::ParticleType:
        case AbstractType::ParticleSystemType:
            *reason = "live type field";
            return Safety::Immediate;
        case AbstractType::AircraftType:
        case AbstractType::BuildingType:
        case AbstractType::InfantryType:
        case AbstractType::UnitType:
        case AbstractType::HouseType:
            *reason = "type field; existing objects may retain copied values";
            return Safety::FutureObjects;
        default:
            *reason = "native LoadFromINI; extension fields are best-effort";
            return Safety::ControlledReload;
        }
    }

    bool AddTypeSection(State& state, const char* section)
    {
        for (int i = 0; i < state.typeCount; ++i) {
            if (Equals(state.typeSections[i], section)) return true;
        }
        if (state.typeCount >= kMaxTypeSections) return false;
        std::snprintf(state.typeSections[state.typeCount++],
                      sizeof(state.typeSections[0]), "%s", section);
        return true;
    }

    bool AddStateEntry(State& state, const char* section,
                       const char* key, const char* value)
    {
        if (state.entryCount >= kMaxStateEntries ||
            std::strlen(section) >= sizeof(state.entries[0].section) ||
            std::strlen(key) >= sizeof(state.entries[0].key) ||
            std::strlen(value) >= sizeof(state.entries[0].value)) {
            return false;
        }
        auto& entry = state.entries[state.entryCount++];
        std::snprintf(entry.section, sizeof(entry.section), "%s", section);
        std::snprintf(entry.key, sizeof(entry.key), "%s", key);
        std::snprintf(entry.value, sizeof(entry.value), "%s", value);
        return true;
    }

    bool ApplyGlobalRoutes(CCINIClass* ini, unsigned int routes)
    {
        if (routes == RouteNone) return true;
        RulesClass* rules = RulesClass::Instance;
        if (!rules || !ini) return false;
        bool ok = true;
        if (routes & RouteGeneral) ok = rules->Read_General(ini) && ok;
        if (routes & RouteCombatDamage) ok = rules->Read_CombatDamage(ini) && ok;
        if (routes & RouteAudioVisual) ok = rules->Read_AudioVisual(ini) && ok;
        if (routes & RouteCrateRules) ok = rules->Read_CrateRules(ini) && ok;
        if (routes & RouteRadiation) ok = rules->Read_Radiation(ini) && ok;
        if (routes & RouteElevation) ok = rules->Read_ElevationModel(ini) && ok;
        if (routes & RouteWall) ok = rules->Read_WallModel(ini) && ok;
        if (routes & RouteSpecialWeapons) ok = rules->Read_SpecialWeapons(ini) && ok;
        if (routes & RoutePowerups) ok = rules->Read_Powerups(ini) && ok;
        if (routes & RouteLand) ok = rules->Read_LandCharacteristics(ini) && ok;
        if (routes & RouteIQ) ok = rules->Read_IQ(ini) && ok;
        if (routes & RouteJumpjet) ok = rules->Read_JumpjetControls(ini) && ok;
        if (routes & RouteCommandBar) ok = rules->Read_AdvancedCommandBar(ini) && ok;
        if (routes & RouteMultiplayerDialog)
            ok = rules->Read_MultiplayerDialogSettings(ini) && ok;
        if (routes & RouteDifficulties) ok = rules->Read_Difficulties(ini) && ok;
        if (routes & RouteAI) ok = rules->Read_AI(ini) && ok;
        return ok;
    }

    bool ApplyTypes(CCINIClass* ini, const State& state)
    {
        bool ok = true;
        for (int i = 0; i < state.typeCount; ++i) {
            AbstractTypeClass* type = FindType(state.typeSections[i]);
            if (!type || !type->LoadFromINI(ini)) ok = false;
        }
        return ok;
    }

    bool CaptureTypeBaselines(const State& state)
    {
        if (state.typeCount == 0) return true;
        if (!s_typeBaseline) s_typeBaseline = GameCreate<CCINIClass>();
        if (!s_typeBaseline) return false;

        for (int i = 0; i < state.typeCount; ++i) {
            bool alreadyCaptured = false;
            for (int j = 0; j < s_baselineTypes.typeCount; ++j) {
                if (Equals(state.typeSections[i], s_baselineTypes.typeSections[j])) {
                    alreadyCaptured = true;
                    break;
                }
            }
            if (alreadyCaptured) continue;

            AbstractTypeClass* type = FindType(state.typeSections[i]);
            if (!type || !type->SaveToINI(s_typeBaseline) ||
                !AddTypeSection(s_baselineTypes, state.typeSections[i])) {
                return false;
            }
        }
        return true;
    }

    void ResetTypeBaselines()
    {
        if (s_typeBaseline) s_typeBaseline->Reset();
        std::memset(&s_baselineTypes, 0, sizeof(s_baselineTypes));
    }

    bool HasState(const State& state)
    {
        return state.entryCount > 0 || state.globalRoutes != RouteNone ||
               state.typeCount > 0;
    }

    bool ApplyState(const State& state)
    {
        CCINIClass* baseline = CCINIClass::INI_Rules;
        if (!baseline) return false;
        CCINIClass staging;
        IniOverlay::Copy(&staging, baseline);
        if (s_typeBaseline) IniOverlay::Copy(&staging, s_typeBaseline);
        for (int i = 0; i < state.entryCount; ++i) {
            const auto& entry = state.entries[i];
            staging.WriteString(entry.section, entry.key, entry.value);
        }
        const bool globals = ApplyGlobalRoutes(&staging, state.globalRoutes);
        const bool types = ApplyTypes(&staging, state);
        return globals && types;
    }

    bool RollbackState(const State& state)
    {
        CCINIClass* baseline = CCINIClass::INI_Rules;
        if (!baseline) return false;
        const bool globals = ApplyGlobalRoutes(baseline, state.globalRoutes);
        const bool types = ApplyTypes(s_typeBaseline ? s_typeBaseline : baseline, state);
        return globals && types;
    }

    bool BuildCandidate()
    {
        std::memset(&s_candidate, 0, sizeof(s_candidate));
        s_candidateItemCount = 0;

        CCINIClass overlay;
        IniOverlay::MergeStats stats;
        if (!IniOverlay::MergeDirectory(&overlay, Config::Get().runtime.directory,
                                        &stats, "runtime")) {
            SetMessage("validation failed: %s", stats.firstError);
            Log::Warn("runtime: validation failed: %s", stats.firstError);
            return false;
        }

        CCINIClass* baseline = CCINIClass::INI_Rules;
        if (!baseline) {
            SetMessage("rules baseline is unavailable");
            return false;
        }

        for (auto* section = overlay.Sections.First();
             section && section->IsValid(); section = section->Next()) {
            if (!section->Name || !section->Name[0]) continue;
            for (auto* node = section->Entries.GenericList::First();
                 node && node->IsValid(); node = node->Next()) {
                auto* entry = static_cast<INIClass::INIEntry*>(node);
                if (!entry->Key || !entry->Key[0]) continue;
                if (s_candidateItemCount >= kMaxItems) {
                    SetMessage("validation failed: more than %d final keys", kMaxItems);
                    return false;
                }

                Item& item = s_candidateItems[s_candidateItemCount++];
                std::snprintf(item.section, sizeof(item.section), "%s", section->Name);
                std::snprintf(item.key, sizeof(item.key), "%s", entry->Key);
                std::snprintf(item.oldValue, sizeof(item.oldValue), "%s",
                              FindValue(baseline, section->Name, entry->Key));
                std::snprintf(item.newValue, sizeof(item.newValue), "%s",
                              entry->Value ? entry->Value : "");

                AbstractTypeClass* type = nullptr;
                unsigned int route = RouteNone;
                const char* reason = nullptr;
                item.safety = Classify(section->Name, entry->Key,
                                       &type, &route, &reason);
                std::snprintf(item.result, sizeof(item.result), "%s", reason);

                if (item.safety == Safety::RestartRequired) continue;
                if (!AddStateEntry(s_candidate, section->Name, entry->Key,
                                   entry->Value ? entry->Value : "")) {
                    SetMessage("validation failed: key or value exceeds runtime limits");
                    return false;
                }
                if (route != RouteNone) {
                    s_candidate.globalRoutes |= route;
                } else if (type && !AddTypeSection(s_candidate, section->Name)) {
                    SetMessage("validation failed: more than %d type sections",
                               kMaxTypeSections);
                    return false;
                }
            }
        }
        return true;
    }

    void UpdateApplyCounts(bool applied, bool restored = true)
    {
        int accepted = 0;
        int rejected = 0;
        for (int i = 0; i < s_candidateItemCount; ++i) {
            auto& item = s_candidateItems[i];
            if (item.safety == Safety::RestartRequired) {
                ++rejected;
            } else {
                ++accepted;
                std::snprintf(item.result, sizeof(item.result), "%s",
                              applied ? "applied" :
                              (restored ? "apply failed; previous state restored" :
                                          "apply failed; recovery incomplete"));
            }
        }
        EnterCriticalSection(&s_lock);
        s_snapshot.appliedKeys = applied ? accepted : s_snapshot.appliedKeys;
        s_snapshot.rejectedKeys = rejected;
        LeaveCriticalSection(&s_lock);
    }

    void ReloadNow()
    {
        if (!BuildCandidate()) {
            PublishItems();
            return;
        }

        s_previous = s_current;
        if (HasState(s_current) && !RollbackState(s_current)) {
            PublishItems();
            SetMessage("apply blocked: could not roll back previous state");
            Log::Error("runtime: could not roll back previous state");
            return;
        }

        if (!CaptureTypeBaselines(s_candidate)) {
            const bool restored = !HasState(s_previous) || ApplyState(s_previous);
            s_current = s_previous;
            PublishItems();
            SetMessage(restored
                ? "apply blocked: could not capture live type baseline"
                : "apply blocked: baseline capture and previous-state restore failed");
            Log::Error("runtime: baseline capture failed; previous restore=%d",
                       restored ? 1 : 0);
            return;
        }

        if (!ApplyState(s_candidate)) {
            const bool candidateRolledBack = RollbackState(s_candidate);
            bool previousRestored = false;
            if (candidateRolledBack) {
                previousRestored = !HasState(s_previous) || ApplyState(s_previous);
                s_current = s_previous;
            } else {
                // Keep the candidate routes/types tracked so a later rollback can retry.
                s_current = s_candidate;
            }
            UpdateApplyCounts(false, candidateRolledBack && previousRestored);
            PublishItems();
            if (candidateRolledBack && previousRestored) {
                SetMessage("apply failed; previous valid state restored");
            } else {
                SetMessage("apply failed; recovery incomplete, rollback will retry");
            }
            Log::Error("runtime: apply failed; candidate rollback=%d previous restore=%d",
                       candidateRolledBack ? 1 : 0, previousRestored ? 1 : 0);
            return;
        }

        s_current = s_candidate;
        UpdateApplyCounts(true);
        PublishItems();
        int generation = 0;
        int rejected = 0;
        EnterCriticalSection(&s_lock);
        s_snapshot.applied = s_current.entryCount > 0;
        generation = ++s_snapshot.generation;
        rejected = s_snapshot.rejectedKeys;
        LeaveCriticalSection(&s_lock);
        SetMessage("applied generation %d (%d key(s), %d rejected)",
                   generation, s_current.entryCount, rejected);
        Log::Info("runtime: generation %d applied (%d keys, %d rejected)",
                  generation, s_current.entryCount, rejected);
    }

    bool RollbackNow(const char* reason)
    {
        const bool hadState = HasState(s_current);
        if (hadState && !RollbackState(s_current)) {
            SetMessage("rollback failed: %s", reason);
            Log::Error("runtime: rollback failed (%s)", reason);
            return false;
        }
        std::memset(&s_current, 0, sizeof(s_current));
        EnterCriticalSection(&s_lock);
        s_snapshot.applied = false;
        s_snapshot.appliedKeys = 0;
        LeaveCriticalSection(&s_lock);
        SetMessage("rolled back: %s", reason);
        Log::Info("runtime: rolled back (%s)", reason);
        return true;
    }

    bool PopCommand(QueuedCommand* output)
    {
        EnterCriticalSection(&s_lock);
        if (s_queueCount == 0) {
            LeaveCriticalSection(&s_lock);
            return false;
        }
        *output = s_queue[s_queueHead];
        s_queueHead = (s_queueHead + 1) % kQueueCapacity;
        --s_queueCount;
        s_snapshot.pendingCommands = s_queueCount;
        LeaveCriticalSection(&s_lock);
        return true;
    }

}  // namespace

const char* SafetyName(Safety safety)
{
    switch (safety) {
    case Safety::Immediate: return "Immediate";
    case Safety::FutureObjects: return "FutureObjects";
    case Safety::ControlledReload: return "ControlledReload";
    case Safety::RestartRequired: return "RestartRequired";
    default: return "RestartRequired";
    }
}

void Initialize()
{
    if (s_initialized) return;
    s_initialized = true;
    InitializeCriticalSection(&s_lock);
    s_lockInitialized = true;

    Config::Load();
    const auto& config = Config::Get().runtime;
    EnsureDirectoryTree(config.directory);

    s_snapshot.initialized = true;
    s_snapshot.enabled = config.enabled;
    s_snapshot.autoApply = config.autoApply;
    std::snprintf(s_snapshot.directory, sizeof(s_snapshot.directory), "%s",
                  config.directory);
    std::snprintf(s_snapshot.lastMessage, sizeof(s_snapshot.lastMessage), "%s",
                  config.enabled ? "waiting for a single-player game" :
                                   "runtime disabled in ra2hook.ini");

    RuntimeProtocol::Start();
    if (config.enabled) RuntimeWatcher::Start(config.directory, config.debounceMs);
    Log::Info("runtime: initialized enabled=%d auto=%d dir=%s pipe=%s",
              config.enabled ? 1 : 0, config.autoApply ? 1 : 0,
              config.directory, kPipeName);
}

bool Queue(Command command, bool argument)
{
    if (!s_lockInitialized) return false;
    EnterCriticalSection(&s_lock);
    if (s_queueCount >= kQueueCapacity) {
        LeaveCriticalSection(&s_lock);
        return false;
    }
    s_queue[s_queueTail].command = command;
    s_queue[s_queueTail].argument = argument;
    s_queueTail = (s_queueTail + 1) % kQueueCapacity;
    ++s_queueCount;
    s_snapshot.pendingCommands = s_queueCount;
    LeaveCriticalSection(&s_lock);
    return true;
}

void NotifyFilesChanged()
{
    Queue(Command::FilesChanged);
}

void Tick()
{
    if (!s_initialized) Initialize();
    UpdateSessionSnapshot();

    const bool allowed = IsAllowedSession();
    const bool enabled = Config::Get().runtime.enabled;

    // Runtime state must never leak into a later network/replay session.
    if (!allowed && (s_wasAllowed || HasState(s_current))) {
        if (RollbackNow("single-player session ended")) {
            ResetTypeBaselines();
        }
    }

    if (allowed && !s_wasAllowed && enabled && s_snapshot.autoApply) {
        ReloadNow();
    }
    s_wasAllowed = allowed;

    QueuedCommand command;
    while (PopCommand(&command)) {
        if (command.command == Command::SetAutoApply) {
            EnterCriticalSection(&s_lock);
            s_snapshot.autoApply = command.argument;
            LeaveCriticalSection(&s_lock);
            SetMessage("auto apply %s", command.argument ? "enabled" : "paused");
            continue;
        }

        if (command.command == Command::Rollback) {
            if (!allowed && SessionClass::Instance.CurrentlyInGame) {
                SetMessage("blocked: rollback command is unavailable in multiplayer/replay");
            } else {
                RollbackNow("manual command");
            }
            continue;
        }

        const bool autoApply = s_snapshot.autoApply;
        const bool shouldReload = command.command == Command::Reload ||
            (command.command == Command::FilesChanged && autoApply);
        if (!shouldReload) continue;
        if (!enabled) {
            SetMessage("blocked: [Runtime] Enabled=no");
        } else if (!allowed) {
            SetMessage("blocked: runtime writes require campaign or skirmish");
        } else {
            ReloadNow();
        }
    }
}

void GetSnapshot(Snapshot* output)
{
    if (!output || !s_lockInitialized) return;
    EnterCriticalSection(&s_lock);
    *output = s_snapshot;
    LeaveCriticalSection(&s_lock);
}

int GetItems(Item* output, int capacity)
{
    if (!output || capacity <= 0 || !s_lockInitialized) return 0;
    EnterCriticalSection(&s_lock);
    const int count = s_itemCount < capacity ? s_itemCount : capacity;
    if (count > 0) {
        std::memcpy(output, s_items, sizeof(Item) * static_cast<size_t>(count));
    }
    LeaveCriticalSection(&s_lock);
    return count;
}

}  // namespace Runtime
