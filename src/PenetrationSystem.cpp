#include "PenetrationSystem.h"

#include "PenetrationConfig.h"
#include "Utils.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <mutex>
#include <string_view>
#include <unordered_map>

#include <REL/Relocation.h>

#include <RE/Bethesda/Projectiles.h>
#include <RE/Bethesda/TESDataHandler.h>
#include <RE/Bethesda/TESForms.h>
#include <RE/Bethesda/TESObjectREFRs.h>
#include <RE/Havok/hknpAllHitsCollector.h>

namespace Penetration
{
    namespace
    {
        using ProjectileProcessFn = bool (*)(RE::Projectile*);
        using MissileProcessFn = bool (*)(RE::MissileProjectile*);
        using BeamProcessFn = bool (*)(RE::BeamProjectile*);

        std::uintptr_t g_projectileProcessImpactsOriginal = 0;
        std::uintptr_t g_missileProcessImpactsOriginal = 0;
        std::uintptr_t g_beamProcessImpactsOriginal = 0;

        RE::bhkPickData g_pickData;

        using ProjectileHandleId = RE::ProjectileHandle::native_handle_type;
        using PendingShooterMap = std::unordered_map<ProjectileHandleId, RE::ObjectRefHandle>;

        PendingShooterMap g_pendingShooters;
        std::mutex g_pendingShootersMutex;

        void QueuePendingShooterAssignment(RE::ProjectileHandle handle, RE::ObjectRefHandle shooter)
        {
            if (!handle || !shooter) {
                return;
            }

            const ProjectileHandleId key = handle.native_handle();
            std::scoped_lock lock(g_pendingShootersMutex);
            g_pendingShooters[key] = shooter;
        }

        void ApplyPendingShooter(RE::Projectile* projectile)
        {
            if (!projectile) {
                return;
            }

            RE::ProjectileHandle handle{ projectile };
            if (!handle) {
                return;
            }

            const ProjectileHandleId key = handle.native_handle();
            RE::ObjectRefHandle shooter;

            {
                std::scoped_lock lock(g_pendingShootersMutex);
                auto it = g_pendingShooters.find(key);
                if (it == g_pendingShooters.end()) {
                    return;
                }
				logger::info(FMT_STRING("Matching shooter found for handle {:08X}"), key);
                shooter = it->second;
                g_pendingShooters.erase(it);
            }

            projectile->shooter = shooter;
        }

        RE::BGSProjectile* GetProjectileBase(RE::Projectile& projectile)
        {
            auto* baseObject = projectile.GetObjectReference();
            return baseObject ? baseObject->As<RE::BGSProjectile>() : nullptr;
        }

        float CalculatePenetrationDepth(
            const RE::Projectile& projectile,
            const RE::BGSProjectile* projectileBase,
            float ammoMultiplier,
            float materialMultiplier)
        {
            float impactForce = 6.0f;
            if (projectileBase) {
                impactForce = projectileBase->data.force;
            }

            const float combinedMultiplier = ammoMultiplier * materialMultiplier;
			float depth = impactForce * projectile.damage / 10.0f * combinedMultiplier;

			logger::info(
				FMT_STRING("[Penetration] Calculation depth {} (impactForce {:.2f}, damage {:.2f}, ammo {:.2f}, material {:.2f})"),
				depth,
				impactForce,
				projectile.damage,
				ammoMultiplier,
				materialMultiplier);
			return depth;
        }

        bool SpawnPenetratedProjectile(RE::Projectile& source, const Utils::RaycastHit& hit, const RE::NiPoint3& launchDir, float remainingPower)
        {
            auto* cell = source.parentCell;
            if (!cell) {
                return false;
            }

            auto* projectileBase = GetProjectileBase(source);
            if (!projectileBase) {
                return false;
            }

            auto* dataHandler = RE::TESDataHandler::GetSingleton();
            if (!dataHandler) {
                return false;
            }

            const float clampedZ = std::clamp(launchDir.z, -1.0f, 1.0f);
            const RE::NiPoint3 launchAngles{ -std::asin(clampedZ), 0.0f, std::atan2(launchDir.x, launchDir.y) };

			RE::ProjectileLaunchData projData
			{
				.fromWeapon = RE::BGSObjectInstanceT<RE::TESObjectWEAP>(
					static_cast<RE::TESObjectWEAP*>(source.weaponSource.object), source.weaponSource.instanceData.get())
			};
			projData.origin = hit.point + launchDir * 5.0f;
			projData.projectileBase = projectileBase;
			projData.fromAmmo = source.ammoSource;
			projData.equipIndex = source.equipIndex;
			projData.xAngle = launchAngles.x;
			projData.zAngle = launchAngles.z;
			projData.parentCell = cell;
			projData.spell = source.spell;
			projData.power = remainingPower;
			projData.useOrigin = true;
			projData.ignoreNearCollisions = true;

			auto handle = Utils::Launch(projData);
            auto ref = handle ? handle.get() : nullptr;
            auto* spawned = ref ? ref->As<RE::Projectile>() : nullptr;
            if (!spawned) {
                return false;
			}

			logger::info(
				FMT_STRING("[Penetration] Spawned projectile at ({:.2f}, {:.2f}, {:.2f}) remaining power {:.2f} handle {:08X}"),
				spawned->data.location.x,
				spawned->data.location.y,
				spawned->data.location.z,
				remainingPower,
				handle.native_handle());

			spawned->avEffect = source.avEffect;
			spawned->damage = source.damage * remainingPower / source.power;
			spawned->SetActorCause(source.GetActorCause());

			if (handle && source.shooter) {
				QueuePendingShooterAssignment(handle, source.shooter);
			}

            return true;
        }

        bool TryHandlePenetration(RE::Projectile* projectile)
        {
            if (!projectile) {
                return false;
			}

            if (projectile->explosion) {
                return false;
            }

            auto& impacts = projectile->impacts;
            RE::Projectile::ImpactData* impactData = nullptr;
            for (auto& impact : impacts) {
                if (!impact.processed) {
                    impactData = std::addressof(impact);
                    break;
                }
            }

            if (!impactData) {
                return false;
            }

            auto* projectileBase = GetProjectileBase(*projectile);
            const float ammoMultiplier = Penetration::GetPenetrationMultiplier(projectile->ammoSource);
            const float materialMultiplier = Penetration::GetMaterialMultiplier(impactData->materialType);
			const float penetrationDepth = CalculatePenetrationDepth(*projectile, projectileBase, ammoMultiplier, materialMultiplier);
			if (impactData->materialType) {
				logger::info(
					FMT_STRING("[Penetration] Impact Material: {}"),
					impactData->materialType->GetFormEditorID());
			}
            if (penetrationDepth <= 0.0f) {
                return false;
            }

            logger::info(
                FMT_STRING("[Penetration] Impact at ({:.2f}, {:.2f}, {:.2f}) power {:.2f} damage {:.2f} ammo {:.2f} material {:.2f} depth {:.2f}"),
                impactData->location.x,
                impactData->location.y,
                impactData->location.z,
                projectile->power,
                projectile->damage,
                ammoMultiplier,
                materialMultiplier,
                penetrationDepth);
			
			float pitch = projectile->data.angle.x;
			float yaw = projectile->data.angle.z;
			RE::NiPoint3 direction{ cos(pitch) * sin(yaw), cos(pitch) * cos(yaw), -sin(pitch) };
            if (direction.Unitize() <= std::numeric_limits<float>::epsilon()) {
                return false;
            }

            auto* shooter = Utils::ResolveActor(projectile->shooter);

            Utils::RaycastHit hit{};

			constexpr float kSurfaceOffset = 1.0f;
            RE::NiPoint3 start = impactData->location + direction * kSurfaceOffset;
            RE::NiPoint3 end = impactData->location + direction * penetrationDepth;

            logger::info(
                FMT_STRING("[Penetration] Forward ray start ({:.2f}, {:.2f}, {:.2f}) end ({:.2f}, {:.2f}, {:.2f})"),
                start.x,
                start.y,
                start.z,
                end.x,
                end.y,
                end.z);

            auto selectExitHit = [&](std::string_view label) {
                const auto hitCount = g_pickData.GetAllCollectorRayHitSize();
				Utils::RaycastHit realHit = hit;
				if (hitCount > 0 && Utils::SelectRealExit(g_pickData, impactData->location, realHit)) {
					hit = realHit;
                }

                logger::info(
                    FMT_STRING("[Penetration] {} ray hit count {} exit ({:.2f}, {:.2f}, {:.2f})"),
                    label,
                    hitCount,
                    hit.point.x,
                    hit.point.y,
                    hit.point.z);
            };

            bool hitFound = Utils::PerformRaycast(*projectile, shooter, projectileBase, start, end, g_pickData, hit, true);
            RE::NiPoint3 exitDirection = direction;

            if (hitFound) {
                selectExitHit("Forward");
            } else {
				start = impactData->location + direction * penetrationDepth;
				end = impactData->location + direction * kSurfaceOffset;

                logger::info(
                    FMT_STRING("[Penetration] Reverse ray start ({:.2f}, {:.2f}, {:.2f}) end ({:.2f}, {:.2f}, {:.2f})"),
                    start.x,
                    start.y,
                    start.z,
                    end.x,
                    end.y,
                    end.z);

				if (!Utils::PerformRaycast(*projectile, shooter, projectileBase, start, end, g_pickData, hit, true)) {
					logger::info(FMT_STRING("[Penetration] Reverse ray also missed"));
                    return false;
                }
                selectExitHit("Reverse");
            }

            g_pickData.Reset();

            const float travelled = impactData->location.GetDistance(hit.point);
            if (travelled <= std::numeric_limits<float>::epsilon()) {
                logger::info(FMT_STRING("[Penetration] Hit point too close to impact location"));
                return false;
			} else if (travelled > penetrationDepth) {
				logger::info(FMT_STRING("[Penetration] Hit point farther than penetration depth?"));
				return false;
			}

            const float depthDenominator = penetrationDepth > std::numeric_limits<float>::epsilon() ? penetrationDepth : travelled;
            const float travelRatio = depthDenominator > std::numeric_limits<float>::epsilon() ? travelled / depthDenominator : 1.0f;
            const float remainingPower = projectile->power * std::clamp(1.0f - travelRatio, 0.0f, 1.0f);
            if (remainingPower <= std::numeric_limits<float>::epsilon()) {
                logger::info(
                    FMT_STRING("[Penetration] No power left after travelling {:.2f}/{:.2f} (power {:.2f})"),
                    travelled,
                    depthDenominator,
                    projectile->power);
                return false;
            }

            if (!SpawnPenetratedProjectile(*projectile, hit, exitDirection, remainingPower)) {
                logger::info(FMT_STRING("[Penetration] Failed to spawn penetrated projectile"));
                return false;
            }
            return true;
        }

        bool ProjectileProcessImpactsHook(RE::Projectile* projectile)
		{
			ApplyPendingShooter(projectile);
			TryHandlePenetration(projectile);
			auto fn = reinterpret_cast<ProjectileProcessFn>(g_projectileProcessImpactsOriginal);
			return fn ? fn(projectile) : false;
        }

        bool MissileProcessImpactsHook(RE::MissileProjectile* projectile)
		{
			ApplyPendingShooter(projectile);
            TryHandlePenetration(projectile);
            auto fn = reinterpret_cast<MissileProcessFn>(g_missileProcessImpactsOriginal);
            return fn ? fn(projectile) : false;
        }

        bool BeamProcessImpactsHook(RE::BeamProjectile* projectile)
		{
			ApplyPendingShooter(projectile);
            TryHandlePenetration(projectile);
            auto fn = reinterpret_cast<BeamProcessFn>(g_beamProcessImpactsOriginal);
            return fn ? fn(projectile) : false;
        }
    }

    void Initialize()
    {
        REL::Relocation<std::uintptr_t> projectileVtbl{ RE::Projectile::VTABLE[0] };
        g_projectileProcessImpactsOriginal = projectileVtbl.write_vfunc(0xD0, ProjectileProcessImpactsHook);

        REL::Relocation<std::uintptr_t> missileVtbl{ RE::MissileProjectile::VTABLE[0] };
        g_missileProcessImpactsOriginal = missileVtbl.write_vfunc(0xD0, MissileProcessImpactsHook);

        REL::Relocation<std::uintptr_t> beamVtbl{ RE::BeamProjectile::VTABLE[0] };
        g_beamProcessImpactsOriginal = beamVtbl.write_vfunc(0xD0, BeamProcessImpactsHook);
	}

	void ClearPendingQueue()
	{
		std::scoped_lock lock(g_pendingShootersMutex);
		g_pendingShooters.clear();
	}
}
