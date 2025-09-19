#include "PenetrationConfig.h"

#include "Utils.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <system_error>

#include <SimpleIni.h>

#include <RE/Bethesda/TESDataHandler.h>
#include <RE/Bethesda/TESForms.h>

namespace Penetration
{
	namespace
	{
		constexpr std::string_view kAmmoSection{ "AmmoMult" };
		constexpr std::string_view kMaterialSection{ "MaterialMult" };

		std::unordered_map<const RE::TESAmmo*, float> g_penetrationByAmmo;
		std::unordered_map<const RE::BGSMaterialType*, float> g_penetrationByMaterial;

		bool TryParseFormID(std::string_view value, std::uint32_t& outFormID)
		{
			std::string trimmed = Utils::Trim(value);
			if (trimmed.empty()) {
				return false;
			}

			if (trimmed.rfind("0x", 0) == 0 || trimmed.rfind("0X", 0) == 0) {
				trimmed.erase(0, 2);
			}

			std::uint32_t parsed = 0;
			auto [ptr, ec] = std::from_chars(trimmed.data(), trimmed.data() + trimmed.size(), parsed, 16);
			if (ec != std::errc{} || ptr != trimmed.data() + trimmed.size()) {
				return false;
			}

			outFormID = parsed & 0xFFFFFF;
			return true;
		}

		bool TryParseFloat(std::string_view value, float& outValue)
		{
			std::string trimmed = Utils::Trim(value);
			if (trimmed.empty()) {
				return false;
			}

			char* endPtr = nullptr;
			outValue = std::strtof(trimmed.c_str(), &endPtr);
			return endPtr == trimmed.c_str() + trimmed.size() && std::isfinite(outValue);
		}

		void LoadFile(const std::filesystem::path& path, RE::TESDataHandler& dataHandler)
		{
			CSimpleIniA ini(true, false, false);
			if (ini.LoadFile(path.string().c_str()) < 0) {
				logger::warn("Failed to load penetration config: {}", path.string());
				return;
			}

			CSimpleIniA::TNamesDepend ammoKeys;
			ini.GetAllKeys(kAmmoSection.data(), ammoKeys);
			ammoKeys.sort(CSimpleIniA::Entry::LoadOrder());

			for (const auto& entry : ammoKeys) {
				const char* key = entry.pItem;
				if (!key) {
					continue;
				}

				const char* value = ini.GetValue(kAmmoSection.data(), key);
				if (!value) {
					continue;
				}

				std::string remainder;
				std::string pluginName = Utils::Trim(Utils::SplitString(key, "|", remainder));
				remainder = Utils::Trim(remainder);

				if (pluginName.empty() || remainder.empty()) {
					logger::warn("Invalid penetration config key '{}' in {}", key, path.string());
					continue;
				}

				std::uint32_t formID = 0;
				if (!TryParseFormID(remainder, formID)) {
					logger::warn("Invalid form ID '{}' in {}", remainder, path.string());
					continue;
				}

				float multiplier = 0.0f;
				if (!TryParseFloat(value, multiplier)) {
					logger::warn("Invalid multiplier '{}' for {} in {}", value, key, path.string());
					continue;
				}

				auto* ammo = dataHandler.LookupForm<RE::TESAmmo>(formID, pluginName);
				if (!ammo) {
					logger::warn("Unable to resolve ammo {}|{:06X} in {}", pluginName, formID, path.string());
					continue;
				}

				g_penetrationByAmmo[ammo] = multiplier;
			}

			std::unordered_map<std::string, float> materialOverrides;
			CSimpleIniA::TNamesDepend materialKeys;
			ini.GetAllKeys(kMaterialSection.data(), materialKeys);
			materialKeys.sort(CSimpleIniA::Entry::LoadOrder());

			for (const auto& entry : materialKeys) {
				const char* key = entry.pItem;
				if (!key) {
					continue;
				}

				const char* value = ini.GetValue(kMaterialSection.data(), key);
				if (!value) {
					continue;
				}

				float multiplier = 0.0f;
				if (!TryParseFloat(value, multiplier)) {
					logger::warn("Invalid material multiplier '{}' for {} in {}", value, key, path.string());
					continue;
				}

				std::string materialKey = Utils::Trim(key);
				if (materialKey.empty()) {
					continue;
				}

				materialOverrides[materialKey] = multiplier;
			}

			if (!materialOverrides.empty()) {
				for (auto* material : dataHandler.GetFormArray<RE::BGSMaterialType>()) {
					if (!material) {
						continue;
					}

					const char* editorID = material->GetFormEditorID();
					if (!editorID || *editorID == '\0') {
						continue;
					}

					auto it = materialOverrides.find(editorID);
					if (it != materialOverrides.end()) {
						logger::warn("Added {} mult: {:.2f}", editorID, it->second);
						g_penetrationByMaterial[material] = it->second;
					}
				}
			}
		}
	}

	void LoadConfig()
	{
		g_penetrationByAmmo.clear();
		g_penetrationByMaterial.clear();

		auto* dataHandler = RE::TESDataHandler::GetSingleton();
		if (!dataHandler) {
			logger::error("TESDataHandler not available; penetration config not loaded");
			return;
		}

		const std::filesystem::path configDirectory{ "Data\\F4SE\\Plugins\\PenetrationSystem\\" };
		if (!std::filesystem::exists(configDirectory)) {
			logger::warn("Penetration config directory does not exist: {}", configDirectory.string());
			return;
		}

		for (const auto& entry : std::filesystem::directory_iterator(configDirectory)) {
			if (!entry.is_regular_file()) {
				continue;
			}

			auto extension = entry.path().extension().string();
			std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
			if (extension != ".ini") {
				continue;
			}

			LoadFile(entry.path(), *dataHandler);
		}

		logger::info(
			FMT_STRING("Loaded penetration multipliers for {} ammunition forms and {} materials"),
			g_penetrationByAmmo.size(),
			g_penetrationByMaterial.size());
	}

	float GetPenetrationMultiplier(const RE::TESAmmo* ammo) noexcept
	{
		if (!ammo) {
			return 1.0f;
		}

		const auto it = g_penetrationByAmmo.find(ammo);
		return it != g_penetrationByAmmo.end() ? it->second : 1.0f;
	}

	float GetMaterialMultiplier(const RE::BGSMaterialType* material) noexcept
	{
		if (!material) {
			return 1.0f;
		}

		const auto it = g_penetrationByMaterial.find(material);
		return it != g_penetrationByMaterial.end() ? it->second : 1.0f;
	}
}
