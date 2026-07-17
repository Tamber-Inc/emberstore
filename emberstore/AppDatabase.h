#pragma once

#include "Database.h"

#include <eacp/Core/Utils/FilePath.h>

#include <string_view>

namespace emberstore
{
// The one place an app's store root is decided:
// <app data>/<company>/<appName> — ~/Library/Application Support/... on macOS,
// %APPDATA%\... on Windows, $XDG_DATA_HOME/... on Linux. Reach for this instead
// of composing a path by hand, so every app's data lands somewhere predictable
// and no two of them disagree about where that is.
inline eacp::FilePath appDataDirectory(std::string_view company,
                                       std::string_view appName)
{
    return eacp::FilePath::appDataDirectory() / company / appName;
}

inline Database databaseForApp(std::string_view company,
                               std::string_view appName,
                               Durability durability = Durability::Durable)
{
    return Database {appDataDirectory(company, appName), durability};
}
} // namespace emberstore
