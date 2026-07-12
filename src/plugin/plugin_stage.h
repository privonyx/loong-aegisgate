#pragma once
#include "core/pipeline.h"

#ifdef _WIN32
#define AEGISGATE_PLUGIN_API extern "C" __declspec(dllexport)
#else
#define AEGISGATE_PLUGIN_API extern "C" __attribute__((visibility("default")))
#endif

constexpr int AEGISGATE_PLUGIN_API_VERSION = 1;

using PluginCreateFunc = aegisgate::PipelineStage* (*)();
using PluginDestroyFunc = void (*)(aegisgate::PipelineStage*);
using PluginInfoFunc = const char* (*)();
