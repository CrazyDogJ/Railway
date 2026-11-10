#pragma once

#include "CoreMinimal.h"

namespace Railway::Editor
{
    FDelegateHandle RegisterTrainDebugDraw();
    void UnregisterTrainDebugDraw(FDelegateHandle Handle);
}
