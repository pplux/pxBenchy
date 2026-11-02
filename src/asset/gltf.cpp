#include "asset/gltf.h"
#include <cstdio>
#include <cstring>
#include <tiny_gltf_v3.h>

static bool fail(std::string* err, const std::string& msg) {
  if (err) { *err = "glb: " + msg; }
  return false;
}

// Bounds-checked view of an accessor's elements: element i starts at base + i * stride.
struct AccView {
  const uint8_t* base = nullptr;
  uint64_t stride = 0, count = 0;
  const tg3_accessor* acc = nullptr;
};

static uint32_t comp_size(int32_t ct) {
  switch (ct) {
    case TG3_COMPONENT_TYPE_BYTE:
    case TG3_COMPONENT_TYPE_UNSIGNED_BYTE: return 1;
    case TG3_COMPONENT_TYPE_SHORT:
    case TG3_COMPONENT_TYPE_UNSIGNED_SHORT: return 2;
    case TG3_COMPONENT_TYPE_INT:
    case TG3_COMPONENT_TYPE_UNSIGNED_INT:
    case TG3_COMPONENT_TYPE_FLOAT: return 4;
    default: return 0;
  }
}

static uint32_t type_comps(int32_t t) {
  switch (t) {
    case TG3_TYPE_SCALAR: return 1;
    case TG3_TYPE_VEC2: return 2;
    case TG3_TYPE_VEC3: return 3;
    case TG3_TYPE_VEC4: return 4;
    default: return 0;
  }
}

static bool acc_view(const tg3_model& m, int32_t idx, const char* what, AccView* v, std::string* err) {
  if (idx < 0 || (uint32_t)idx >= m.accessors_count) { return fail(err, std::string(what) + ": bad accessor"); }
  const tg3_accessor& a = m.accessors[idx];
  if (a.sparse.is_sparse) { return fail(err, std::string(what) + ": sparse accessor unsupported"); }
  if (a.buffer_view < 0 || (uint32_t)a.buffer_view >= m.buffer_views_count) {
    return fail(err, std::string(what) + ": accessor has no buffer view");
  }
  const tg3_buffer_view& bv = m.buffer_views[a.buffer_view];
  if (bv.buffer < 0 || (uint32_t)bv.buffer >= m.buffers_count) { return fail(err, std::string(what) + ": bad buffer"); }
  const tg3_buffer& buf = m.buffers[bv.buffer];
  uint32_t esize = comp_size(a.component_type) * type_comps(a.type);
  if (!esize) { return fail(err, std::string(what) + ": unsupported component type or shape"); }
  uint64_t stride = bv.byte_stride ? bv.byte_stride : esize;
  if (stride < esize) { return fail(err, std::string(what) + ": byteStride too small"); }
  uint64_t blen = buf.data.count;
  if (bv.byte_offset > blen || bv.byte_length > blen - bv.byte_offset) {
    return fail(err, std::string(what) + ": buffer view out of range");
  }
  if (a.byte_offset > bv.byte_length) { return fail(err, std::string(what) + ": accessor out of range"); }
  uint64_t room = bv.byte_length - a.byte_offset;
  if (a.count > 0) {
    if (a.count - 1 > room) { return fail(err, std::string(what) + ": accessor out of range"); }
    uint64_t last = (a.count - 1) * stride; // room < buffer size, stride < 2^32: no overflow
    if (last > room || esize > room - last) { return fail(err, std::string(what) + ": accessor out of range"); }
  }
  v->base = buf.data.data + bv.byte_offset + a.byte_offset;
  v->stride = stride;
  v->count = a.count;
  v->acc = &a;
  return true;
}

static const char* attr_semantic(AssetAttr a) {
  switch (a) {
    case AssetAttr::position: return "POSITION";
    case AssetAttr::normal: return "NORMAL";
    case AssetAttr::tangent: return "TANGENT";
    case AssetAttr::uv0: return "TEXCOORD_0";
    case AssetAttr::uv1: return "TEXCOORD_1";
    case AssetAttr::color0: return "COLOR_0";
  }
  return "";
}

static uint32_t attr_floats(AssetAttr a) {
  switch (a) {
    case AssetAttr::position:
    case AssetAttr::normal: return 3;
    case AssetAttr::tangent:
    case AssetAttr::color0: return 4;
    case AssetAttr::uv0:
    case AssetAttr::uv1: return 2;
  }
  return 0;
}

static float read_comp(const uint8_t* p, int32_t ct) {
  switch (ct) {
    case TG3_COMPONENT_TYPE_UNSIGNED_BYTE: return *p / 255.0f;
    case TG3_COMPONENT_TYPE_UNSIGNED_SHORT: {
      uint16_t u;
      memcpy(&u, p, 2);
      return u / 65535.0f;
    }
    default: {
      float f;
      memcpy(&f, p, 4);
      return f;
    }
  }
}

// Validates the accessor shape for attribute `a` (see header).
static bool check_attr(AssetAttr a, const tg3_accessor& acc, std::string* err) {
  const char* name = attr_semantic(a);
  bool is_float = acc.component_type == TG3_COMPONENT_TYPE_FLOAT;
  bool norm_int = acc.normalized && (acc.component_type == TG3_COMPONENT_TYPE_UNSIGNED_BYTE ||
                                     acc.component_type == TG3_COMPONENT_TYPE_UNSIGNED_SHORT);
  bool ok = false;
  switch (a) {
    case AssetAttr::position:
    case AssetAttr::normal: ok = is_float && acc.type == TG3_TYPE_VEC3; break;
    case AssetAttr::tangent: ok = is_float && acc.type == TG3_TYPE_VEC4; break;
    case AssetAttr::uv0:
    case AssetAttr::uv1: ok = (is_float || norm_int) && acc.type == TG3_TYPE_VEC2; break;
    case AssetAttr::color0:
      ok = (is_float || norm_int) && (acc.type == TG3_TYPE_VEC3 || acc.type == TG3_TYPE_VEC4);
      break;
  }
  if (!ok) { return fail(err, std::string(name) + ": unsupported component type or shape"); }
  return true;
}

bool asset_glb_load(const void* data, size_t size, const std::vector<AssetAttr>& attrs, AssetMesh* out,
                    std::string* err) {
  if (attrs.empty()) { return fail(err, "no attributes requested"); }
  for (size_t i = 0; i < attrs.size(); i++) {
    for (size_t j = i + 1; j < attrs.size(); j++) {
      if (attrs[i] == attrs[j]) { return fail(err, std::string("duplicate attribute ") + attr_semantic(attrs[i])); }
    }
  }

  tg3_parse_options opts;
  tg3_parse_options_init(&opts);
  opts.borrow_input_buffers = 1;
  tg3_model model;
  memset(&model, 0, sizeof(model));
  tg3_error_stack es;
  tg3_error_stack_init(&es);

  // Error text is arena-owned by the model, so it is copied before the model is freed.
  tg3_error_code code = tg3_parse_glb(&model, &es, (const uint8_t*)data, size, nullptr, 0, &opts);
  if (code != TG3_OK || tg3_errors_has_error(&es)) {
    std::string msg = "parse failed";
    for (uint32_t i = 0; i < tg3_errors_count(&es); i++) {
      const tg3_error_entry* e = tg3_errors_get(&es, i);
      if (e->severity == TG3_SEVERITY_ERROR && e->message) {
        msg = e->message;
        break;
      }
    }
    tg3_error_stack_free(&es);
    tg3_model_free(&model);
    return fail(err, msg);
  }
  tg3_error_stack_free(&es);

  struct Guard {
    tg3_model* m;
    ~Guard() { tg3_model_free(m); }
  } guard{&model};

  for (uint32_t i = 0; i < model.buffers_count; i++) {
    if (model.buffers[i].uri.len) { return fail(err, "buffer with URI unsupported (embedded BIN only)"); }
  }
  if (model.meshes_count == 0 || model.meshes[0].primitives_count == 0) { return fail(err, "no meshes"); }
  const tg3_mesh& mesh = model.meshes[0];
  if (mesh.primitives_count > 1) {
    printf("warning: glb mesh 0 has %u primitives, using primitive 0\n", mesh.primitives_count);
  }
  const tg3_primitive& prim = mesh.primitives[0];
  if (prim.mode != -1 && prim.mode != TG3_MODE_TRIANGLES) { return fail(err, "primitive mode is not TRIANGLES"); }
  if (prim.indices < 0) { return fail(err, "primitive has no indices"); }

  auto find_attr = [&](const char* name) -> int32_t {
    for (uint32_t i = 0; i < prim.attributes_count; i++) {
      if (strlen(name) == prim.attributes[i].key.len &&
          !memcmp(name, prim.attributes[i].key.data, prim.attributes[i].key.len)) {
        return prim.attributes[i].value;
      }
    }
    return -1;
  };

  // Position accessor: always needed for AABB and vertex count.
  int32_t pos_idx = find_attr("POSITION");
  if (pos_idx < 0) { return fail(err, "POSITION missing"); }
  AccView pos;
  if (!acc_view(model, pos_idx, "POSITION", &pos, err)) { return false; }
  if (!check_attr(AssetAttr::position, *pos.acc, err)) { return false; }
  if (pos.acc->min_values_count < 3 || pos.acc->max_values_count < 3) { return fail(err, "POSITION min/max missing"); }
  if (pos.count == 0 || pos.count > UINT32_MAX) { return fail(err, "bad vertex count"); }
  uint32_t vcount = (uint32_t)pos.count;

  std::vector<AccView> views(attrs.size());
  uint32_t stride = 0;
  for (size_t i = 0; i < attrs.size(); i++) {
    const char* name = attr_semantic(attrs[i]);
    int32_t idx = find_attr(name);
    if (idx < 0) { return fail(err, std::string(name) + " missing"); }
    if (!acc_view(model, idx, name, &views[i], err)) { return false; }
    if (!check_attr(attrs[i], *views[i].acc, err)) { return false; }
    if (views[i].count != vcount) { return fail(err, std::string(name) + ": count mismatch"); }
    stride += attr_floats(attrs[i]) * 4;
  }

  AccView idx;
  if (!acc_view(model, prim.indices, "indices", &idx, err)) { return false; }
  int32_t ict = idx.acc->component_type;
  if (idx.acc->type != TG3_TYPE_SCALAR ||
      (ict != TG3_COMPONENT_TYPE_UNSIGNED_BYTE && ict != TG3_COMPONENT_TYPE_UNSIGNED_SHORT &&
       ict != TG3_COMPONENT_TYPE_UNSIGNED_INT)) {
    return fail(err, "indices: unsupported component type or shape");
  }
  if (idx.count == 0 || idx.count > UINT32_MAX) { return fail(err, "bad index count"); }
  bool wide = ict == TG3_COMPONENT_TYPE_UNSIGNED_INT;

  AssetMesh r;
  r.vertex_count = vcount;
  r.stride = stride;
  r.vertices.resize((size_t)vcount * stride);
  uint32_t off = 0;
  for (size_t a = 0; a < attrs.size(); a++) {
    const AccView& v = views[a];
    uint32_t nsrc = type_comps(v.acc->type), nout = attr_floats(attrs[a]);
    uint32_t csz = comp_size(v.acc->component_type);
    for (uint32_t i = 0; i < vcount; i++) {
      const uint8_t* src = v.base + i * v.stride;
      float f[4];
      for (uint32_t c = 0; c < nsrc; c++) { f[c] = read_comp(src + c * csz, v.acc->component_type); }
      for (uint32_t c = nsrc; c < nout; c++) {
        f[c] = 1.0f; // VEC3 color -> alpha 1
      }
      memcpy(&r.vertices[(size_t)i * stride + off], f, nout * 4);
    }
    off += nout * 4;
  }

  r.index_count = (uint32_t)idx.count;
  r.index_type = wide ? AssetIndexType::u32 : AssetIndexType::u16;
  r.indices.resize((size_t)r.index_count * (wide ? 4 : 2));
  for (uint32_t i = 0; i < r.index_count; i++) {
    const uint8_t* src = idx.base + i * idx.stride;
    uint32_t u;
    if (ict == TG3_COMPONENT_TYPE_UNSIGNED_BYTE) {
      u = *src;
    } else if (ict == TG3_COMPONENT_TYPE_UNSIGNED_SHORT) {
      uint16_t s;
      memcpy(&s, src, 2);
      u = s;
    } else {
      memcpy(&u, src, 4);
    }
    if (u >= vcount) { return fail(err, "index out of range"); }
    if (wide) {
      memcpy(&r.indices[(size_t)i * 4], &u, 4);
    } else {
      uint16_t s = (uint16_t)u;
      memcpy(&r.indices[(size_t)i * 2], &s, 2);
    }
  }

  for (int i = 0; i < 3; i++) {
    r.aabb_min[i] = (float)pos.acc->min_values[i];
    r.aabb_max[i] = (float)pos.acc->max_values[i];
  }
  *out = std::move(r);
  return true;
}
