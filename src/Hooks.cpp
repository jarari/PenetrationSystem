#include "Hooks.h"

#include "PenetrationSystem.h"

namespace Hooks
{
	void InitializeHooks()
	{
		Penetration::Initialize();
	}
}