#pragma once

// Suppress C++17 deprecation of std::codecvt used in F4SEMenuFramework.h
#define _SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING

#pragma warning(push)
#include "F4SE/F4SE.h"
#include "RE/Fallout.h"
#pragma warning(pop)

#pragma warning(disable: 4099)
#pragma warning(disable: 4100)
#pragma warning(disable: 4189)
#pragma warning(disable: 4244)
#pragma warning(disable: 4302)
#pragma warning(disable: 4311)
#pragma warning(disable: 5054)

#define DLLEXPORT __declspec(dllexport)

#include <spdlog/sinks/basic_file_sink.h>

namespace logger = F4SE::log;

using namespace std::literals;

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <format>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// SimpleIni for INI file parsing
#include "SimpleIni.h"
