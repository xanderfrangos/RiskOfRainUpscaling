#include "Core/Native.h"
#include <filesystem>
#include <fstream>
#include <iterator>
#include <iostream>

int main() {
    auto folder = std::filesystem::absolute(L"build/test-results/logging");
    std::filesystem::create_directories(folder);
    // Also prove that an Off startup suppresses initialization messages.
    RiskOfRainUpscaling_SetLoggingLevel(0);
    RiskOfRainUpscaling_Initialize(folder.c_str(), L"unused", L"logging-test");
    auto size = [&]() { return std::filesystem::file_size(folder / L"native.log"); };
    if (size() != 0) return 1;
    auto emit = [](int level) {
        RiskOfRainUpscaling_SetLoggingLevel(level);
        Log(ModLogLevel::Error, "level=%d error", level);
        Log(ModLogLevel::Warning, "level=%d warning", level);
        Log("level=%d info", level);
        Log(ModLogLevel::Debug, "level=%d debug", level);
    };
    for (int level = 0; level <= 4; ++level) emit(level);
    auto before = size();
    emit(0);
    if (size() != before) return 2;
    std::ifstream file(folder / L"native.log");
    std::string contents((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    const char* labels[] = { "error", "warning", "info", "debug" };
    for (int level = 0; level <= 4; ++level) {
        for (int severity = 1; severity <= 4; ++severity) {
            std::string message = "level=" + std::to_string(level) + " " + labels[severity - 1];
            bool found = contents.find(message) != std::string::npos;
            if (found != (level >= severity)) return 3;
        }
    }
    std::cout << "PASS: native startup Off, every logging threshold and live Off/on changes.\n";
}
