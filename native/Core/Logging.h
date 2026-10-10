#pragma once

// Values match ModLogLevel in the managed config.
enum class ModLogLevel { Off, Error, Warning, Info, Debug };
inline bool ShouldLog(ModLogLevel configured, ModLogLevel severity) {
    return severity >= ModLogLevel::Error && severity <= ModLogLevel::Debug && configured >= severity;
}
