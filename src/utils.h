#pragma once

#include <string>
#include <cstdint>
#include <ctime>

// --- Logging (prints to stderr with prefix) ---

void LogInfo(const std::string& msg);
void LogError(const std::string& msg);

// --- Windows error formatting ---

std::string GetLastWinErrorMessage();
std::string GetErrorMessage(const std::string& context);

// --- Human-readable formatting ---

std::string FormatSize(uint64_t bytes);
std::string FormatTime(time_t t);
std::string FormatTimeAgo(time_t t, time_t now);

// --- String helpers ---

std::string Trim(const std::string& s);
bool StartsWith(const std::string& s, const std::string& prefix);
