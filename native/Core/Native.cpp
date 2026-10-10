#include "Core/Native.h"
#include "FrameGeneration/FrameBridge.h"
#include "Presentation/GraphicsHooks.h"
#include <cstdarg>
#include <cstdio>
#include <mutex>

std::wstring g_logFolder, g_runtimeFolder;
static std::mutex logMutex, initMutex;
static HANDLE logFile = INVALID_HANDLE_VALUE;
static std::atomic<ModLogLevel> loggingLevel{ModLogLevel::Info};

extern "C" void __cdecl RiskOfRainUpscaling_SetLoggingLevel(int level)
{
    std::lock_guard<std::mutex> guard(logMutex);
    loggingLevel = level >= 0 && level <= 4 ? static_cast<ModLogLevel>(level) : ModLogLevel::Info;
}

static void WriteLog(ModLogLevel severity, const char* format, va_list args)
{
    if (!ShouldLog(loggingLevel.load(), severity)) return;
    char message[4096];
    vsnprintf_s(message, sizeof(message), _TRUNCATE, format, args);
    std::lock_guard<std::mutex> guard(logMutex);
    if (logFile == INVALID_HANDLE_VALUE || !ShouldLog(loggingLevel.load(), severity)) return;
    char line[4352];
    SYSTEMTIME now{}; GetSystemTime(&now);
    int size = sprintf_s(line, "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ tid=%lu %s\r\n", now.wYear, now.wMonth, now.wDay,
        now.wHour, now.wMinute, now.wSecond, now.wMilliseconds, GetCurrentThreadId(), message);
    DWORD written{}; WriteFile(logFile, line, size, &written, nullptr);
    FlushFileBuffers(logFile);
}

bool LogEnabled(ModLogLevel severity) { return ShouldLog(loggingLevel.load(), severity); }

void Log(const char* format, ...)
{
    va_list args; va_start(args, format);
    WriteLog(ModLogLevel::Info, format, args); va_end(args);
}

void Log(ModLogLevel severity, const char* format, ...)
{
    va_list args; va_start(args, format);
    WriteLog(severity, format, args); va_end(args);
}

extern "C" int __cdecl RiskOfRainUpscaling_Initialize(const wchar_t* logs, const wchar_t* runtimes, const wchar_t* startupStage)
{
    std::lock_guard<std::mutex> guard(initMutex);
    if (!logs || !runtimes || !startupStage) return -1;
    if (g_logFolder.empty())
    {
        g_logFolder = logs; g_runtimeFolder = runtimes;
        CreateDirectoryW(logs, nullptr);
        logFile = CreateFileW((g_logFolder + L"\\native.log").c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    Log("Initialize stage=%ls nativeABI=1 x64=true; upscaling runs from render events, not initialization; UnityPlayer=%p d3d11-already-loaded=%p dxgi-already-loaded=%p",
        startupStage, GetModuleHandleW(L"UnityPlayer.dll"), GetModuleHandleW(L"d3d11.dll"), GetModuleHandleW(L"dxgi.dll"));
    if (!wcscmp(startupStage, L"preloader") || !wcscmp(startupStage, L"standalone-test")) {
        ConfigureFrameBridge();return InstallGraphicsHooks();
    }
    if (!wcscmp(startupStage, L"plugin-awake")) return VerifyGraphicsHooks() ? 0 : -1;
    Log("Startup tracing not installed from plugin Awake; preloader configuration/timing controls early hooks");
    return 0;
}

extern "C" int __cdecl RiskOfRainUpscaling_GetObservedDeviceCount() { return ObservedDeviceCount(); }
