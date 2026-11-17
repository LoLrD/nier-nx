#pragma once

#include <memory>

#include <rex/system/interfaces/graphics.h>

namespace nier::native {

std::unique_ptr<rex::system::IGraphicsSystem> CreateNativeGraphicsSystem();

}  // namespace nier::native
