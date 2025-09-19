#pragma once

#include <string>
#include <string_view>

#include <RE/Bethesda/BSPointerHandle.h>
#include <RE/Bethesda/Projectiles.h>
#include <RE/Bethesda/TESBoundObjects.h>
#include <RE/Bethesda/bhkPickData.h>

namespace Utils
{
	std::string Trim(std::string_view value);
	std::string SplitString(const std::string& str, const std::string& delimiter, std::string& remainder);

	RE::Actor* ResolveActor(const RE::ObjectRefHandle& handle) noexcept;

	struct RaycastHit
	{
		RE::NiPoint3 point;
		RE::NiPoint3 normal;
	};

	bool PerformRaycast(
		RE::Projectile& projectile,
		RE::Actor* shooter,
		RE::BGSProjectile* projectileBase,
		const RE::NiPoint3& start,
		const RE::NiPoint3& end,
		RE::bhkPickData& pickData,
		RaycastHit& outHit,
		bool excludeShooter = true);

	bool SelectRealExit(RE::bhkPickData& pickData, const RE::NiPoint3& reference, RaycastHit& outHit);
	RE::ProjectileHandle Launch(const RE::ProjectileLaunchData& data);
}
