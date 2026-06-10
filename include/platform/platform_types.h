#pragma once

// INT32 / uint32 come from the business project's internal api.h. For a
// standalone build of mem-manager we define compatible aliases here; when
// integrating, define MEM_MANAGER_USE_API_H so they come from api.h instead.
#ifdef MEM_MANAGER_USE_API_H
#  include "api.h"
#else
#  include <cstdint>
typedef std::int32_t  INT32;
typedef std::uint32_t uint32;
#endif
