#pragma once
#include <string>

bool InitLogFile(const std::string& path);
void WriteLog(const std::string& msg);
void CloseLogFile();
bool Daemonize(int argc, char* argv[]);
void* CreateUnmountEvent();
bool IsUnmountSignaled(void* eventHandle);
void SignalUnmount(void* eventHandle);
void SetAutoStart(bool enable);
