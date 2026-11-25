#pragma once

namespace rex {
struct PathConfig;
}

namespace nier::packaged {

bool Active();
void ConfigurePaths(rex::PathConfig& paths);
void LogStatus();

}  // namespace nier::packaged
