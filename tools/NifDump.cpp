// tesworld-nifdump <file.nif|.btr|.bto>...
// Prints a NIF's scene graph: block types, names, transforms, shapes' vertex formats and counts, shader flags,
// texture sets, segments and extra data. For comparing generated LOD against the game's.

#include <cstdio>
#include <iostream>
#include <string>

#include <NifFile.hpp>

using namespace nifly;

namespace
{
	void Indent(int a_depth) { std::cout << std::string(static_cast<std::size_t>(a_depth) * 2, ' '); }

	void PrintTransform(const MatTransform& a_transform)
	{
		std::printf(" t=(%.1f, %.1f, %.1f) s=%.3f", a_transform.translation.x, a_transform.translation.y, a_transform.translation.z, a_transform.scale);
	}

	void Dump(NifFile& a_nif, NiObject* a_object, int a_depth)
	{
		if (!a_object) return;
		Indent(a_depth);
		std::cout << a_object->GetBlockName();
		if (auto* net = dynamic_cast<NiObjectNET*>(a_object)) {
			std::cout << " '" << net->name.get() << "'";
			for (auto& extraRef : net->extraDataRefs) {
				if (auto* extra = a_nif.GetHeader().GetBlock<NiExtraData>(extraRef)) {
					std::cout << " [extra " << extra->GetBlockName() << " '" << extra->name.get() << "']";
				}
			}
		}
		if (auto* av = dynamic_cast<NiAVObject*>(a_object)) {
			std::cout << " flags=" << av->flags;
			PrintTransform(av->transform);
		}
		if (auto* multi = dynamic_cast<BSMultiBoundNode*>(a_object)) {
			std::cout << " cull=" << multi->cullingMode;
			if (auto* bound = a_nif.GetHeader().GetBlock(multi->multiBoundRef)) {
				if (auto* aabb = a_nif.GetHeader().GetBlock<BSMultiBoundAABB>(bound->dataRef)) {
					std::printf(" aabb c=(%.1f, %.1f, %.1f) e=(%.1f, %.1f, %.1f)", aabb->center.x, aabb->center.y, aabb->center.z,
						aabb->halfExtent.x, aabb->halfExtent.y, aabb->halfExtent.z);
				}
			}
		}
		std::cout << '\n';

		if (auto* shape = dynamic_cast<BSTriShape*>(a_object)) {
			Indent(a_depth + 1);
			std::printf("verts=%u tris=%u desc: vert=%d uv=%d normal=%d tangent=%d colors=%d skinned=%d fullprec=%d\n",
				shape->GetNumVertices(), shape->GetNumTriangles(), shape->HasVertices(), shape->HasUVs(), shape->HasNormals(),
				shape->HasTangents(), shape->HasVertexColors(), shape->IsSkinned(), shape->IsFullPrecision());
			const auto bounds = shape->GetBounds();
			Indent(a_depth + 1);
			std::printf("bounds c=(%.1f, %.1f, %.1f) r=%.1f\n", bounds.center.x, bounds.center.y, bounds.center.z, bounds.radius);
			if (!shape->vertData.empty()) {
				const auto& v = shape->vertData.front();
				Indent(a_depth + 1);
				std::printf("vert0 p=(%.2f, %.2f, %.2f) uv=(%.4f, %.4f) n=(%u %u %u) c=(%u %u %u %u)\n", v.vert.x, v.vert.y, v.vert.z,
					v.uv.u, v.uv.v, v.normal[0], v.normal[1], v.normal[2], v.colorData[0], v.colorData[1], v.colorData[2], v.colorData[3]);
			}
			if (auto* subIndex = dynamic_cast<BSSubIndexTriShape*>(a_object)) {
				const auto segments = subIndex->GetSegments();
				Indent(a_depth + 1);
				std::cout << "segments=" << segments.size() << ':';
				for (const auto& segment : segments) {
					std::cout << " (" << static_cast<int>(segment.flags) << ',' << segment.index << ',' << segment.numTris << ')';
				}
				std::cout << '\n';
			}
			if (auto* shader = a_nif.GetShader(shape)) {
				Indent(a_depth + 1);
				std::cout << shader->GetBlockName() << " type=" << shader->GetShaderType();
				if (auto* bs = dynamic_cast<BSShaderProperty*>(shader)) {
					std::printf(" flags1=0x%08X flags2=0x%08X", bs->shaderFlags1, bs->shaderFlags2);
				}
				if (auto* lighting = dynamic_cast<BSLightingShaderProperty*>(shader)) {
					std::printf(" clamp=%u gloss=%.2f spec=%.2f emit=(%.2f %.2f %.2f)x%.2f alpha=%.2f", static_cast<unsigned>(lighting->textureClampMode),
						lighting->glossiness, lighting->specularStrength, lighting->emissiveColor.x, lighting->emissiveColor.y,
						lighting->emissiveColor.z, lighting->emissiveMultiple, lighting->alpha);
				}
				std::cout << '\n';
				for (uint32_t slot = 0; slot < 9; ++slot) {
					std::string texture;
					a_nif.GetTextureSlot(shape, texture, slot);
					if (!texture.empty()) {
						Indent(a_depth + 2);
						std::cout << "tex" << slot << ": " << texture << '\n';
					}
				}
			}
			if (auto* alpha = a_nif.GetAlphaProperty(shape)) {
				Indent(a_depth + 1);
				std::cout << "NiAlphaProperty flags=" << alpha->flags << " threshold=" << static_cast<int>(alpha->threshold) << '\n';
			}
		}

		if (auto* node = dynamic_cast<NiNode*>(a_object)) {
			for (auto& child : node->childRefs) {
				Dump(a_nif, a_nif.GetHeader().GetBlock<NiObject>(child), a_depth + 1);
			}
		}
	}
}

int main(int argc, char** argv)
{
	for (int i = 1; i < argc; ++i) {
		NifFile nif;
		if (nif.Load(std::filesystem::path(argv[i])) != 0) {
			std::cerr << "can't read " << argv[i] << '\n';
			continue;
		}
		const auto& version = nif.GetHeader().GetVersion();
		std::cout << "== " << argv[i] << " (" << version.String() << ", user " << version.User() << ", stream " << version.Stream()
		          << ", blocks " << nif.GetHeader().GetNumBlocks() << ")\n";
		Dump(nif, nif.GetHeader().GetBlock<NiObject>(0u), 0);
	}
	return 0;
}
