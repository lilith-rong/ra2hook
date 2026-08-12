#include <windows.h>
#include <sddl.h>

#include <cstdio>
#include <cstring>

#include "Logger.h"
#include "Runtime.h"
#include "RuntimeProtocol.h"

namespace RuntimeProtocol {
namespace {

    bool s_started = false;
    Runtime::Item s_items[Runtime::kMaxItems] = {};

    bool WriteAll(HANDLE pipe, const char* data)
    {
        const char* cursor = data;
        DWORD remaining = static_cast<DWORD>(std::strlen(data));
        while (remaining > 0) {
            DWORD written = 0;
            if (!WriteFile(pipe, cursor, remaining, &written, nullptr) || written == 0)
                return false;
            cursor += written;
            remaining -= written;
        }
        return true;
    }

    bool ReadLine(HANDLE pipe, char* output, size_t capacity)
    {
        if (!output || capacity == 0) return false;
        size_t length = 0;
        for (;;) {
            char ch = 0;
            DWORD read = 0;
            if (!ReadFile(pipe, &ch, 1, &read, nullptr) || read == 0) return false;
            if (ch == '\n') break;
            if (ch == '\r') continue;
            if (length + 1 < capacity) output[length++] = ch;
        }
        output[length] = '\0';
        return true;
    }

    void EncodeField(const char* input, char* output, size_t capacity)
    {
        static const char hex[] = "0123456789ABCDEF";
        if (!output || capacity == 0) return;
        size_t used = 0;
        for (const unsigned char* p = reinterpret_cast<const unsigned char*>(input ? input : "");
             *p && used + 1 < capacity; ++p) {
            const bool escape = *p == '%' || *p == '\t' || *p == '\r' || *p == '\n';
            if (escape) {
                if (used + 3 >= capacity) break;
                output[used++] = '%';
                output[used++] = hex[*p >> 4];
                output[used++] = hex[*p & 0x0F];
            } else {
                output[used++] = static_cast<char>(*p);
            }
        }
        output[used] = '\0';
    }

    void WriteStatus(HANDLE pipe)
    {
        Runtime::Snapshot snapshot;
        Runtime::GetSnapshot(&snapshot);
        char directory[780] = {};
        char message[780] = {};
        EncodeField(snapshot.directory, directory, sizeof(directory));
        EncodeField(snapshot.lastMessage, message, sizeof(message));

        char line[2048] = {};
        std::snprintf(line, sizeof(line),
            "STATUS\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%s\t%s\t%s\n",
            snapshot.initialized ? 1 : 0, snapshot.enabled ? 1 : 0,
            snapshot.autoApply ? 1 : 0, snapshot.currentlyInGame ? 1 : 0,
            snapshot.singlePlayer ? 1 : 0, snapshot.replay ? 1 : 0,
            snapshot.applied ? 1 : 0, snapshot.generation,
            snapshot.appliedKeys, snapshot.rejectedKeys, snapshot.pendingCommands,
            snapshot.mode, directory, message);
        WriteAll(pipe, line);
    }

    void WriteItems(HANDLE pipe)
    {
        const int count = Runtime::GetItems(s_items, Runtime::kMaxItems);
        char section[192] = {};
        char key[288] = {};
        char oldValue[768] = {};
        char newValue[1536] = {};
        char result[288] = {};
        char line[4096] = {};

        for (int i = 0; i < count; ++i) {
            const auto& item = s_items[i];
            EncodeField(item.section, section, sizeof(section));
            EncodeField(item.key, key, sizeof(key));
            EncodeField(item.oldValue, oldValue, sizeof(oldValue));
            EncodeField(item.newValue, newValue, sizeof(newValue));
            EncodeField(item.result, result, sizeof(result));
            std::snprintf(line, sizeof(line), "ITEM\t%s\t%s\t%s\t%s\t%s\t%s\n",
                          section, key, oldValue, newValue,
                          Runtime::SafetyName(item.safety), result);
            if (!WriteAll(pipe, line)) break;
        }
    }

    void HandleRequest(HANDLE pipe, const char* request)
    {
        if (_stricmp(request, "HELLO 1") == 0) {
            WriteAll(pipe, "HELLO\t1\tra2hook-runtime\n");
        } else if (_stricmp(request, "STATUS") == 0) {
            WriteStatus(pipe);
        } else if (_stricmp(request, "INSPECT") == 0) {
            WriteItems(pipe);
        } else if (_stricmp(request, "RELOAD") == 0) {
            WriteAll(pipe, Runtime::Queue(Runtime::Command::Reload)
                ? "OK\treload queued\n" : "ERROR\tcommand queue full\n");
        } else if (_stricmp(request, "ROLLBACK") == 0) {
            WriteAll(pipe, Runtime::Queue(Runtime::Command::Rollback)
                ? "OK\trollback queued\n" : "ERROR\tcommand queue full\n");
        } else if (_stricmp(request, "AUTO 1") == 0) {
            WriteAll(pipe, Runtime::Queue(Runtime::Command::SetAutoApply, true)
                ? "OK\tauto apply queued\n" : "ERROR\tcommand queue full\n");
        } else if (_stricmp(request, "AUTO 0") == 0) {
            WriteAll(pipe, Runtime::Queue(Runtime::Command::SetAutoApply, false)
                ? "OK\tauto pause queued\n" : "ERROR\tcommand queue full\n");
        } else {
            WriteAll(pipe, "ERROR\tunknown command\n");
        }
        WriteAll(pipe, "END\n");
    }

    DWORD WINAPI PipeThread(void*)
    {
        for (;;) {
            PSECURITY_DESCRIPTOR descriptor = nullptr;
            SECURITY_ATTRIBUTES security = {};
            SECURITY_ATTRIBUTES* securityPointer = nullptr;
            if (ConvertStringSecurityDescriptorToSecurityDescriptorA(
                    "D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;AU)S:(ML;;NW;;;LW)",
                    SDDL_REVISION_1, &descriptor, nullptr)) {
                security.nLength = sizeof(security);
                security.lpSecurityDescriptor = descriptor;
                security.bInheritHandle = FALSE;
                securityPointer = &security;
            } else {
                Log::Warn("runtime pipe: security descriptor failed (%lu)",
                          GetLastError());
            }

            HANDLE pipe = CreateNamedPipeA(Runtime::PipeName(),
                PIPE_ACCESS_DUPLEX,
                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
                    PIPE_REJECT_REMOTE_CLIENTS,
                PIPE_UNLIMITED_INSTANCES, 4096, 4096, 0, securityPointer);
            if (descriptor) LocalFree(descriptor);
            if (pipe == INVALID_HANDLE_VALUE) {
                Log::Error("runtime pipe: CreateNamedPipe failed for %s (%lu)",
                           Runtime::PipeName(), GetLastError());
                Sleep(1000);
                continue;
            }

            const BOOL connected = ConnectNamedPipe(pipe, nullptr) ||
                                   GetLastError() == ERROR_PIPE_CONNECTED;
            if (connected) {
                char request[256] = {};
                if (ReadLine(pipe, request, sizeof(request))) HandleRequest(pipe, request);
                FlushFileBuffers(pipe);
                DisconnectNamedPipe(pipe);
            }
            CloseHandle(pipe);
        }
    }

}  // namespace

bool Start()
{
    if (s_started) return true;
    HANDLE thread = CreateThread(nullptr, 0, PipeThread, nullptr, 0, nullptr);
    if (!thread) {
        Log::Error("runtime pipe: CreateThread failed (%lu)", GetLastError());
        return false;
    }
    CloseHandle(thread);
    s_started = true;
    return true;
}

}  // namespace RuntimeProtocol
