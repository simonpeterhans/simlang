#pragma once

#include "util/types.h"

namespace simlang
{

enum class ModuleStage : u8
{
    cCreated,
    cParsed,
    cDeclsCollected,
    cImportsBound,
    cNamesResolved,
    cThisRewritten,
    cCapturesAnalyzed,
    cTypesChecked,
    cConstsFolded
};

} // namespace simlang
