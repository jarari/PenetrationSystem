#pragma once

namespace Penetration
{
	void LoadConfig();
	float GetPenetrationMultiplier(const RE::TESAmmo* ammo) noexcept;
	float GetMaterialMultiplier(const RE::BGSMaterialType* material) noexcept;
}
namespace RE
{
	class TESAmmo;
	class BGSMaterialType;
}
