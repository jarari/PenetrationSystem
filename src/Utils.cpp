#include "Utils.h"

#include <algorithm>
#include <cctype>
#include <cstdint>

#include <REL/Relocation.h>

#include <RE/Bethesda/Actor.h>
#include <RE/Bethesda/TESForms.h>
#include <RE/Bethesda/TESObjectREFRs.h>
#include <RE/Havok/hknpCollisionResult.h>

namespace RE
{
	class hknpBSWorld;
}

namespace
{
	REL::Relocation<std::uint64_t*> g_collisionFilterRoot{ REL::ID(469495) };
	REL::Relocation<float*> g_ptrBS2HkScale{ REL::ID(1126486) };

	void ConfigurePickFilter(RE::bhkPickData& pickData, RE::Actor* shooter, RE::BGSProjectile* projectileBase, bool excludeShooter)
	{
		std::uint32_t collisionIndex = 6;
		std::uint64_t flagMask = 0x15C15160;

		if (projectileBase) {
			if (projectileBase->data.collisionLayer) {
				collisionIndex = projectileBase->data.collisionLayer->collisionIdx;
			}
			if (projectileBase->CollidesWithSmallTransparentLayer()) {
				flagMask = 0x1C15160;
			}
		}

		std::uint64_t filterRoot = 0;
		if (g_collisionFilterRoot.address() != 0) {
			filterRoot = *g_collisionFilterRoot;
		}
		if (filterRoot) {
			auto* filterEntry = reinterpret_cast<std::uint64_t*>(filterRoot + 0x1A0 + (0x8 * collisionIndex));
			const std::uint64_t collisionFilter = (*filterEntry | 0x40000000ull) & ~flagMask;
			*reinterpret_cast<std::uint64_t*>(reinterpret_cast<std::uintptr_t>(&pickData) + 0xC8) = collisionFilter;
		}

		std::uint32_t collisionGroup = 6;
		if (excludeShooter && shooter && shooter->loadedData) {
			auto* loadedFlag = reinterpret_cast<std::uint8_t*>(shooter->loadedData) + 0x20;
			if ((*loadedFlag & 0x1) != 0) {
				collisionGroup = shooter->GetCurrentCollisionGroup();
			}
		}

		*reinterpret_cast<std::uint32_t*>(reinterpret_cast<std::uintptr_t>(&pickData) + 0x0C) = (collisionGroup << 16);
	}
}

namespace Utils
{
	std::string Trim(std::string_view value)
	{
		const auto begin = std::find_if_not(value.begin(), value.end(), [](unsigned char ch) { return std::isspace(ch) != 0; });
		const auto end = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char ch) { return std::isspace(ch) != 0; }).base();
		if (begin >= end) {
			return {};
		}
		return std::string(begin, end);
	}

	std::string SplitString(const std::string& str, const std::string& delimiter, std::string& remainder)
	{
		const auto pos = str.find(delimiter);
		if (pos == std::string::npos) {
			remainder.clear();
			return str;
		}

		remainder = str.substr(pos + delimiter.size());
		return str.substr(0, pos);
	}

	RE::Actor* ResolveActor(const RE::ObjectRefHandle& handle) noexcept
	{
		if (!handle) {
			return nullptr;
		}

		if (auto ref = handle.get()) {
			return ref->As<RE::Actor>();
		}

		return nullptr;
	}

	bool PerformRaycast(
		RE::Projectile& projectile,
		RE::Actor* shooter,
		RE::BGSProjectile* projectileBase,
		const RE::NiPoint3& start,
		const RE::NiPoint3& end,
		RE::bhkPickData& pickData,
		RaycastHit& outHit,
		bool excludeShooter)
	{
		auto* cell = projectile.parentCell;
		if (!cell) {
			return false;
		}

		auto* world = cell->GetbhkWorld();
		if (!world) {
			return false;
		}

		auto* hkWorld = *reinterpret_cast<RE::hknpBSWorld**>(reinterpret_cast<std::uintptr_t>(world) + 0x60);
		if (!hkWorld) {
			return false;
		}

		pickData.Reset();
		pickData.SetStartEnd(start, end);

		if (auto* collector = new RE::hknpAllHitsCollector()) {
			*reinterpret_cast<std::uintptr_t*>(reinterpret_cast<std::uintptr_t>(&pickData) + 0xD0) = reinterpret_cast<std::uintptr_t>(collector);
			*reinterpret_cast<std::uint32_t*>(reinterpret_cast<std::uintptr_t>(&pickData) + 0xD8) = 0;
			collector->Reset();
		} else {
			logger::warn("[Penetration] Failed to allocate all-hits collector; penetration raycasts limited to closest hit");
		}

		ConfigurePickFilter(pickData, shooter, projectileBase, excludeShooter);

		if (!cell->Pick(pickData)) {
			return false;
		}

		if (!pickData.HasHit()) {
			return false;
		}

		const float worldScale = g_ptrBS2HkScale.address() != 0 ? *g_ptrBS2HkScale : 1.0f;
		const auto& hitPosition = pickData.result.position;
		const auto& hitNormal = pickData.result.normal;

		outHit.point = RE::NiPoint3(hitPosition.x, hitPosition.y, hitPosition.z) / worldScale;
		outHit.normal = RE::NiPoint3(hitNormal.x, hitNormal.y, hitNormal.z);

		return true;
	}

	bool SelectRealExit(RE::bhkPickData& pickData, const RE::NiPoint3& reference, RaycastHit& outHit)
	{
		const std::int32_t hitCount = pickData.GetAllCollectorRayHitSize();
		if (hitCount <= 0) {
			return false;
		}

		const float worldScale = g_ptrBS2HkScale.address() != 0 ? *g_ptrBS2HkScale : 1.0f;
		RE::hknpCollisionResult temp{};
		bool found = false;

		for (std::int32_t index = 0; index < hitCount; ++index) {
			if (!pickData.GetAllCollectorRayHitAt(static_cast<std::uint32_t>(index), temp)) {
				continue;
			}

			RE::NiPoint3 point(temp.position.x, temp.position.y, temp.position.z);
			point /= worldScale;

			const float distanceSq = reference.GetSquaredDistance(point);
			if (distanceSq >= 2.25f) {
				outHit.point = point;
				outHit.normal = RE::NiPoint3(temp.normal.x, temp.normal.y, temp.normal.z);
				found = true;
				break;
			}
		}

		return found;
	}

	RE::ProjectileHandle Launch(const RE::ProjectileLaunchData& data)
	{
		using func_t = decltype(&Utils::Launch);
		REL::Relocation<func_t> func{ REL::ID(1452334) };
		return func(data);
	}
}
