#pragma once

namespace Runtime {

    constexpr int kMaxItems = 512;

    enum class Safety {
        Immediate,
        FutureObjects,
        ControlledReload,
        RestartRequired
    };

    enum class Command {
        Reload,
        Rollback,
        SetAutoApply,
        FilesChanged
    };

    struct Item {
        char section[64] = {};
        char key[96] = {};
        char oldValue[256] = {};
        char newValue[512] = {};
        Safety safety = Safety::RestartRequired;
        char result[96] = {};
    };

    struct Snapshot {
        bool initialized = false;
        bool enabled = false;
        bool autoApply = false;
        bool currentlyInGame = false;
        bool singlePlayer = false;
        bool replay = false;
        bool applied = false;
        int generation = 0;
        int appliedKeys = 0;
        int rejectedKeys = 0;
        int pendingCommands = 0;
        char mode[32] = {};
        char directory[260] = {};
        char lastMessage[256] = {};
    };

    constexpr const char* kPipeName = "\\\\.\\pipe\\ra2hook-runtime-v1";

    void Initialize();
    void Tick();

    bool Queue(Command command, bool argument = false);
    void NotifyFilesChanged();

    void GetSnapshot(Snapshot* output);
    int GetItems(Item* output, int capacity);
    const char* SafetyName(Safety safety);

}  // namespace Runtime
