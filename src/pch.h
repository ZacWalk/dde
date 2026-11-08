// pch.h — Precompiled header: standard library includes and global declarations

#pragma once

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cassert>
#include <charconv>
#include <cstdio>
#include <cstdarg>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <compare>
#include <format>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "libdesktop/core.h"
#include "libdesktop/platform.h"
#include "libdesktop/render.h"
#include "libdesktop/ui.h"
#include "libdesktop/ui/controls.h"
#include "libdesktop/ui/elements.h"
#include "libdesktop/ui/menu_bar.h"
#include "libdesktop/ui/views.h"
#include "libdesktop/ui/test_support.h"
#include "libdesktop/platform/dialogs.h"
#include "libdesktop/platform/file.h"
#include "libdesktop/platform/input.h"
#include "libdesktop/platform/process.h"
#include "libdesktop/platform/services.h"
#include "libdesktop/platform/shell.h"
#include "libdesktop/platform/tasks.h"
#include "libdesktop/platform/text.h"
#include "util.h"


extern std::string g_app_name;
