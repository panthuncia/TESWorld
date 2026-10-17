// The tests check with assert, so it stays on in every configuration.
#undef NDEBUG

#include "tesworld/Plugins.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <utility>
#include <vector>

int main()
{
	using namespace tesworld;

	assert(FourCCString(MakeFourCC('T', 'E', 'S', '4')) == "TES4");
	assert(FourCCFromString("REFR") == MakeFourCC('R', 'E', 'F', 'R'));
	assert(NormalizeModelPath(" Architecture/Whiterun/WRWall.nif ") == "meshes\\architecture\\whiterun\\wrwall.nif");
	assert(NormalizeModelPath("\\meshes/Actors/Test.NIF\0 ") == "meshes\\actors\\test.nif");
	assert(NormalizeModelPath("textures\\not-a-model.dds").empty());
	assert(NormalizeModelPath("   ").empty());

	const auto loadOrderTestDirectory = std::filesystem::temp_directory_path() / "tesworld_plugins_txt_test";
	std::filesystem::create_directories(loadOrderTestDirectory);
	std::ofstream(loadOrderTestDirectory / "enabled.esp", std::ios::binary).put('\0');
	std::ofstream(loadOrderTestDirectory / "disabled.esp", std::ios::binary).put('\0');
	const auto pluginsTxt = loadOrderTestDirectory / "plugins.txt";
	{
		std::ofstream output(pluginsTxt);
		output << "# This file is used by Skyrim to keep track of your downloaded content.\n"
		       << "disabled.esp\n"
		       << "*enabled.esp\n";
	}
	const auto pluginLoadOrder = PluginsTxtLoadOrderSource(loadOrderTestDirectory, pluginsTxt).Load();
	assert(pluginLoadOrder.size() == 1);
	assert(pluginLoadOrder.front().pluginName == "enabled.esp");
	std::filesystem::remove_all(loadOrderTestDirectory);

	const FileID full{ .kind = ModuleKind::Full, .slot = 5 };
	assert(full.BaseFormID() == 0x05000000u);
	assert(full.ObjectMask() == 0x00FFFFFFu);

	const FileID light{ .kind = ModuleKind::Light, .slot = 0x123 };
	assert(light.BaseFormID() == 0xFE123000u);
	assert(light.ObjectMask() == 0x00000FFFu);

	assert(FormID{ 0 }.IsNull());
	assert(FormID{ 0xFFFFFFFFu }.IsNone());
	assert(FormID{ 0x14u }.IsPlayer());
	assert(FormID{ 0x7FFu }.IsHardcoded());
	assert(FormID{ 0xFE123ABCu }.GetFileID() == light);
	assert(FormID{ 0xFE123ABCu }.ObjectID() == 0xABCu);
	assert(FormID{ 0x05123456u }.GetFileID() == full);
	assert(FormID{ 0x05123456u }.ObjectID() == 0x123456u);

	OverrideChain chain;
	RawRecord first;
	first.sourceFileIndex = 1;
	RawRecord second;
	second.sourceFileIndex = 3;
	chain.firstRecord = &first;
	chain.overrides = { &second };
	assert(chain.Winning() == &second);
	assert(chain.VisibleFrom(0) == nullptr);
	assert(chain.VisibleFrom(1) == &first);
	assert(chain.VisibleFrom(2) == &first);
	assert(chain.VisibleFrom(3) == &second);

	StaticWorldSnapshot snapshot;
	const FormID world{ 0x3C };
	const FormID exteriorCell{ 0x01001234 };
	const FormID interiorCell{ 0x01005678 };
	const FormID staticBase{ 0x01000001 };
	const FormID treeBase{ 0x01000002 };
	const FormID movableBase{ 0x01000003 };
	snapshot.baseObjectsByFormID.emplace(staticBase, BaseObjectInfo{ .formID = staticBase, .kind = BaseObjectKind::Static, .modelPath = "meshes\\a.nif" });
	snapshot.baseObjectsByFormID.emplace(treeBase, BaseObjectInfo{ .formID = treeBase, .kind = BaseObjectKind::Tree, .modelPath = "meshes\\tree.nif" });
	snapshot.baseObjectsByFormID.emplace(movableBase, BaseObjectInfo{ .formID = movableBase, .kind = BaseObjectKind::MovableStatic, .modelPath = "meshes\\movable.nif" });
	snapshot.exteriorPlacementsByCell[CellKey{ world, 4, -7 }] = {
		PlacementInfo{ .formID = FormID{ 0x01010001 }, .parentCell = exteriorCell, .worldFormID = world, .baseFormID = staticBase },
		PlacementInfo{ .formID = FormID{ 0x01010002 }, .parentCell = exteriorCell, .worldFormID = world, .baseFormID = movableBase }
	};
	snapshot.interiorPlacementsByCell[interiorCell] = {
		PlacementInfo{ .formID = FormID{ 0x01020001 }, .parentCell = interiorCell, .baseFormID = treeBase }
	};
	assert(snapshot.GetExteriorCell(world, 4, -7)->size() == 2);
	assert(snapshot.GetExteriorCell(world, 0, 0) == nullptr);
	assert(snapshot.GetInteriorCell(interiorCell)->size() == 1);
	assert(snapshot.GetInteriorCell(FormID{ 0x01009999 }) == nullptr);
	assert(snapshot.baseObjectsByFormID.at(staticBase).kind == BaseObjectKind::Static);
	assert(snapshot.baseObjectsByFormID.at(treeBase).kind == BaseObjectKind::Tree);
	assert(snapshot.baseObjectsByFormID.at(movableBase).kind == BaseObjectKind::MovableStatic);

	StaticPluginShard persistentExteriorShard;
	const FormID persistentExteriorCell{ 0x0100A000 };
	persistentExteriorShard.baseObjects.push_back(BaseObjectInfo{ .formID = staticBase, .kind = BaseObjectKind::Static, .modelPath = "meshes\\a.nif" });
	persistentExteriorShard.cells.push_back(CellInfo{
		.formID = persistentExteriorCell,
		.worldFormID = world,
		.cellFlags = 0
	});
	PlacementInfo persistentPlacement;
	persistentPlacement.formID = FormID{ 0x01010003 };
	persistentPlacement.parentCell = persistentExteriorCell;
	persistentPlacement.baseFormID = staticBase;
	persistentPlacement.position[0] = 4.0f * kSkyrimTerrainCellSize + 10.0f;
	persistentPlacement.position[1] = -7.0f * kSkyrimTerrainCellSize + 10.0f;
	persistentExteriorShard.placements.push_back(persistentPlacement);
	const auto persistentSnapshot = BuildStaticWorldSnapshot(
		std::vector<StaticPluginShard>{ persistentExteriorShard },
		StaticWorldSnapshotBuildOptions{ .parallelPlacementBucketing = false });
	const auto* persistentCellPlacements = persistentSnapshot.GetExteriorCell(world, 4, -7);
	assert(persistentCellPlacements != nullptr);
	assert(persistentCellPlacements->size() == 1);
	assert(persistentSnapshot.GetInteriorCell(persistentExteriorCell) == nullptr);
	for (const bool parallel : { false, true }) {
		auto disabledShard = persistentExteriorShard;
		disabledShard.placements[0].flags |= 1u << 11;
		const auto filtered = BuildStaticWorldSnapshot({ disabledShard }, { .parallelPlacementBucketing = parallel });
		assert(filtered.GetExteriorCell(world, 4, -7) == nullptr);
		const auto retained = BuildStaticWorldSnapshot({ disabledShard }, { .parallelPlacementBucketing = parallel, .includeDisabledPlacements = true });
		const auto* candidates = retained.GetExteriorCell(world, 4, -7);
		assert(candidates && candidates->size() == 1 && candidates->front().IsInitiallyDisabled());
		assert(retained.buildStats.skippedDisabledPlacements == 0);
	}

	// A partial form (as SR Exterior Cities overrides Tamriel and its cells with only their editor IDs) changes only the
	// fields it has; a whole override replaces every one.
	{
		const FormID partialCell{ 0x0100B000 };
		StaticPluginShard master;
		master.worlds.push_back(WorldInfo{ .formID = world, .editorID = "Tamriel", .parentUseFlags = 0, .dataFlags = 0, .defaultLandHeight = -27000.0f,
			.defaultWaterHeight = -14000.0f, .lodWaterHeight = -14000.0f,
			.fields = WorldInfo::FieldEditorID | WorldInfo::FieldHeights | WorldInfo::FieldLodWaterHeight });
		master.cells.push_back(CellInfo{ .formID = partialCell, .worldFormID = world, .editorID = "Coast", .cellFlags = 0x2, .gridX = 3, .gridY = -2, .forceHideLand = 0x80000005u,
			.waterHeight = -13670.0f, .fields = CellInfo::FieldEditorID | CellInfo::FieldData | CellInfo::FieldGrid | CellInfo::FieldWater });
		StaticPluginShard partial;
		partial.worlds.push_back(WorldInfo{ .formID = world, .flags = RecordFlagPartialForm, .editorID = "Tamriel", .fields = WorldInfo::FieldEditorID });
		partial.cells.push_back(CellInfo{ .formID = partialCell, .worldFormID = world, .editorID = "CoastRenamed", .flags = RecordFlagPartialForm,
			.fields = CellInfo::FieldEditorID });
		const auto merged = BuildStaticWorldSnapshot(std::vector<StaticPluginShard>{ master, partial }, StaticWorldSnapshotBuildOptions{ .parallelPlacementBucketing = false });
		const auto& tamriel = merged.worldsByFormID.at(world);
		assert(tamriel.defaultWaterHeight == -14000.0f && tamriel.defaultLandHeight == -27000.0f && tamriel.lodWaterHeight == -14000.0f);
		const auto& coast = merged.cellsByFormID.at(partialCell);
		assert(coast.editorID == "CoastRenamed" && coast.gridX == 3 && coast.gridY == -2 && coast.waterHeight == -13670.0f && coast.cellFlags == 0x2);
		assert(coast.forceHideLand == 0x80000005u);

		// The same overrides, whole: nothing below them shows.
		partial.worlds.front().flags = 0;
		partial.cells.front().flags = 0;
		const auto replaced = BuildStaticWorldSnapshot(std::vector<StaticPluginShard>{ master, partial }, StaticWorldSnapshotBuildOptions{ .parallelPlacementBucketing = false });
		assert(!replaced.worldsByFormID.at(world).defaultWaterHeight && !replaced.cellsByFormID.at(partialCell).gridX);
	}

	auto appendBytes = [](std::vector<std::uint8_t>& bytes, const void* data, std::size_t size) {
		const auto* first = static_cast<const std::uint8_t*>(data);
		bytes.insert(bytes.end(), first, first + size);
	};
	auto appendU16 = [&](std::vector<std::uint8_t>& bytes, std::uint16_t value) {
		appendBytes(bytes, &value, sizeof(value));
	};
	auto appendU32 = [&](std::vector<std::uint8_t>& bytes, std::uint32_t value) {
		appendBytes(bytes, &value, sizeof(value));
	};
	auto appendFloat = [&](std::vector<std::uint8_t>& bytes, float value) {
		appendBytes(bytes, &value, sizeof(value));
	};
	auto appendStringPayload = [&](std::vector<std::uint8_t>& bytes, std::string_view value) {
		appendBytes(bytes, value.data(), value.size());
		const char nul = '\0';
		appendBytes(bytes, &nul, 1);
	};
	auto appendSubrecord = [&](RawRecord& record, FourCC signature, const std::vector<std::uint8_t>& payload) {
		appendU32(record.ownedData, signature);
		appendU16(record.ownedData, static_cast<std::uint16_t>(payload.size()));
		RawSubrecord subrecord;
		subrecord.signature = signature;
		subrecord.size = static_cast<std::uint32_t>(payload.size());
		subrecord.dataOffset = static_cast<std::uint32_t>(record.ownedData.size());
		record.ownedData.insert(record.ownedData.end(), payload.begin(), payload.end());
		record.subrecords.push_back(subrecord);
		record.dataSize = static_cast<std::uint32_t>(record.ownedData.size());
		record.dataPtr = record.ownedData.data();
	};
	auto stringPayload = [&](std::string_view value) {
		std::vector<std::uint8_t> payload;
		appendStringPayload(payload, value);
		return payload;
	};

	RawRecord textureSetRecord;
	textureSetRecord.signature = MakeFourCC('T', 'X', 'S', 'T');
	textureSetRecord.loadOrderFormID = FormID{ 0x01000042 };
	appendSubrecord(textureSetRecord, MakeFourCC('E', 'D', 'I', 'D'), stringPayload("TestTextureSet"));
	appendSubrecord(textureSetRecord, MakeFourCC('T', 'X', '0', '0'), stringPayload("textures\\test\\diffuse.dds"));
	appendSubrecord(textureSetRecord, MakeFourCC('T', 'X', '0', '1'), stringPayload("textures\\test\\normal.dds"));
	appendSubrecord(textureSetRecord, MakeFourCC('T', 'X', '0', '3'), stringPayload("textures\\test\\glow.dds"));
	appendSubrecord(textureSetRecord, MakeFourCC('T', 'X', '0', '4'), stringPayload("textures\\test\\height.dds"));
	std::vector<std::uint8_t> dnam;
	appendU16(dnam, 0x0004u);
	appendSubrecord(textureSetRecord, MakeFourCC('D', 'N', 'A', 'M'), dnam);

	RawRecord staticRecord;
	staticRecord.signature = MakeFourCC('S', 'T', 'A', 'T');
	staticRecord.loadOrderFormID = FormID{ 0x01000024 };
	staticRecord.sourceFileID = FileID{ .kind = ModuleKind::Full, .slot = 1 };
	appendSubrecord(staticRecord, MakeFourCC('E', 'D', 'I', 'D'), stringPayload("TestStatic"));
	appendSubrecord(staticRecord, MakeFourCC('M', 'O', 'D', 'L'), stringPayload("meshes\\test\\static.nif"));
	std::vector<std::uint8_t> mods;
	appendU32(mods, 1);
	const std::string name3D = "TestShape";
	appendU32(mods, static_cast<std::uint32_t>(name3D.size()));
	appendBytes(mods, name3D.data(), name3D.size());
	appendU32(mods, 0x01000042u);
	appendU32(mods, 7);
	appendSubrecord(staticRecord, MakeFourCC('M', 'O', 'D', 'S'), mods);

	ResolvedRecordStore resolved;
	OverrideChain textureChain;
	textureChain.formID = textureSetRecord.loadOrderFormID;
	textureChain.firstRecord = &textureSetRecord;
	resolved.chains.emplace(textureSetRecord.loadOrderFormID, textureChain);
	OverrideChain staticChain;
	staticChain.formID = staticRecord.loadOrderFormID;
	staticChain.firstRecord = &staticRecord;
	resolved.chains.emplace(staticRecord.loadOrderFormID, staticChain);
	PluginFile sourcePlugin;
	sourcePlugin.entry.pluginName = "Source.esp";
	const auto modelPaths = BuildModelPathIndex({ sourcePlugin }, resolved);
	assert(modelPaths.references.size() == 1);
	assert(modelPaths.references.front().normalizedPath == "meshes\\test\\static.nif");
	assert(modelPaths.references.front().sourcePlugin == "Source.esp");
	assert(modelPaths.uniqueNormalizedPaths == std::vector<std::string>{ "meshes\\test\\static.nif" });
	const auto worldIndex = BuildWorldIndex(resolved);
	assert(worldIndex.textureSets.at(textureSetRecord.loadOrderFormID).texturePaths[0] == "textures\\test\\diffuse.dds");
	assert(worldIndex.textureSets.at(textureSetRecord.loadOrderFormID).texturePaths[1] == "textures\\test\\normal.dds");
	assert(worldIndex.textureSets.at(textureSetRecord.loadOrderFormID).texturePaths[3] == "textures\\test\\glow.dds");
	assert(worldIndex.textureSets.at(textureSetRecord.loadOrderFormID).texturePaths[4] == "textures\\test\\height.dds");
	assert(worldIndex.textureSets.at(textureSetRecord.loadOrderFormID).dnamFlags == 0x0004u);
	const auto& alternateTextures = worldIndex.baseObjects.at(staticRecord.loadOrderFormID).alternateTextures;
	assert(alternateTextures.size() == 1);
	assert(alternateTextures.front().name3D == "TestShape");
	assert(alternateTextures.front().textureSetFormID == textureSetRecord.loadOrderFormID);
	assert(alternateTextures.front().index3D == 7);

	// A static collection's parts: each ONAM, then the DATA of its placements; and flora as a base of its own.
	RawRecord collectionRecord;
	collectionRecord.signature = MakeFourCC('S', 'C', 'O', 'L');
	collectionRecord.loadOrderFormID = FormID{ 0x01000070 };
	collectionRecord.sourceFileID = FileID{ .kind = ModuleKind::Full, .slot = 1 };
	appendSubrecord(collectionRecord, MakeFourCC('E', 'D', 'I', 'D'), stringPayload("TestCollection"));
	auto partPayload = [&](std::uint32_t base) {
		std::vector<std::uint8_t> payload;
		appendU32(payload, base);
		return payload;
	};
	auto placementsPayload = [&](std::initializer_list<float> values) {
		std::vector<std::uint8_t> payload;
		for (const auto value : values) appendFloat(payload, value);
		return payload;
	};
	appendSubrecord(collectionRecord, MakeFourCC('O', 'N', 'A', 'M'), partPayload(0x01000024u));
	appendSubrecord(collectionRecord, MakeFourCC('D', 'A', 'T', 'A'),
		placementsPayload({ 1, 2, 3, 0, 0, 0.5f, 1, 10, 20, 30, 0, 0, 0, 2 }));
	appendSubrecord(collectionRecord, MakeFourCC('O', 'N', 'A', 'M'), partPayload(0x01000025u));
	appendSubrecord(collectionRecord, MakeFourCC('D', 'A', 'T', 'A'), placementsPayload({ -5, 0, 0, 0, 0, 0, 1 }));
	RawRecord floraRecord;
	floraRecord.signature = MakeFourCC('F', 'L', 'O', 'R');
	floraRecord.loadOrderFormID = FormID{ 0x01000071 };
	appendSubrecord(floraRecord, MakeFourCC('M', 'O', 'D', 'L'), stringPayload("meshes\\test\\flora.nif"));
	ResolvedRecordStore collectionResolved;
	for (RawRecord* record : { &collectionRecord, &floraRecord }) {
		OverrideChain collectionChain;
		collectionChain.formID = record->loadOrderFormID;
		collectionChain.firstRecord = record;
		collectionResolved.chains.emplace(record->loadOrderFormID, collectionChain);
	}
	const auto collectionIndex = BuildWorldIndex(collectionResolved);
	const auto& collection = collectionIndex.baseObjects.at(collectionRecord.loadOrderFormID);
	assert(collection.kind == BaseObjectKind::StaticCollection);
	assert(collection.parts.size() == 2);
	assert(collection.parts[0].baseFormID == FormID{ 0x01000024 });
	assert(collection.parts[0].placements.size() == 2);
	assert(collection.parts[0].placements[0].rotation[2] == 0.5f);
	assert(collection.parts[0].placements[1].position[2] == 30.0f && collection.parts[0].placements[1].scale == 2.0f);
	assert(collection.parts[1].placements.size() == 1 && collection.parts[1].placements[0].position[0] == -5.0f);
	assert(collectionIndex.baseObjects.at(floraRecord.loadOrderFormID).kind == BaseObjectKind::Flora);

	std::vector<std::uint8_t> vhgt;
	appendFloat(vhgt, 0.0f);
	vhgt.resize(sizeof(float) + LandInfo::VertexCount + 3, 0);
	vhgt[sizeof(float) + 0] = 1;
	vhgt[sizeof(float) + 1] = 1;
	const auto decodedHeights = DecodeLandHeights(vhgt);
	assert(decodedHeights[0] == 8.0f);
	assert(decodedHeights[1] == 16.0f);

	const FormID landTextureA{ 0x01000060 };
	const FormID landTextureB{ 0x01000061 };
	const FormID landTextureC{ 0x01000062 };
	const FormID landTextureD{ 0x01000063 };
	const FormID landTextureE{ 0x01000064 };
	const FormID landTextureF{ 0x01000065 };
	const FormID landTextureG{ 0x01000066 };
	RawRecord landTextureRecordA;
	landTextureRecordA.signature = MakeFourCC('L', 'T', 'E', 'X');
	landTextureRecordA.loadOrderFormID = landTextureA;
	landTextureRecordA.sourceFileID = FileID{ .kind = ModuleKind::Full, .slot = 1 };
	appendSubrecord(landTextureRecordA, MakeFourCC('T', 'N', 'A', 'M'), std::vector<std::uint8_t>{ 0x42, 0x00, 0x00, 0x01 });

	RawRecord landTextureRecordB;
	landTextureRecordB.signature = MakeFourCC('L', 'T', 'E', 'X');
	landTextureRecordB.loadOrderFormID = landTextureB;
	landTextureRecordB.sourceFileID = FileID{ .kind = ModuleKind::Full, .slot = 1 };
	appendSubrecord(landTextureRecordB, MakeFourCC('T', 'N', 'A', 'M'), std::vector<std::uint8_t>{ 0x42, 0x00, 0x00, 0x01 });
	std::vector<std::uint8_t> landTextureFlags;
	appendU32(landTextureFlags, kSkyrimLandTextureFlagSnow);
	appendSubrecord(landTextureRecordB, MakeFourCC('I', 'N', 'A', 'M'), landTextureFlags);

	RawRecord cellRecord;
	cellRecord.signature = MakeFourCC('C', 'E', 'L', 'L');
	cellRecord.loadOrderFormID = exteriorCell;
	cellRecord.parentWorld = world;
	std::vector<std::uint8_t> xclc;
	appendU32(xclc, 4);
	appendU32(xclc, static_cast<std::uint32_t>(-7));
	appendSubrecord(cellRecord, MakeFourCC('X', 'C', 'L', 'C'), xclc);

	RawRecord landRecord;
	landRecord.signature = MakeFourCC('L', 'A', 'N', 'D');
	landRecord.loadOrderFormID = FormID{ 0x01000070 };
	landRecord.parentWorld = world;
	landRecord.parentCell = exteriorCell;
	landRecord.sourceFileID = FileID{ .kind = ModuleKind::Full, .slot = 1 };
	std::vector<std::uint8_t> flatVhgt;
	appendFloat(flatVhgt, 0.0f);
	flatVhgt.resize(sizeof(float) + LandInfo::VertexCount + 3, 0);
	appendSubrecord(landRecord, MakeFourCC('V', 'H', 'G', 'T'), flatVhgt);
	std::vector<std::uint8_t> btxt;
	appendU32(btxt, landTextureA.value);
	btxt.push_back(0);
	btxt.push_back(0);
	appendU16(btxt, 0);
	appendSubrecord(landRecord, MakeFourCC('B', 'T', 'X', 'T'), btxt);
	std::vector<std::uint8_t> atxt;
	appendU32(atxt, landTextureB.value);
	atxt.push_back(0);
	atxt.push_back(0);
	appendU16(atxt, 0);
	appendSubrecord(landRecord, MakeFourCC('A', 'T', 'X', 'T'), atxt);
	std::vector<std::uint8_t> vtxt;
	appendU16(vtxt, 0);
	vtxt.push_back(0);
	vtxt.push_back(0);
	appendFloat(vtxt, 0.25f);
	appendU16(vtxt, 1);
	vtxt.push_back(0);
	vtxt.push_back(0);
	appendFloat(vtxt, 0.25f);
	appendSubrecord(landRecord, MakeFourCC('V', 'T', 'X', 'T'), vtxt);
	for (const auto& [formID, layerIndex] : {
		     std::pair{ landTextureC, static_cast<std::uint16_t>(1) },
		     std::pair{ landTextureD, static_cast<std::uint16_t>(2) },
		     std::pair{ landTextureE, static_cast<std::uint16_t>(3) },
		     std::pair{ landTextureF, static_cast<std::uint16_t>(4) },
		     std::pair{ landTextureG, static_cast<std::uint16_t>(5) },
	     }) {
		std::vector<std::uint8_t> extraAtxt;
		appendU32(extraAtxt, formID.value);
		extraAtxt.push_back(0);
		extraAtxt.push_back(0);
		appendU16(extraAtxt, layerIndex);
		appendSubrecord(landRecord, MakeFourCC('A', 'T', 'X', 'T'), extraAtxt);
	}

	ResolvedRecordStore terrainResolved;
	for (RawRecord* record : { &textureSetRecord, &landTextureRecordA, &landTextureRecordB, &cellRecord, &landRecord }) {
		OverrideChain terrainChain;
		terrainChain.formID = record->loadOrderFormID;
		terrainChain.firstRecord = record;
		terrainResolved.chains.emplace(record->loadOrderFormID, terrainChain);
	}
	const auto terrainIndex = BuildWorldIndex(terrainResolved);
	assert(terrainIndex.landTextures.at(landTextureA).textureSetFormID == textureSetRecord.loadOrderFormID);
	assert(terrainIndex.landTextures.at(landTextureB).IsSnow());
	assert(terrainIndex.landsByWorldspace.at(world).size() == 1);
	assert(*terrainIndex.landsByWorldspace.at(world).front().cellX == 4);
	assert(*terrainIndex.landsByWorldspace.at(world).front().cellY == -7);

	const auto terrain = BuildTerrainWorld(world, terrainIndex);
	assert(terrain.vertices.size() == LandInfo::VertexCount);
	assert(terrain.indices.size() == 32u * 32u * 6u);
	assert(terrain.regions.size() == 4);
	const auto q0 = std::find_if(terrain.regions.begin(), terrain.regions.end(), [](const TerrainRegionDescriptor& descriptor) {
		return descriptor.regionX == 8 && descriptor.regionY == -14;
	});
	assert(q0 != terrain.regions.end());
	// The base, the painted snow layer and the default texture the missing neighbours fall back to. Layers C to G have
	// no vertex weights here, so the build drops them (the expectation SARP's test had was from before it did; its
	// asserts were compiled out, so the test kept passing).
	assert(q0->layerRefCount == 3);
	assert(q0->weightSampleSide == 19);
	assert(q0->layerRefStart + q0->layerRefCount <= terrain.layerRefs.size());
	assert(q0->weightBlockStart < terrain.weightBlocks.size());
	const auto q0RefsBegin = terrain.layerRefs.begin() + static_cast<std::ptrdiff_t>(q0->layerRefStart);
	const auto q0RefsEnd = q0RefsBegin + static_cast<std::ptrdiff_t>(q0->layerRefCount);
	assert(std::ranges::any_of(q0RefsBegin, q0RefsEnd, [&](const TerrainLayerRef& layerRef) {
		return layerRef.layerIndex < terrain.layers.size() && terrain.layers[layerRef.layerIndex].landTextureFormID == landTextureA;
	}));
	assert(std::ranges::any_of(q0RefsBegin, q0RefsEnd, [&](const TerrainLayerRef& layerRef) {
		return layerRef.layerIndex < terrain.layers.size() && terrain.layers[layerRef.layerIndex].landTextureFormID == landTextureB;
	}));
	assert(std::ranges::none_of(q0RefsBegin, q0RefsEnd, [&](const TerrainLayerRef& layerRef) {
		return layerRef.layerIndex < terrain.layers.size() && terrain.layers[layerRef.layerIndex].landTextureFormID == landTextureF;
	}));
	assert(std::ranges::none_of(terrain.layers, [&](const TerrainLayer& layer) {
		return layer.landTextureFormID == landTextureG;
	}));
	const auto snowLayer = std::ranges::find_if(terrain.layers, [&](const TerrainLayer& layer) {
		return layer.landTextureFormID == landTextureB;
	});
	assert(snowLayer != terrain.layers.end());
	assert((snowLayer->materialFlags & kSkyrimLandTextureFlagSnow) != 0);
	const auto unpackWeight = [&](std::uint32_t localLayer, std::uint32_t sampleIndex) {
		const auto wordsPerLayer = (q0->weightSampleSide * q0->weightSampleSide + 3u) / 4u;
		const auto word = terrain.weightBlocks[q0->weightBlockStart + localLayer * wordsPerLayer + sampleIndex / 4u];
		return static_cast<std::uint8_t>((word >> ((sampleIndex & 3u) * 8u)) & 0xFFu);
	};
	const auto layerRefLocalIndex = [&](FormID landTextureFormID) {
		for (std::uint32_t localLayer = 0; localLayer < q0->layerRefCount; ++localLayer) {
			const auto layerIndex = terrain.layerRefs[q0->layerRefStart + localLayer].layerIndex;
			if (layerIndex < terrain.layers.size() && terrain.layers[layerIndex].landTextureFormID == landTextureFormID) {
				return localLayer;
			}
		}
		return std::numeric_limits<std::uint32_t>::max();
	};
	const auto baseLocalLayer = layerRefLocalIndex(landTextureA);
	const auto snowLocalLayer = layerRefLocalIndex(landTextureB);
	assert(baseLocalLayer != std::numeric_limits<std::uint32_t>::max());
	assert(snowLocalLayer != std::numeric_limits<std::uint32_t>::max());
	const auto interiorSample = 1u * q0->weightSampleSide + 1u;
	assert(unpackWeight(baseLocalLayer, interiorSample) == 191);
	assert(unpackWeight(snowLocalLayer, interiorSample) == 64);
	const auto missingNeighborGutterSample = 1u * q0->weightSampleSide;
	assert(unpackWeight(snowLocalLayer, missingNeighborGutterSample) == 0);

	std::cout << "tesworld plugins tests passed\n";
	return 0;
}