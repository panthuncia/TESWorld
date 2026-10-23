#pragma once

#include <array>
#include <compare>
#include <cstdint>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace tesworld
{
	using FourCC = std::uint32_t;

	constexpr FourCC MakeFourCC(char a, char b, char c, char d)
	{
		return static_cast<FourCC>(static_cast<unsigned char>(a)) |
		       (static_cast<FourCC>(static_cast<unsigned char>(b)) << 8) |
		       (static_cast<FourCC>(static_cast<unsigned char>(c)) << 16) |
		       (static_cast<FourCC>(static_cast<unsigned char>(d)) << 24);
	}

	[[nodiscard]] std::string FourCCString(FourCC a_value);
	[[nodiscard]] FourCC FourCCFromString(std::string_view a_value);
	[[nodiscard]] std::string LowerAscii(std::string_view a_value);
	[[nodiscard]] std::string Trim(std::string_view a_value);

	inline constexpr float kSkyrimTerrainCellSize = 4096.0f;
	inline constexpr float kSkyrimDefaultLandTextureTilingMult = 3.0f;
	inline constexpr float kSkyrimLandTextureRepeatsPerTilingMult = 8.0f;
	inline constexpr float kSkyrimDefaultTerrainLayerUvScale =
		(kSkyrimDefaultLandTextureTilingMult * kSkyrimLandTextureRepeatsPerTilingMult) / kSkyrimTerrainCellSize;

	enum class ModuleKind
	{
		Full,
		Light
	};

	struct FileID
	{
		ModuleKind kind{ ModuleKind::Full };
		std::uint16_t slot{ 0 };

		[[nodiscard]] std::uint32_t BaseFormID() const;
		[[nodiscard]] std::uint32_t ObjectMask() const;
		[[nodiscard]] std::string ToString() const;

		friend bool operator==(const FileID&, const FileID&) = default;
	};

	struct FormID
	{
		std::uint32_t value{ 0 };

		[[nodiscard]] bool IsNull() const { return value == 0; }
		[[nodiscard]] bool IsNone() const { return value == 0xFFFFFFFFu; }
		[[nodiscard]] bool IsEmpty() const { return IsNull() || IsNone(); }
		[[nodiscard]] bool IsPlayer() const { return value == 0x14u; }
		[[nodiscard]] bool IsHardcoded() const { return value < 0x800u; }
		[[nodiscard]] FileID GetFileID() const;
		[[nodiscard]] std::uint32_t ObjectID() const;
		[[nodiscard]] FormID WithFileID(FileID a_fileID) const;
		[[nodiscard]] std::string ToString() const;

		friend bool operator==(const FormID&, const FormID&) = default;
		friend auto operator<=>(const FormID&, const FormID&) = default;
	};

	struct FormIDHash
	{
		std::size_t operator()(FormID a_id) const noexcept { return a_id.value; }
	};

	inline constexpr FormID kSkyrimDefaultLandTextureFormID{ 0x00000C16u };
	inline constexpr std::uint32_t kSkyrimLandTextureFlagSnow = 0x1u;

	struct LoadOrderEntry
	{
		std::filesystem::path path;
		std::string pluginName;
		ModuleKind kind{ ModuleKind::Full };
		FileID fileID{};
		std::vector<std::string> masters;
		std::uint32_t headerFlags{ 0 };
		std::uint32_t recordCount{ 0 };
	};

	class ILoadOrderSource
	{
	public:
		virtual ~ILoadOrderSource() = default;
		virtual std::vector<LoadOrderEntry> Load() = 0;
	};

	class ExplicitLoadOrderSource final : public ILoadOrderSource
	{
	public:
		explicit ExplicitLoadOrderSource(std::vector<std::filesystem::path> a_pluginPaths);
		std::vector<LoadOrderEntry> Load() override;

	private:
		std::vector<std::filesystem::path> _pluginPaths;
	};

	class PluginsTxtLoadOrderSource final : public ILoadOrderSource
	{
	public:
		PluginsTxtLoadOrderSource(std::filesystem::path a_dataPath, std::filesystem::path a_pluginsTxtPath);
		std::vector<LoadOrderEntry> Load() override;

	private:
		std::filesystem::path _dataPath;
		std::filesystem::path _pluginsTxtPath;
	};

	struct GroupPathEntry
	{
		std::int32_t type{ -1 };
		std::uint32_t label{ 0 };
		std::int16_t gridX{ 0 };
		std::int16_t gridY{ 0 };
	};

	struct RawSubrecord
	{
		FourCC signature{ 0 };
		std::uint32_t size{ 0 };
		std::uint32_t dataOffset{ 0 };
	};

	struct RawRecord
	{
		FourCC signature{ 0 };
		std::uint32_t dataSize{ 0 };
		std::uint32_t flags{ 0 };
		FormID fileFormID{};
		FormID loadOrderFormID{};
		std::uint32_t revision{ 0 };
		std::uint16_t formVersion{ 0 };
		std::uint16_t unknown{ 0 };
		std::size_t sourceFileIndex{ 0 };
		FileID sourceFileID{};
		std::shared_ptr<const std::vector<FileID>> sourceMasterFileIDs;
		std::uint64_t rawOffset{ 0 };
		std::vector<GroupPathEntry> groupPath;
		std::optional<FormID> parentWorld;
		std::optional<FormID> parentCell;
		const std::uint8_t* dataPtr{ nullptr };
		std::vector<std::uint8_t> ownedData;
		std::vector<RawSubrecord> subrecords;

		[[nodiscard]] std::span<const std::uint8_t> Data() const { return dataPtr && dataSize ? std::span<const std::uint8_t>(dataPtr, dataSize) : std::span<const std::uint8_t>(); }
		[[nodiscard]] bool IsDeleted() const { return (flags & (1u << 5)) != 0; }
		[[nodiscard]] bool IsIgnored() const { return (flags & (1u << 12)) != 0; }
	};

	struct PluginHeader
	{
		std::uint32_t flags{ 0 };
		float version{ 0.0f };
		std::uint32_t recordCount{ 0 };
		std::uint32_t nextObjectID{ 0 };
		std::string author;
		std::string description;
		std::vector<std::string> masters;
		std::vector<FormID> onamOverrides;
	};

	struct PluginFile
	{
		LoadOrderEntry entry;
		PluginHeader header;
		std::vector<std::uint8_t> bytes;
		std::vector<RawRecord> records;
		std::unordered_map<FourCC, std::vector<std::size_t>> recordsBySignature;
	};

	struct OverrideChain
	{
		FormID formID{};
		const RawRecord* firstRecord{ nullptr };
		std::vector<const RawRecord*> overrides;

		[[nodiscard]] const RawRecord* Winning() const;
		[[nodiscard]] const RawRecord* VisibleFrom(std::size_t a_sourceFileIndex) const;
	};

	struct ResolvedRecordStore
	{
		std::unordered_map<FormID, OverrideChain, FormIDHash> chains;

		[[nodiscard]] const OverrideChain* Find(FormID a_id) const;
		[[nodiscard]] const RawRecord* Winning(FormID a_id) const;
		[[nodiscard]] const RawRecord* VisibleFrom(FormID a_id, std::size_t a_sourceFileIndex) const;
	};

	enum class BaseObjectKind
	{
		Unknown,
		Static,
		MovableStatic,
		Tree,
		Activator,
		Door,
		Container,
		Furniture,
		Light,
		StaticCollection,
		Flora,
		TalkingActivator
	};

	struct TextureSetInfo
	{
		FormID formID{};
		std::uint32_t flags{ 0 };
		std::string editorID;
		std::array<std::string, 8> texturePaths{};
		std::uint16_t dnamFlags{ 0 };
	};

	struct AlternateTextureOverride
	{
		FourCC subrecordSignature{ 0 };
		std::string name3D;
		FormID textureSetFormID{};
		std::uint32_t index3D{ 0 };
	};

	struct ObjectBounds
	{
		std::int16_t min[3]{};
		std::int16_t max[3]{};
	};

	// A static collection's part: the base it places, and where, relative to the collection.
	struct CollectionPlacement
	{
		float position[3]{};
		// Radians, as a reference's DATA has them.
		float rotation[3]{};
		float scale{ 1.0f };
	};

	struct CollectionPart
	{
		FormID baseFormID{};
		std::vector<CollectionPlacement> placements;
	};

	// A destruction stage (DEST's DSTD) that shows a replacement model (DMDL).
	struct DestructionModel
	{
		// DSTD's model damage stage.
		std::uint8_t stage{ 0 };
		std::string modelPath;
	};

	struct BaseObjectInfo
	{
		FormID formID{};
		FourCC signature{ 0 };
		BaseObjectKind kind{ BaseObjectKind::Unknown };
		std::uint32_t flags{ 0 };
		std::string editorID;
		std::string modelPath;
		std::array<std::string, 4> lodModels{};
		std::vector<AlternateTextureOverride> alternateTextures;
		std::optional<ObjectBounds> bounds;
		// STAT DNAM's directional material (MATO), which LOD uses to pick snow and ash shading.
		FormID material{};
		// A static collection's parts (SCOL ONAM and DATA); empty for anything else.
		std::vector<CollectionPart> parts;
		// Whether the base has destruction data (DEST), and its stages with a replacement model, in record order.
		bool destructible{ false };
		std::vector<DestructionModel> destructionModels;

		[[nodiscard]] bool HasDistantLOD() const { return (flags & (1u << 15)) != 0; }
		[[nodiscard]] bool IsNeverFades() const { return (flags & (1u << 2)) != 0; }
		[[nodiscard]] bool HasTreeLOD() const { return (flags & (1u << 6)) != 0; }
	};

	// A record flagged a partial form (CELL, WRLD) changes only the fields it has; the rest are the version below's.
	inline constexpr std::uint32_t RecordFlagPartialForm = 0x4000;

	struct CellInfo
	{
		static constexpr std::uint8_t FieldEditorID = 0x01;
		static constexpr std::uint8_t FieldData = 0x02;
		static constexpr std::uint8_t FieldGrid = 0x04;
		static constexpr std::uint8_t FieldWater = 0x08;

		FormID formID{};
		std::optional<FormID> worldFormID;
		std::string editorID;
		std::uint32_t flags{ 0 };
		std::uint16_t cellFlags{ 0 };
		std::optional<std::int32_t> gridX;
		std::optional<std::int32_t> gridY;
		// XCLC's optional third word; low four bits hide the southwest, southeast, northwest and northeast LAND quadrants.
		std::uint32_t forceHideLand{ 0 };
		std::optional<float> waterHeight;
		// The fields the record has (Field*): those a partial form changes.
		std::uint8_t fields{ 0 };

		[[nodiscard]] bool IsInterior() const { return (cellFlags & 0x1u) != 0; }
		// A worldspace's persistent cell: the record flag 0x400. It has a grid (0,0, in Skyrim.esm) but stands for no
		// place, so it's never the exterior cell at its grid.
		[[nodiscard]] bool IsPersistent() const { return (flags & (1u << 10)) != 0; }
		[[nodiscard]] bool IsPartialForm() const { return (flags & RecordFlagPartialForm) != 0; }
		// This version with a partial form's fields over it.
		void Apply(const CellInfo& a_partial);
	};

	struct LandTextureInfo
	{
		FormID formID{};
		std::uint32_t flags{ 0 };
		std::string editorID;
		std::uint8_t havokFriction{ 0 };
		std::uint8_t havokRestitution{ 0 };
		std::uint8_t textureSpecular{ 30 };
		FormID textureSetFormID{};
		FormID materialFormID{};
		std::uint32_t materialFlags{ 0 };
		std::vector<FormID> grassFormIDs;

		[[nodiscard]] bool IsSnow() const { return (materialFlags & kSkyrimLandTextureFlagSnow) != 0; }
	};

	// MATT: a material type. The engine's material ID is BSCRC32 of its lower-cased name (seasons::MaterialID).
	struct MaterialTypeInfo
	{
		FormID formID{};
		std::string editorID;
		// MNAM.
		std::string name;
	};

	enum class GrassWaterState : std::uint32_t
	{
		AboveOnlyAtLeast = 0,
		AboveOnlyAtMost = 1,
		BelowOnlyAtLeast = 2,
		BelowOnlyAtMost = 3,
		BothAtLeast = 4,
		BothAtMost = 5,
		BothAtMostAbove = 6,
		BothAtMostBelow = 7
	};

	struct GrassInfo
	{
		FormID formID{};
		std::uint32_t flags{ 0 };
		std::string editorID;
		std::string modelPath;
		std::optional<ObjectBounds> bounds;
		std::uint8_t density{ 0 };
		std::uint8_t minSlopeDegrees{ 0 };
		std::uint8_t maxSlopeDegrees{ 90 };
		std::uint16_t distanceFromWaterLevel{ 0 };
		GrassWaterState underwaterState{ GrassWaterState::AboveOnlyAtLeast };
		float positionRange{ 128.0f };
		float heightRange{ 0.0f };
		float colorRange{ 0.0f };
		float wavePeriod{ 0.0f };
		std::uint8_t grassFlags{ 0 };

		[[nodiscard]] bool HasVertexLighting() const { return (grassFlags & 0x1u) != 0; }
		[[nodiscard]] bool HasUniformScaling() const { return (grassFlags & 0x2u) != 0; }
		[[nodiscard]] bool FitsToSlope() const { return (grassFlags & 0x4u) != 0; }
	};

	struct LandBaseTexture
	{
		FormID landTextureFormID{};
		std::uint8_t quadrant{ 0 };
	};

	struct LandAlphaTexture
	{
		FormID landTextureFormID{};
		std::uint8_t quadrant{ 0 };
		std::uint16_t layerIndex{ 0 };
	};

	struct LandVertexAlpha
	{
		std::uint8_t quadrant{ 0 };
		std::uint16_t layerIndex{ 0 };
		std::uint16_t position{ 0 };
		float opacity{ 0.0f };
	};

	struct LandInfo
	{
		static constexpr std::size_t VertexSide = 33;
		static constexpr std::size_t VertexCount = VertexSide * VertexSide;
		static constexpr std::size_t QuadrantVertexSide = 17;
		static constexpr std::size_t QuadrantVertexCount = QuadrantVertexSide * QuadrantVertexSide;
		static constexpr std::size_t QuadrantCount = 4;

		FormID formID{};
		std::uint32_t flags{ 0 };
		FormID parentCell{};
		std::optional<FormID> worldFormID;
		std::optional<std::int32_t> cellX;
		std::optional<std::int32_t> cellY;
		std::array<float, VertexCount> heights{};
		std::array<std::array<std::int8_t, 3>, VertexCount> sourceNormals{};
		std::array<std::array<std::uint8_t, 3>, VertexCount> vertexColors{};
		bool hasHeights{ false };
		bool hasSourceNormals{ false };
		bool hasVertexColors{ false };
		std::array<std::optional<LandBaseTexture>, QuadrantCount> baseTextures{};
		std::vector<LandAlphaTexture> alphaTextures;
		std::vector<LandVertexAlpha> vertexAlphas;
	};

	struct PlacementInfo
	{
		FormID formID{};
		FormID parentCell{};
		std::optional<FormID> worldFormID;
		FormID baseFormID{};
		FourCC signature{ 0 };
		std::uint32_t flags{ 0 };
		float position[3]{};
		float rotation[3]{};
		float scale{ 1.0f };
		std::optional<FormID> enableParent;
		std::uint32_t enableParentFlags{ 0 };
		std::optional<FormID> emittance;
		// The plugin whose version this is (the winning one, in a snapshot), and its record's form version.
		FileID sourceFile{};
		std::uint16_t formVersion{ 0 };

		[[nodiscard]] bool IsDeleted() const { return (flags & (1u << 5)) != 0; }
		[[nodiscard]] bool IsInitiallyDisabled() const { return (flags & (1u << 11)) != 0; }
		[[nodiscard]] bool IsPersistent() const { return (flags & (1u << 10)) != 0; }
		[[nodiscard]] bool IsVisibleWhenDistant() const { return (flags & (1u << 15)) != 0; }
		[[nodiscard]] bool IsFullLOD() const { return (flags & (1u << 16)) != 0; }
	};

	struct CellKey
	{
		FormID worldFormID{};
		std::int32_t x{ 0 };
		std::int32_t y{ 0 };

		friend bool operator==(const CellKey&, const CellKey&) = default;
	};

	struct CellKeyHash
	{
		std::size_t operator()(const CellKey& a_key) const noexcept;
	};

	struct WorldIndex
	{
		std::unordered_map<FormID, BaseObjectInfo, FormIDHash> baseObjects;
		std::unordered_map<FormID, TextureSetInfo, FormIDHash> textureSets;
		std::unordered_map<FormID, LandTextureInfo, FormIDHash> landTextures;
		std::unordered_map<FormID, GrassInfo, FormIDHash> grasses;
		std::unordered_map<FormID, CellInfo, FormIDHash> cells;
		std::unordered_map<FormID, std::vector<LandInfo>, FormIDHash> landsByWorldspace;
		std::unordered_map<CellKey, std::vector<PlacementInfo>, CellKeyHash> exteriorPlacements;
		std::unordered_map<FormID, std::vector<PlacementInfo>, FormIDHash> interiorPlacements;
		std::vector<PlacementInfo> orphanPlacements;
	};

	struct TerrainVertex
	{
		float position[3]{};
		float normal[3]{ 0.0f, 0.0f, 1.0f };
		float color[3]{ 1.0f, 1.0f, 1.0f };
		float uv[2]{};
	};

	struct TerrainLayer
	{
		FormID landTextureFormID{};
		FormID textureSetFormID{};
		std::string textureSetEditorID;
		std::string diffusePath;
		std::string normalPath;
		std::string heightPath;
		std::string rmaosPath;
		float uvScale{ kSkyrimDefaultTerrainLayerUvScale };
		float fallbackColor[4]{ 0.45f, 0.42f, 0.36f, 1.0f };
		// Close LAND layer metadata.
		std::uint32_t materialFlags{ 0 };
	};

	struct TerrainLayerRef
	{
		std::uint32_t layerIndex{ 0 };
	};

	struct TerrainRegionDescriptor
	{
		std::int32_t regionX{ 0 };
		std::int32_t regionY{ 0 };
		std::uint32_t layerRefStart{ 0 };
		std::uint32_t layerRefCount{ 0 };
		std::uint32_t weightBlockStart{ 0 };
		std::uint32_t weightSampleSide{ 0 };
	};

	struct TerrainBuildDiagnostics
	{
		std::uint32_t maxFilterFootprintLayers{ 0 };
		std::uint32_t totalFilterFootprintLayers{ 0 };
	};

	struct TerrainWorldBuildResult
	{
		FormID worldFormID{};
		std::vector<TerrainVertex> vertices;
		std::vector<std::uint32_t> indices;
		std::vector<TerrainLayer> layers;
		std::vector<TerrainLayerRef> layerRefs;
		std::vector<TerrainRegionDescriptor> regions;
		std::vector<std::uint32_t> weightBlocks;
		TerrainBuildDiagnostics diagnostics;
	};

	// What BuildTerrainWorld builds: the welded mesh, the painted layers (layers, region weight blocks), or both.
	struct TerrainBuildOptions
	{
		bool geometry{ true };
		bool material{ true };
		// Land textures painted as others, as Seasons of Skyrim swaps them in a season: each layer's land texture (the
		// default one included) is looked up here first. Must outlive the call.
		const std::unordered_map<FormID, FormID, FormIDHash>* landTextureSwaps{ nullptr };
	};

	struct ModelPathReference
	{
		std::string normalizedPath;
		std::string originalPath;
		FormID formID{};
		FourCC recordSignature{ 0 };
		FourCC subrecordSignature{ 0 };
		std::string sourcePlugin;
		std::size_t sourceFileIndex{ 0 };
		bool lodModel{ false };
	};

	struct ModelPathIndex
	{
		std::vector<ModelPathReference> references;
		std::vector<std::string> uniqueNormalizedPaths;
	};

	// A REFR the worldspace shows as a large reference (WRLD RNAM): loaded with the cells around the player rather
	// than only with its own.
	struct LargeReference
	{
		FormID reference{};
		std::int16_t cellY{ 0 };
		std::int16_t cellX{ 0 };
	};

	struct WorldInfo
	{
		static constexpr std::uint8_t DataFlagSmallWorld = 0x01;
		static constexpr std::uint8_t DataFlagNoLodWater = 0x08;
		static constexpr std::uint8_t DataFlagNoLandscape = 0x10;
		static constexpr std::uint16_t ParentUseLandData = 0x0001;
		static constexpr std::uint16_t ParentUseLodData = 0x0002;
		static constexpr std::uint16_t ParentUseWaterData = 0x0008;

		static constexpr std::uint16_t FieldEditorID = 0x0001;
		static constexpr std::uint16_t FieldParent = 0x0002;
		static constexpr std::uint16_t FieldParentUse = 0x0004;
		static constexpr std::uint16_t FieldData = 0x0008;
		static constexpr std::uint16_t FieldHeights = 0x0010;
		static constexpr std::uint16_t FieldBoundsMin = 0x0020;
		static constexpr std::uint16_t FieldBoundsMax = 0x0040;
		static constexpr std::uint16_t FieldLodWaterType = 0x0080;
		static constexpr std::uint16_t FieldLodWaterHeight = 0x0100;
		static constexpr std::uint16_t FieldLodMultiplier = 0x0200;
		static constexpr std::uint16_t FieldHdLodDiffuse = 0x0400;
		static constexpr std::uint16_t FieldHdLodNormal = 0x0800;
		static constexpr std::uint16_t FieldLargeReferences = 0x1000;

		FormID formID{};
		std::uint32_t flags{ 0 };
		std::string editorID;
		std::optional<FormID> parentWorld;
		std::uint16_t parentUseFlags{ 0 };
		std::uint8_t dataFlags{ 0 };
		std::optional<float> defaultLandHeight;
		std::optional<float> defaultWaterHeight;
		// Object bounds in world units (NAM0, NAM9): the corners of the area the worldspace holds.
		std::optional<std::array<float, 2>> boundsMin;
		std::optional<std::array<float, 2>> boundsMax;
		FormID lodWaterType{};
		std::optional<float> lodWaterHeight;
		std::optional<float> distantLodMultiplier;
		std::string hdLodDiffuse;
		std::string hdLodNormal;
		std::vector<LargeReference> largeReferences;
		// The fields the record has (Field*): those a partial form changes.
		std::uint16_t fields{ 0 };

		[[nodiscard]] bool HasNoLodWater() const { return (dataFlags & DataFlagNoLodWater) != 0; }
		[[nodiscard]] bool IsPartialForm() const { return (flags & RecordFlagPartialForm) != 0; }
		// This version with a partial form's fields over it.
		void Apply(const WorldInfo& a_partial);
		[[nodiscard]] bool UsesParent(std::uint16_t a_flag) const { return parentWorld.has_value() && (parentUseFlags & a_flag) != 0; }
	};

	struct StaticRecordSuppressor
	{
		FormID formID{};
		FourCC signature{ 0 };
		std::uint32_t flags{ 0 };
		std::size_t sourceFileIndex{ 0 };
	};

	struct StaticPluginStats
	{
		std::size_t recordsScanned{ 0 };
		std::size_t typedRecords{ 0 };
		std::size_t baseObjects{ 0 };
		std::size_t worlds{ 0 };
		std::size_t cells{ 0 };
		std::size_t lands{ 0 };
		std::size_t landTextures{ 0 };
		std::size_t placements{ 0 };
		std::size_t suppressors{ 0 };
		std::size_t compressedRecords{ 0 };
		std::size_t compressedBaseRecords{ 0 };
		std::size_t compressedWorldRecords{ 0 };
		std::size_t compressedCellRecords{ 0 };
		std::size_t compressedLandRecords{ 0 };
		std::size_t compressedPlacementRecords{ 0 };
		std::size_t streamedCellRecords{ 0 };
		std::size_t streamedCellEarlyStops{ 0 };
		std::size_t streamedCellOutputBytes{ 0 };
		std::size_t fileBytes{ 0 };
		std::size_t uncompressedRecordBytes{ 0 };
		double readMilliseconds{ 0.0 };
		double scanMilliseconds{ 0.0 };
		double inflateMilliseconds{ 0.0 };
		double totalMilliseconds{ 0.0 };
	};

	struct EditorIDInfo
	{
		FormID formID{};
		std::string editorID;
	};

	struct StaticPluginShard
	{
		LoadOrderEntry entry;
		PluginHeader header;
		std::vector<std::uint8_t> bytes;
		std::vector<BaseObjectInfo> baseObjects;
		std::vector<TextureSetInfo> textureSets;
		std::vector<LandTextureInfo> landTextures;
		std::vector<GrassInfo> grasses;
		std::vector<WorldInfo> worlds;
		std::vector<EditorIDInfo> materialObjects;
		std::vector<EditorIDInfo> regions;
		std::vector<MaterialTypeInfo> materialTypes;
		std::vector<CellInfo> cells;
		std::vector<LandInfo> lands;
		std::vector<PlacementInfo> placements;
		std::vector<StaticRecordSuppressor> suppressors;
		StaticPluginStats stats;
	};

	struct StaticWorldSnapshot
	{
		struct BuildStats
		{
			double countInputsMilliseconds{ 0.0 };
			double mergeRecordsMilliseconds{ 0.0 };
			double bucketPlacementsMilliseconds{ 0.0 };
			double publishKeysMilliseconds{ 0.0 };
			std::size_t placementCandidates{ 0 };
			std::size_t winningPlacements{ 0 };
			std::size_t skippedPlacementOverrides{ 0 };
			std::size_t skippedDeletedPlacements{ 0 };
			std::size_t skippedDisabledPlacements{ 0 };
			std::size_t skippedMissingBasePlacements{ 0 };
			std::size_t skippedMissingCellPlacements{ 0 };
		};

		std::unordered_map<FormID, BaseObjectInfo, FormIDHash> baseObjectsByFormID;
		std::unordered_map<FormID, TextureSetInfo, FormIDHash> textureSetsByFormID;
		std::unordered_map<FormID, LandTextureInfo, FormIDHash> landTexturesByFormID;
		std::unordered_map<FormID, GrassInfo, FormIDHash> grassesByFormID;
		// The winning version of each worldspace, with large references (RNAM) merged from every master's version.
		std::unordered_map<FormID, WorldInfo, FormIDHash> worldsByFormID;
		std::unordered_map<FormID, std::string, FormIDHash> materialObjectsByFormID;
		std::unordered_map<FormID, std::string, FormIDHash> regionsByFormID;
		std::unordered_map<FormID, MaterialTypeInfo, FormIDHash> materialTypesByFormID;
		// The order forms were first defined in, through the load order, and the first definition's editor ID: the
		// engine's creation order and the editor ID powerofthree's Tweaks keeps. Land textures, material types, texture
		// sets, material objects and base objects only.
		std::unordered_map<FormID, std::uint32_t, FormIDHash> creationOrder;
		std::unordered_map<FormID, std::string, FormIDHash> firstEditorIDs;
		std::unordered_map<FormID, CellInfo, FormIDHash> cellsByFormID;
		std::unordered_map<FormID, std::vector<LandInfo>, FormIDHash> landsByWorldspace;
		std::unordered_map<CellKey, std::vector<PlacementInfo>, CellKeyHash> exteriorPlacementsByCell;
		std::unordered_map<FormID, std::vector<PlacementInfo>, FormIDHash> interiorPlacementsByCell;
		std::vector<CellKey> exteriorCellKeys;
		std::vector<FormID> interiorCellKeys;
		std::vector<PlacementInfo> orphanPlacements;
		std::size_t suppressedRecords{ 0 };
		BuildStats buildStats;

		// Every placement, in exterior and interior cells and orphaned, in no particular order.
		template <class Visit>
		void ForEachPlacement(Visit&& a_visit) const
		{
			for (const auto& [_, placements] : exteriorPlacementsByCell)
				for (const auto& placement : placements) a_visit(placement);
			for (const auto& [_, placements] : interiorPlacementsByCell)
				for (const auto& placement : placements) a_visit(placement);
			for (const auto& placement : orphanPlacements) a_visit(placement);
		}

		[[nodiscard]] const std::vector<PlacementInfo>* GetExteriorCell(FormID a_worldFormID, std::int32_t a_x, std::int32_t a_y) const;
		[[nodiscard]] const std::vector<PlacementInfo>* GetInteriorCell(FormID a_cellFormID) const;
		[[nodiscard]] WorldIndex MaterializeWorldIndex() const;
	};

	struct StaticWorldSnapshotBuildOptions
	{
		bool parallelPlacementBucketing{ true };
		// Skyrim LoadPartial: CELL retains all fields; WRLD reads only map bounds. Orphans are skipped.
		bool enginePartialForms{ false };
		// Consumers planning dynamic references need these observations before their own visibility classification.
		bool includeDisabledPlacements{ false };
	};

	class GameDataSnapshot
	{
	public:
		std::vector<PluginFile> plugins;
		ResolvedRecordStore resolvedRecords;
		WorldIndex worldIndex;

		[[nodiscard]] std::size_t RawRecordCount() const;
	};

	// Plugin headers by path, file time and size, so a load order read again only reads the headers that changed.
	// Thread-safe. Its owner decides how long it lives; a parser without one reads every header.
	class HeaderCache
	{
	public:
		[[nodiscard]] std::optional<PluginHeader> Find(
			const std::filesystem::path& a_path,
			std::filesystem::file_time_type a_writeTime,
			std::uintmax_t a_fileSize) const;
		void Store(
			const std::filesystem::path& a_path,
			std::filesystem::file_time_type a_writeTime,
			std::uintmax_t a_fileSize,
			PluginHeader a_header);

	private:
		struct Entry
		{
			std::filesystem::file_time_type writeTime{};
			std::uintmax_t fileSize{ 0 };
			PluginHeader header;
		};

		mutable std::mutex _mutex;
		std::unordered_map<std::filesystem::path::string_type, Entry> _entries;
	};

	class PluginParser
	{
	public:
		PluginParser() = default;
		explicit PluginParser(HeaderCache* a_headerCache) :
			_headerCache(a_headerCache)
		{}

		enum class ParseMode
		{
			FullRaw,
			StaticWorldIndex,
			// Skyrim grass loading: LAND DATA gating, with the same decoding and override resolver.
			StaticGrassIndex
		};

		[[nodiscard]] PluginHeader ReadHeader(const std::filesystem::path& a_path) const;
		[[nodiscard]] PluginFile Parse(
			const LoadOrderEntry& a_entry,
			std::size_t a_sourceFileIndex,
			std::span<const LoadOrderEntry> a_loadOrder,
			ParseMode a_mode = ParseMode::FullRaw) const;
		[[nodiscard]] StaticPluginShard ParseStaticWorldShard(
			const LoadOrderEntry& a_entry,
			std::size_t a_sourceFileIndex,
			std::span<const LoadOrderEntry> a_loadOrder,
			ParseMode a_mode = ParseMode::StaticWorldIndex) const;

	private:
		HeaderCache* _headerCache{ nullptr };
	};

	[[nodiscard]] std::vector<LoadOrderEntry> PrepareLoadOrder(std::vector<LoadOrderEntry> a_entries, HeaderCache* a_headerCache = nullptr);
	[[nodiscard]] ResolvedRecordStore BuildResolvedRecordStore(const std::vector<PluginFile>& a_plugins);
	[[nodiscard]] WorldIndex BuildWorldIndex(const ResolvedRecordStore& a_resolvedRecords);
	[[nodiscard]] WorldIndex BuildStaticWorldIndex(const std::vector<PluginFile>& a_plugins);
	[[nodiscard]] std::array<float, LandInfo::VertexCount> DecodeLandHeights(std::span<const std::uint8_t> a_vhgtPayload);
	[[nodiscard]] TerrainWorldBuildResult BuildTerrainWorld(FormID a_worldFormID, const WorldIndex& a_worldIndex, TerrainBuildOptions a_options = {});
	[[nodiscard]] TerrainWorldBuildResult BuildTerrainWorld(FormID a_worldFormID, const StaticWorldSnapshot& a_snapshot, TerrainBuildOptions a_options = {});
	[[nodiscard]] std::string NormalizeModelPath(std::string_view a_path);
	[[nodiscard]] ModelPathIndex BuildModelPathIndex(
		const std::vector<PluginFile>& a_plugins,
		const ResolvedRecordStore& a_resolvedRecords);
	[[nodiscard]] StaticWorldSnapshot BuildStaticWorldSnapshot(
		const std::vector<StaticPluginShard>& a_shards,
		StaticWorldSnapshotBuildOptions a_options = {});
	[[nodiscard]] GameDataSnapshot LoadGameData(ILoadOrderSource& a_source);
}