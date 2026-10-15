#include "tesworld/TerrainMaterial.h"

#include "tesworld/Profiling.h"

#include <map>
#include <mutex>

#include <oneapi/tbb/parallel_for_each.h>

namespace tesworld::texture
{
	namespace
	{
		std::string TerrainTexturePath(std::string_view a_path)
		{
			auto normalized = vfs::NormalizePath(a_path);
			if (!normalized.empty() && !normalized.starts_with("textures\\")) {
				normalized = "textures\\" + normalized;
			}
			return normalized;
		}

		// Paths SARP won't use for close terrain: terrain LOD textures, or its own placeholder for the default layer.
		bool IsTerrainLodTexturePath(std::string_view a_path)
		{
			return a_path.starts_with("textures\\terrain\\") || a_path.starts_with("terrain\\") || a_path.find("\\terrain\\lod\\") != std::string_view::npos ||
			       a_path.find("\\lod\\") != std::string_view::npos;
		}

		bool IsDefaultFallbackTexturePath(std::string_view a_path)
		{
			return a_path == "textures\\_land_default.dds";
		}

		struct Wanted
		{
			bool normal{ false };
			std::shared_ptr<const MipChain> chain;
			std::shared_ptr<const StochasticTexture> stochastic;
			std::string error;
		};
	}

	std::vector<TerrainLayerTextures> LoadTerrainLayers(
		const TerrainWorldBuildResult& a_terrain,
		const vfs::FileSystem& a_files,
		const pbr::Database* a_pbr,
		const TerrainLayerLoadOptions& a_options,
		std::vector<std::string>* a_warnings)
	{
		TW_ZONE("LoadTerrainLayers");
		std::vector<TerrainLayerTextures> layers(a_terrain.layers.size());
		struct Paths
		{
			std::string diffuse;
			std::string normal;
			std::string rmaos;
		};
		std::vector<Paths> paths(a_terrain.layers.size());
		std::map<std::string, Wanted> textures;
		const auto warn = [&](std::string a_message) {
			if (a_warnings) a_warnings->push_back(std::move(a_message));
		};

		for (std::size_t i = 0; i < a_terrain.layers.size(); ++i) {
			const auto& source = a_terrain.layers[i];
			auto& layer = layers[i];
			layer.flags = source.materialFlags & kTerrainLayerSnow;
			layer.uvScale = source.uvScale;
			layer.fallbackColor = { source.fallbackColor[0], source.fallbackColor[1], source.fallbackColor[2], source.fallbackColor[3] };

			const auto sourceDiffuse = TerrainTexturePath(source.diffusePath);
			auto diffuse = sourceDiffuse;
			auto normal = TerrainTexturePath(source.normalPath);
			auto rmaos = TerrainTexturePath(source.rmaosPath);
			if (const auto* set = a_pbr ? a_pbr->FindTextureSet(source.textureSetEditorID) : nullptr) {
				layer.flags |= kTerrainLayerPbr;
				layer.roughnessScale = set->roughnessScale;
				if (set->glintParameters.enabled) layer.flags |= kTerrainLayerGlint;
				diffuse = pbr::PreferDerivedTexturePath(a_files, diffuse, pbr::DerivedTextureKind::BaseColor);
				const auto derivedNormal = pbr::DerivedTexturePath(sourceDiffuse, pbr::DerivedTextureKind::Normal);
				if (!derivedNormal.empty() && a_files.Exists(derivedNormal)) normal = derivedNormal;
				if (rmaos.empty()) {
					const auto derivedRmaos = pbr::DerivedTexturePath(sourceDiffuse, pbr::DerivedTextureKind::RMAOS);
					if (!derivedRmaos.empty() && a_files.Exists(derivedRmaos)) rmaos = derivedRmaos;
				}
			}
			if (!diffuse.empty() && !IsDefaultFallbackTexturePath(diffuse)) {
				if (IsTerrainLodTexturePath(diffuse)) {
					warn("ignoring terrain-LOD-looking diffuse texture '" + diffuse + "' of LTEX " + source.landTextureFormID.ToString());
				} else {
					paths[i].diffuse = diffuse;
					textures.emplace(diffuse, Wanted{});
				}
			}
			if (!normal.empty()) {
				if (IsTerrainLodTexturePath(normal)) {
					warn("ignoring terrain-LOD-looking normal texture '" + normal + "' of LTEX " + source.landTextureFormID.ToString());
				} else {
					paths[i].normal = normal;
					textures.emplace(normal, Wanted{ .normal = true });
				}
			}
			if (a_options.loadRmaos && (layer.flags & kTerrainLayerPbr) != 0 && !rmaos.empty() && !IsTerrainLodTexturePath(rmaos)) {
				paths[i].rmaos = rmaos;
				textures.emplace(rmaos, Wanted{});
			}
		}

		// Each distinct texture once, in parallel: many LTEX share their textures.
		std::vector<std::pair<const std::string, Wanted>*> work;
		work.reserve(textures.size());
		for (auto& entry : textures) work.push_back(&entry);
		oneapi::tbb::parallel_for_each(work.begin(), work.end(), [&](std::pair<const std::string, Wanted>* a_entry) {
			TW_ZONE_TEXT("terrain layer texture", "{}", a_entry->first);
			auto& wanted = a_entry->second;
			if (a_options.cache) {
				if (auto cached = a_options.cache->Find(a_entry->first, wanted.normal)) {
					wanted.chain = std::move(cached->chain);
					wanted.stochastic = std::move(cached->stochastic);
					wanted.error = std::move(cached->error);
					return;
				}
			}
			const auto store = [&] {
				if (a_options.cache) a_options.cache->Store(a_entry->first, wanted.normal, { wanted.chain, wanted.stochastic, wanted.error });
			};
			const auto bytes = a_files.Read(a_entry->first);
			if (!bytes) {
				wanted.error = "missing";
				store();
				return;
			}
			std::string error;
			auto chain = DecodeDds(*bytes, DecodeOptions{ .maxSize = a_options.maxTextureSize, .flipGreen = wanted.normal }, &error);
			if (!chain) {
				wanted.error = error;
				store();
				return;
			}
			if (a_options.stochastic && a_options.gaussian) {
				wanted.stochastic = std::make_shared<const StochasticTexture>(
					BuildStochasticTexture(*chain, wanted.normal ? StochasticMode::NormalXY : StochasticMode::DecorrelatedColor, a_options.lutWidth));
			}
			wanted.chain = std::make_shared<const MipChain>(std::move(*chain));
			store();
		});

		for (std::size_t i = 0; i < layers.size(); ++i) {
			const auto take = [&](const std::string& a_path, std::shared_ptr<const MipChain>& a_chain, std::shared_ptr<const StochasticTexture>* a_stochastic) {
				if (a_path.empty()) return;
				const auto& wanted = textures.at(a_path);
				if (!wanted.chain) {
					warn("terrain texture '" + a_path + "' of LTEX " + a_terrain.layers[i].landTextureFormID.ToString() + " not used: " + wanted.error);
					return;
				}
				a_chain = wanted.chain;
				if (a_stochastic) *a_stochastic = wanted.stochastic;
			};
			take(paths[i].diffuse, layers[i].diffuse, &layers[i].diffuseStochastic);
			take(paths[i].normal, layers[i].normal, &layers[i].normalStochastic);
			take(paths[i].rmaos, layers[i].rmaos, nullptr);
		}
		return layers;
	}
}
