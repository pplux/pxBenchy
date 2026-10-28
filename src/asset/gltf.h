#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Vertex attributes a loaded mesh can carry. Output is always float32:
// position/normal float3, tangent float4, uv0/uv1 float2, color0 float4.
enum class AssetAttr { position, normal, tangent, uv0, uv1, color0 };
enum class AssetIndexType { u16, u32 };

struct AssetMesh {
  std::vector<uint8_t> vertices;  // interleaved float32, in request order
  uint32_t vertex_count = 0, stride = 0;  // stride in bytes
  std::vector<uint8_t> indices;
  AssetIndexType index_type = AssetIndexType::u16;
  uint32_t index_count = 0;
  float aabb_min[3] = {}, aabb_max[3] = {};  // from the POSITION accessor, always filled
};

// CPU-only. Parses .glb bytes (all buffers embedded in the BIN chunk) and extracts mesh 0, primitive 0
// (TRIANGLES, indexed, non-sparse). Node transforms are ignored. If mesh 0 has more primitives a warning
// line is printed to stdout and the rest are skipped.
// attrs: non-empty, no duplicates. Sources: POSITION/NORMAL FLOAT VEC3, TANGENT FLOAT VEC4, TEXCOORD_n FLOAT
// or normalized UNSIGNED_BYTE/UNSIGNED_SHORT VEC2, COLOR_0 FLOAT or normalized UNSIGNED_BYTE/UNSIGNED_SHORT
// VEC3 (alpha = 1) / VEC4. Indices: UNSIGNED_BYTE widened to u16, UNSIGNED_SHORT -> u16, UNSIGNED_INT -> u32.
// On failure returns false and sets *err to a short "glb: ..." text; *out is left unspecified.
bool asset_glb_load(const void* data, size_t size, const std::vector<AssetAttr>& attrs, AssetMesh* out,
                    std::string* err);
