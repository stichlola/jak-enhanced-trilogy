#include "EnhancedImport.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>

#include "common/log/log.h"
#include "common/util/FileUtil.h"
#include "common/util/gltf_util.h"

#include "third-party/meshoptimizer/meshoptimizer.h"
#include "third-party/stb_image/stb_image_write.h"
#include "third-party/tiny_gltf/tiny_gltf.h"


namespace {

constexpr float kLodRatio = 0.33f;  // the far version's share of the faces
constexpr int kLodMin = 300;        // (but never fewer than this, unless the model has fewer)
constexpr int kImpGrid = 8, kImpCell = 128;
constexpr int kImpSuper = 2;  // samples a pixel across (the edges smooth)
constexpr int kTexMax = 2048;

struct Vtx {
  float p[3];
  float n[3];
  float uv[2];
};

struct Image {
  int w = 0, h = 0;
  std::vector<u8> rgba;
  bool cutout = false;  // holes (alpha under a half): drawn alpha tested
};

// ---- reading

// an accessor's elements as floats, comps of each (normalized integers as glTF reads them)
bool read_floats(const tinygltf::Model& m, int acc, int comps, std::vector<float>* out) {
  if (acc < 0 || acc >= (int)m.accessors.size()) {
    return false;
  }
  const auto& a = m.accessors[acc];
  if (a.bufferView < 0 || a.bufferView >= (int)m.bufferViews.size() ||
      tinygltf::GetNumComponentsInType(a.type) < comps) {
    return false;
  }
  const auto& bv = m.bufferViews[a.bufferView];
  if (bv.buffer < 0 || bv.buffer >= (int)m.buffers.size()) {
    return false;
  }
  const auto& data = m.buffers[bv.buffer].data;
  const int stride = a.ByteStride(bv);
  const int csize = tinygltf::GetComponentSizeInBytes(a.componentType);
  const size_t start = bv.byteOffset + a.byteOffset;
  if (stride <= 0 || csize <= 0 ||
      (a.count && start + (a.count - 1) * (size_t)stride + (size_t)comps * csize > data.size())) {
    return false;
  }
  out->resize(a.count * comps);
  for (size_t i = 0; i < a.count; i++) {
    for (int c = 0; c < comps; c++) {
      const u8* p = data.data() + start + i * stride + (size_t)c * csize;
      float v = 0.f;
      switch (a.componentType) {
        case TINYGLTF_COMPONENT_TYPE_FLOAT:
          memcpy(&v, p, 4);
          break;
        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
          v = a.normalized ? p[0] / 255.f : p[0];
          break;
        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: {
          u16 s;
          memcpy(&s, p, 2);
          v = a.normalized ? s / 65535.f : s;
          break;
        }
        case TINYGLTF_COMPONENT_TYPE_BYTE:
          v = a.normalized ? std::max((s8)p[0] / 127.f, -1.f) : (s8)p[0];
          break;
        case TINYGLTF_COMPONENT_TYPE_SHORT: {
          s16 s;
          memcpy(&s, p, 2);
          v = a.normalized ? std::max(s / 32767.f, -1.f) : s;
          break;
        }
        default:
          return false;
      }
      (*out)[i * comps + c] = v;
    }
  }
  return true;
}

bool read_indices(const tinygltf::Model& m, int acc, std::vector<u32>* out) {
  if (acc < 0 || acc >= (int)m.accessors.size()) {
    return false;
  }
  const auto& a = m.accessors[acc];
  if (a.bufferView < 0 || a.bufferView >= (int)m.bufferViews.size()) {
    return false;
  }
  const auto& bv = m.bufferViews[a.bufferView];
  const auto& data = m.buffers[bv.buffer].data;
  const int csize = tinygltf::GetComponentSizeInBytes(a.componentType);
  const int stride = bv.byteStride ? (int)bv.byteStride : csize;
  const size_t start = bv.byteOffset + a.byteOffset;
  if (csize <= 0 || (a.count && start + (a.count - 1) * (size_t)stride + csize > data.size())) {
    return false;
  }
  out->resize(a.count);
  for (size_t i = 0; i < a.count; i++) {
    const u8* p = data.data() + start + i * stride;
    u32 v = 0;
    memcpy(&v, p, csize);  // (little endian: the low bytes)
    (*out)[i] = v;
  }
  return true;
}

// the base color picture of a material (its texture, else its color)
bool material_image(const tinygltf::Model& m, int mat_idx, Image* img) {
  if (mat_idx < 0 || mat_idx >= (int)m.materials.size()) {
    return false;
  }
  const auto& pbr = m.materials[mat_idx].pbrMetallicRoughness;
  const int ti = pbr.baseColorTexture.index;
  if (ti >= 0 && ti < (int)m.textures.size()) {
    const int si = m.textures[ti].source;
    if (si >= 0 && si < (int)m.images.size()) {
      const auto& src = m.images[si];
      if (src.bits == 8 && src.component >= 3 && !src.image.empty() &&
          src.image.size() >= (size_t)src.width * src.height * src.component) {
        img->w = src.width;
        img->h = src.height;
        img->rgba.resize((size_t)img->w * img->h * 4);
        // (times its color factor: the game's own are 2, their textures at half as the PS2 drew
        // them; no alpha unless the material is cut out or blended)
        const auto& mat = m.materials[mat_idx];
        const bool alpha = mat.alphaMode == "MASK" || mat.alphaMode == "BLEND";
        double f[4] = {1, 1, 1, 1};
        if (pbr.baseColorFactor.size() == 4) {
          for (int c = 0; c < 4; c++) {
            f[c] = pbr.baseColorFactor[c];
          }
        }
        for (size_t p = 0; p < (size_t)img->w * img->h; p++) {
          for (int c = 0; c < 4; c++) {
            const double v = c < src.component ? src.image[p * src.component + c] : 255;
            img->rgba[p * 4 + c] = c == 3 && !alpha ? 255 : (u8)std::clamp(v * f[c] + 0.5, 0.0, 255.0);
          }
        }
        return true;
      }
    }
  }
  if (pbr.baseColorFactor.size() == 4) {
    img->w = img->h = 4;
    img->rgba.resize(4 * 4 * 4);
    for (size_t p = 0; p < 16; p++) {
      for (int c = 0; c < 4; c++) {
        img->rgba[p * 4 + c] = (u8)std::clamp(pbr.baseColorFactor[c] * 255.0 + 0.5, 0.0, 255.0);
      }
    }
    return true;
  }
  return false;
}

// every triangle of every mesh in the file, in the file's space (meters, y up), joined
void pack_atlas(std::vector<Image>& images,
                const std::vector<int>& tri_image,
                std::vector<Vtx>* verts,
                std::vector<u32>* inds,
                Image* atlas);

bool load_model(const std::string& src,
                std::vector<Vtx>* verts,
                std::vector<u32>* inds,
                Image* img,
                int* materials,
                std::string* error) {
  tinygltf::TinyGLTF loader;
  tinygltf::Model model;
  std::string err, warn;
  std::string ext = fs::path(src).extension().string();
  std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
  const bool ok = ext == ".glb" ? loader.LoadBinaryFromFile(&model, &err, &warn, src)
                                : loader.LoadASCIIFromFile(&model, &err, &warn, src);
  if (!ok) {
    *error = "can't read the file: " + err;
    return false;
  }
  // (a file with no scene: its nodes as they are)
  auto nodes = gltf_util::flatten_nodes_from_all_scenes(model);
  if (nodes.empty()) {
    for (size_t i = 0; i < model.nodes.size(); i++) {
      nodes.push_back({(int)i, math::Matrix4f::identity()});
    }
  }
  std::vector<int> used_materials;
  std::vector<Image> images;      // each used material's picture
  std::vector<int> tri_image;     // each triangle's picture
  for (const auto& n : nodes) {
    const auto& node = model.nodes[n.node_idx];
    if (node.mesh < 0 || node.mesh >= (int)model.meshes.size()) {
      continue;
    }
    for (const auto& prim : model.meshes[node.mesh].primitives) {
      if (prim.mode != TINYGLTF_MODE_TRIANGLES) {
        continue;
      }
      auto pos_it = prim.attributes.find("POSITION");
      std::vector<float> pos, nrm, uv;
      if (pos_it == prim.attributes.end() || !read_floats(model, pos_it->second, 3, &pos)) {
        continue;
      }
      const size_t count = pos.size() / 3;
      auto nrm_it = prim.attributes.find("NORMAL");
      if (nrm_it == prim.attributes.end() || !read_floats(model, nrm_it->second, 3, &nrm) ||
          nrm.size() != count * 3) {
        nrm.clear();
      }
      auto uv_it = prim.attributes.find("TEXCOORD_0");
      if (uv_it == prim.attributes.end() || !read_floats(model, uv_it->second, 2, &uv) ||
          uv.size() != count * 2) {
        uv.assign(count * 2, 0.f);
      }
      std::vector<u32> idx;
      if (prim.indices >= 0) {
        if (!read_indices(model, prim.indices, &idx)) {
          continue;
        }
      } else {
        idx.resize(count);
        for (size_t i = 0; i < count; i++) {
          idx[i] = (u32)i;
        }
      }
      const u32 base = (u32)verts->size();
      for (size_t i = 0; i < count; i++) {
        Vtx v;
        math::Vector4f p = n.w_T_node * math::Vector4f(pos[i * 3], pos[i * 3 + 1], pos[i * 3 + 2], 1.f);
        math::Vector4f q = nrm.empty() ? math::Vector4f(0, 1, 0, 0)
                                       : n.w_T_node * math::Vector4f(nrm[i * 3], nrm[i * 3 + 1],
                                                                     nrm[i * 3 + 2], 0.f);
        const float len = std::sqrt(q.x() * q.x() + q.y() * q.y() + q.z() * q.z());
        for (int k = 0; k < 3; k++) {
          v.p[k] = p[k];
          v.n[k] = len > 0 ? q[k] / len : (k == 1 ? 1.f : 0.f);
        }
        v.uv[0] = uv[i * 2];
        v.uv[1] = uv[i * 2 + 1];
        verts->push_back(v);
      }
      // (its material's picture: one each, a plain grey for none)
      auto used = std::find(used_materials.begin(), used_materials.end(), prim.material);
      int image = (int)(used - used_materials.begin());
      if (used == used_materials.end()) {
        used_materials.push_back(prim.material);
        Image im;
        if (!material_image(model, prim.material, &im)) {
          im.w = im.h = 4;
          im.rgba.assign(4 * 4 * 4, 200);
        }
        images.push_back(std::move(im));
      }
      for (size_t i = 0; i + 2 < idx.size(); i += 3) {
        if (idx[i] < count && idx[i + 1] < count && idx[i + 2] < count) {
          for (int k = 0; k < 3; k++) {
            inds->push_back(base + idx[i + k]);
          }
          tri_image.push_back(image);
        }
      }
    }
  }
  *materials = (int)used_materials.size();
  if (inds->empty()) {
    *error = "no triangles in the file";
    return false;
  }
  if (images.size() == 1) {
    *img = std::move(images[0]);
  } else {
    // (the game draws a model with one texture: several made one atlas, the UVs moved into it)
    pack_atlas(images, tri_image, verts, inds, img);
  }
  // halved down to kTexMax at most (Tripo's are 4096: a model is never that big on screen)
  while (std::max(img->w, img->h) > kTexMax && img->w % 2 == 0 && img->h % 2 == 0) {
    const int w = img->w / 2, h = img->h / 2;
    std::vector<u8> half((size_t)w * h * 4);
    for (int y = 0; y < h; y++) {
      for (int x = 0; x < w; x++) {
        for (int c = 0; c < 4; c++) {
          auto at = [&](int dx, int dy) { return (u32)img->rgba[((size_t)(2 * y + dy) * img->w + 2 * x + dx) * 4 + c]; };
          half[((size_t)y * w + x) * 4 + c] = (u8)((at(0, 0) + at(1, 0) + at(0, 1) + at(1, 1) + 2) / 4);
        }
      }
    }
    img->rgba = std::move(half);
    img->w = w;
    img->h = h;
  }
  for (size_t p = 3; p < img->rgba.size() && !img->cutout; p += 4) {
    img->cutout = img->rgba[p] < 128;
  }
  return true;
}

// several pictures in one (rows of them, each with a border of its own edge pixels so the
// filtering doesn't bleed), each triangle's UVs moved into its picture's place. A triangle whose
// UVs repeat its picture is moved to the repeat it mostly lies in and held inside it
void pack_atlas(std::vector<Image>& images,
                const std::vector<int>& tri_image,
                std::vector<Vtx>* verts,
                std::vector<u32>* inds,
                Image* atlas) {
  constexpr int kPad = 4;
  // (all of them shrunk alike until they fit in kTexMax square)
  for (int tries = 0; tries < 8; tries++) {
    double area = 0;
    for (const auto& im : images) {
      area += (double)(im.w + 2 * kPad) * (im.h + 2 * kPad);
    }
    if (area < 0.7 * kTexMax * kTexMax) {
      break;
    }
    for (auto& im : images) {
      if (im.w >= 8 && im.h >= 8 && im.w % 2 == 0 && im.h % 2 == 0) {
        std::vector<u8> half((size_t)(im.w / 2) * (im.h / 2) * 4);
        for (int y = 0; y < im.h / 2; y++) {
          for (int x = 0; x < im.w / 2; x++) {
            for (int c = 0; c < 4; c++) {
              auto at = [&](int dx, int dy) { return (u32)im.rgba[((size_t)(2 * y + dy) * im.w + 2 * x + dx) * 4 + c]; };
              half[((size_t)y * (im.w / 2) + x) * 4 + c] = (u8)((at(0, 0) + at(1, 0) + at(0, 1) + at(1, 1) + 2) / 4);
            }
          }
        }
        im.rgba = std::move(half);
        im.w /= 2;
        im.h /= 2;
      }
    }
  }
  // (shelves: the tallest first)
  std::vector<int> order(images.size());
  for (size_t i = 0; i < order.size(); i++) {
    order[i] = (int)i;
  }
  std::sort(order.begin(), order.end(), [&](int a, int b) { return images[a].h > images[b].h; });
  int width = 64;
  for (const auto& im : images) {
    width = std::max(width, im.w + 2 * kPad);
  }
  {
    double area = 0;
    for (const auto& im : images) {
      area += (double)(im.w + 2 * kPad) * (im.h + 2 * kPad);
    }
    while (width < kTexMax && (double)width * width < area * 1.3) {
      width *= 2;
    }
  }
  std::vector<std::array<int, 2>> at(images.size());
  int x = 0, y = 0, shelf = 0;
  for (int i : order) {
    const int w = images[i].w + 2 * kPad, h = images[i].h + 2 * kPad;
    if (x + w > width) {
      x = 0;
      y += shelf;
      shelf = 0;
    }
    at[i] = {x + kPad, y + kPad};
    x += w;
    shelf = std::max(shelf, h);
  }
  int height = 1;
  while (height < y + shelf) {
    height *= 2;
  }
  atlas->w = width;
  atlas->h = height;
  atlas->rgba.assign((size_t)width * height * 4, 0);
  for (size_t i = 0; i < images.size(); i++) {
    const auto& im = images[i];
    for (int py = -kPad; py < im.h + kPad; py++) {
      for (int px = -kPad; px < im.w + kPad; px++) {
        const int sx = std::clamp(px, 0, im.w - 1), sy = std::clamp(py, 0, im.h - 1);
        const int dx = at[i][0] + px, dy = at[i][1] + py;
        if (dx < 0 || dy < 0 || dx >= width || dy >= height) {
          continue;
        }
        std::memcpy(&atlas->rgba[((size_t)dy * width + dx) * 4], &im.rgba[((size_t)sy * im.w + sx) * 4], 4);
      }
    }
  }
  // each triangle its own three vertices, their UVs in its picture's place (joined again later)
  std::vector<Vtx> out;
  std::vector<u32> out_i;
  out.reserve(inds->size());
  for (size_t t = 0; t + 2 < inds->size(); t += 3) {
    const int i = tri_image[t / 3];
    const auto& im = images[i];
    float lo[2] = {1e30f, 1e30f};
    for (int k = 0; k < 3; k++) {
      for (int c = 0; c < 2; c++) {
        lo[c] = std::min(lo[c], (*verts)[(*inds)[t + k]].uv[c]);
      }
    }
    // (the repeat the triangle lies in)
    const float shift[2] = {std::floor(lo[0] + 1e-4f), std::floor(lo[1] + 1e-4f)};
    for (int k = 0; k < 3; k++) {
      Vtx v = (*verts)[(*inds)[t + k]];
      const float u = std::clamp(v.uv[0] - shift[0], 0.f, 1.f), w = std::clamp(v.uv[1] - shift[1], 0.f, 1.f);
      v.uv[0] = (at[i][0] + u * im.w) / width;
      v.uv[1] = (at[i][1] + w * im.h) / height;
      out_i.push_back((u32)out.size());
      out.push_back(v);
    }
  }
  *verts = std::move(out);
  *inds = std::move(out_i);
}

// ---- the mesh

// the same vertices made one (the file's triangles each had their own, often)
void weld(std::vector<Vtx>* verts, std::vector<u32>* inds) {
  std::vector<u32> remap(verts->size());
  const size_t n =
      meshopt_generateVertexRemap(remap.data(), inds->data(), inds->size(), verts->data(), verts->size(), sizeof(Vtx));
  std::vector<Vtx> out(n);
  meshopt_remapVertexBuffer(out.data(), verts->data(), verts->size(), sizeof(Vtx), remap.data());
  meshopt_remapIndexBuffer(inds->data(), inds->data(), inds->size(), remap.data());
  *verts = std::move(out);
}

// down to `faces` faces (if more): edges collapsed where it shows least, and small loose pieces
// (a tuft's lone blades) dropped first; past what that can do, cells merged (sloppy)
std::vector<u32> reduce(const std::vector<Vtx>& verts, const std::vector<u32>& inds, size_t faces) {
  if (inds.size() / 3 <= faces) {
    return inds;
  }
  const size_t target = faces * 3;
  std::vector<u32> out(inds.size());
  float err = 0.f;
  size_t n = meshopt_simplify(out.data(), inds.data(), inds.size(), verts[0].p, verts.size(), sizeof(Vtx), target,
                              1.f, meshopt_SimplifyPrune, &err);
  // (pruning can take a model made of many small loose pieces away whole: then without it)
  if (n < target / 4) {
    n = meshopt_simplify(out.data(), inds.data(), inds.size(), verts[0].p, verts.size(), sizeof(Vtx), target,
                         1.f, 0, &err);
  }
  if (n > target + target / 8 || n < target / 4) {
    n = meshopt_simplifySloppy(out.data(), inds.data(), inds.size(), verts[0].p, verts.size(), sizeof(Vtx), target,
                               1.f, &err);
  }
  // (nothing that works: the faces as they are)
  if (n < 3) {
    return inds;
  }
  out.resize(n);
  return out;
}

// only the vertices the triangles use, in their order
void compact(const std::vector<Vtx>& verts,
             const std::vector<u32>& inds,
             std::vector<Vtx>* out_verts,
             std::vector<u32>* out_inds) {
  std::vector<u32> map(verts.size(), UINT32_MAX);
  out_verts->clear();
  out_inds->resize(inds.size());
  for (size_t i = 0; i < inds.size(); i++) {
    u32& m = map[inds[i]];
    if (m == UINT32_MAX) {
      m = (u32)out_verts->size();
      out_verts->push_back(verts[inds[i]]);
    }
    (*out_inds)[i] = m;
  }
}

// ---- writing

std::vector<u8> png_bytes(const u8* rgba, int w, int h) {
  std::vector<u8> out;
  stbi_write_png_to_func(
      [](void* ctx, void* data, int size) {
        auto* o = (std::vector<u8>*)ctx;
        o->insert(o->end(), (u8*)data, (u8*)data + size);
      },
      &out, w, h, 4, rgba, w * 4);
  return out;
}

// one mesh, one material (the texture inside the file), as the scenery reads it
bool write_glb(const fs::path& path,
               const std::vector<Vtx>& verts,
               const std::vector<u32>& inds,
               const std::vector<u8>& png,
               bool cutout) {
  tinygltf::Model m;
  m.asset.version = "2.0";
  m.asset.generator = "Jak Enhanced editor";
  tinygltf::Buffer buf;
  auto add_view = [&](const void* data, size_t size, int target) {
    while (buf.data.size() % 4) {
      buf.data.push_back(0);
    }
    tinygltf::BufferView bv;
    bv.buffer = 0;
    bv.byteOffset = buf.data.size();
    bv.byteLength = size;
    bv.target = target;
    buf.data.insert(buf.data.end(), (const u8*)data, (const u8*)data + size);
    m.bufferViews.push_back(bv);
    return (int)m.bufferViews.size() - 1;
  };
  std::vector<float> pos, nrm, uv;
  std::array<double, 3> lo = {1e30, 1e30, 1e30}, hi = {-1e30, -1e30, -1e30};
  for (const auto& v : verts) {
    for (int k = 0; k < 3; k++) {
      pos.push_back(v.p[k]);
      nrm.push_back(v.n[k]);
      lo[k] = std::min(lo[k], (double)v.p[k]);
      hi[k] = std::max(hi[k], (double)v.p[k]);
    }
    uv.push_back(v.uv[0]);
    uv.push_back(v.uv[1]);
  }
  auto add_accessor = [&](int view, int comp, int type, size_t count) {
    tinygltf::Accessor a;
    a.bufferView = view;
    a.componentType = comp;
    a.type = type;
    a.count = count;
    m.accessors.push_back(a);
    return (int)m.accessors.size() - 1;
  };
  const int a_pos = add_accessor(add_view(pos.data(), pos.size() * 4, TINYGLTF_TARGET_ARRAY_BUFFER),
                                 TINYGLTF_COMPONENT_TYPE_FLOAT, TINYGLTF_TYPE_VEC3, verts.size());
  m.accessors[a_pos].minValues = {lo[0], lo[1], lo[2]};
  m.accessors[a_pos].maxValues = {hi[0], hi[1], hi[2]};
  const int a_nrm = add_accessor(add_view(nrm.data(), nrm.size() * 4, TINYGLTF_TARGET_ARRAY_BUFFER),
                                 TINYGLTF_COMPONENT_TYPE_FLOAT, TINYGLTF_TYPE_VEC3, verts.size());
  const int a_uv = add_accessor(add_view(uv.data(), uv.size() * 4, TINYGLTF_TARGET_ARRAY_BUFFER),
                                TINYGLTF_COMPONENT_TYPE_FLOAT, TINYGLTF_TYPE_VEC2, verts.size());
  const int a_ind = add_accessor(add_view(inds.data(), inds.size() * 4, TINYGLTF_TARGET_ELEMENT_ARRAY_BUFFER),
                                 TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT, TINYGLTF_TYPE_SCALAR, inds.size());
  tinygltf::Image image;
  image.mimeType = "image/png";
  image.bufferView = add_view(png.data(), png.size(), 0);
  m.bufferViews.back().target = 0;
  m.images.push_back(image);
  m.buffers.push_back(std::move(buf));
  tinygltf::Sampler sampler;
  sampler.magFilter = TINYGLTF_TEXTURE_FILTER_LINEAR;
  sampler.minFilter = TINYGLTF_TEXTURE_FILTER_LINEAR_MIPMAP_LINEAR;
  m.samplers.push_back(sampler);
  tinygltf::Texture tex;
  tex.source = 0;
  tex.sampler = 0;
  m.textures.push_back(tex);
  tinygltf::Material mat;
  mat.pbrMetallicRoughness.baseColorTexture.index = 0;
  mat.pbrMetallicRoughness.metallicFactor = 0.0;
  mat.pbrMetallicRoughness.roughnessFactor = 1.0;
  mat.doubleSided = true;
  if (cutout) {
    mat.alphaMode = "MASK";
    mat.alphaCutoff = 0.5;
  }
  m.materials.push_back(mat);
  tinygltf::Primitive prim;
  prim.attributes["POSITION"] = a_pos;
  prim.attributes["NORMAL"] = a_nrm;
  prim.attributes["TEXCOORD_0"] = a_uv;
  prim.indices = a_ind;
  prim.material = 0;
  prim.mode = TINYGLTF_MODE_TRIANGLES;
  tinygltf::Mesh mesh;
  mesh.name = path.stem().string();
  mesh.primitives.push_back(prim);
  m.meshes.push_back(mesh);
  tinygltf::Node node;
  node.mesh = 0;
  node.name = mesh.name;
  m.nodes.push_back(node);
  tinygltf::Scene scene;
  scene.nodes.push_back(0);
  m.scenes.push_back(scene);
  m.defaultScene = 0;
  tinygltf::TinyGLTF writer;
  return writer.WriteGltfSceneToFile(&m, path.string(), true, true, false, true);
}

// ---- the impostor: the model drawn (unlit, its texture's colors) from kImpGrid x kImpGrid
// directions of the upper hemisphere, and its normals, each in its cell of an atlas (as the game
// picks them: EnhancedScenery, shrub_imp.vert; Blender's scenery_import.py draws the same)

struct V3 {
  float x, y, z;
};
V3 sub(V3 a, V3 b) {
  return {a.x - b.x, a.y - b.y, a.z - b.z};
}
float dot(V3 a, V3 b) {
  return a.x * b.x + a.y * b.y + a.z * b.z;
}
V3 cross(V3 a, V3 b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
V3 norm(V3 a) {
  const float l = std::sqrt(dot(a, a));
  return l > 0 ? V3{a.x / l, a.y / l, a.z / l} : a;
}

// the view direction of a cell's middle (u, v in 0..1: from the bottom left)
V3 hemi_oct_dir(float u, float v) {
  const float px = u * 2 - 1, py = v * 2 - 1;
  const float x = (px + py) * 0.5f, z = (px - py) * 0.5f;
  return norm({x, 1 - std::abs(x) - std::abs(z), z});
}

void sample(const Image& img, float u, float v, float out[4]) {
  // (bilinear, repeating)
  const float fx = u * img.w - 0.5f, fy = v * img.h - 0.5f;
  const int x0 = (int)std::floor(fx), y0 = (int)std::floor(fy);
  const float tx = fx - x0, ty = fy - y0;
  auto texel = [&](int x, int y, int c) {
    x = ((x % img.w) + img.w) % img.w;
    y = ((y % img.h) + img.h) % img.h;
    return (float)img.rgba[((size_t)y * img.w + x) * 4 + c];
  };
  for (int c = 0; c < 4; c++) {
    const float a = texel(x0, y0, c) + (texel(x0 + 1, y0, c) - texel(x0, y0, c)) * tx;
    const float b = texel(x0, y0 + 1, c) + (texel(x0 + 1, y0 + 1, c) - texel(x0, y0 + 1, c)) * tx;
    out[c] = (a + (b - a) * ty) / 255.f;
  }
}

void bake_impostor(const std::vector<Vtx>& verts,
                   const std::vector<u32>& inds,
                   const Image& img,
                   const fs::path& color_path,
                   const fs::path& normal_path) {
  V3 lo = {1e30f, 1e30f, 1e30f}, hi = {-1e30f, -1e30f, -1e30f};
  for (const auto& v : verts) {
    lo = {std::min(lo.x, v.p[0]), std::min(lo.y, v.p[1]), std::min(lo.z, v.p[2])};
    hi = {std::max(hi.x, v.p[0]), std::max(hi.y, v.p[1]), std::max(hi.z, v.p[2])};
  }
  const V3 center = {(lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f, (lo.z + hi.z) * 0.5f};
  const float r = std::max(std::sqrt(dot(sub(hi, lo), sub(hi, lo))) * 0.5f, 1e-4f);
  const int size = kImpGrid * kImpCell;
  std::vector<u8> color((size_t)size * size * 4, 0), normal((size_t)size * size * 4, 0);
  const int n = kImpCell * kImpSuper;  // a cell's samples across
  std::vector<float> depth((size_t)n * n);
  std::vector<std::array<float, 3>> scol((size_t)n * n), snrm((size_t)n * n);
  struct P {
    float x, y, d;
  };
  std::vector<P> proj(verts.size());
  for (int j = 0; j < kImpGrid; j++) {
    for (int i = 0; i < kImpGrid; i++) {
      const V3 d = hemi_oct_dir((i + 0.5f) / kImpGrid, (j + 0.5f) / kImpGrid);
      const V3 f = {-d.x, -d.y, -d.z};
      const V3 right = norm(cross(f, {0, 1, 0}));
      const V3 up = cross(right, f);
      // (orthographic, 2r across: x right, y up from the cell's bottom, depth along f)
      for (size_t k = 0; k < verts.size(); k++) {
        const V3 rel = sub({verts[k].p[0], verts[k].p[1], verts[k].p[2]}, center);
        proj[k] = {(dot(rel, right) / (2 * r) + 0.5f) * n, (dot(rel, up) / (2 * r) + 0.5f) * n, dot(rel, f)};
      }
      std::fill(depth.begin(), depth.end(), 1e30f);
      for (size_t t = 0; t + 2 < inds.size(); t += 3) {
        const P& a = proj[inds[t]];
        const P& b = proj[inds[t + 1]];
        const P& c = proj[inds[t + 2]];
        const float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
        if (std::abs(area) < 1e-12f) {
          continue;
        }
        const int x0 = std::max(0, (int)std::floor(std::min({a.x, b.x, c.x})));
        const int x1 = std::min(n - 1, (int)std::ceil(std::max({a.x, b.x, c.x})));
        const int y0 = std::max(0, (int)std::floor(std::min({a.y, b.y, c.y})));
        const int y1 = std::min(n - 1, (int)std::ceil(std::max({a.y, b.y, c.y})));
        const Vtx& va = verts[inds[t]];
        const Vtx& vb = verts[inds[t + 1]];
        const Vtx& vc = verts[inds[t + 2]];
        for (int y = y0; y <= y1; y++) {
          for (int x = x0; x <= x1; x++) {
            // (both faces: barycentric weights by the signed area)
            const float px = x + 0.5f, py = y + 0.5f;
            const float w0 = ((b.x - px) * (c.y - py) - (b.y - py) * (c.x - px)) / area;
            const float w1 = ((c.x - px) * (a.y - py) - (c.y - py) * (a.x - px)) / area;
            const float w2 = 1.f - w0 - w1;
            if (w0 < 0 || w1 < 0 || w2 < 0) {
              continue;
            }
            const float z = w0 * a.d + w1 * b.d + w2 * c.d;
            const size_t s = (size_t)y * n + x;
            if (z >= depth[s]) {
              continue;
            }
            float texel[4];
            sample(img, w0 * va.uv[0] + w1 * vb.uv[0] + w2 * vc.uv[0],
                   w0 * va.uv[1] + w1 * vb.uv[1] + w2 * vc.uv[1], texel);
            if (img.cutout && texel[3] < 0.5f) {
              continue;
            }
            depth[s] = z;
            scol[s] = {texel[0], texel[1], texel[2]};
            const V3 nn = norm({w0 * va.n[0] + w1 * vb.n[0] + w2 * vc.n[0], w0 * va.n[1] + w1 * vb.n[1] + w2 * vc.n[1],
                                w0 * va.n[2] + w1 * vb.n[2] + w2 * vc.n[2]});
            snrm[s] = {nn.x, nn.y, nn.z};
          }
        }
      }
      // the samples down to the cell's pixels: the colors of what's covered, alpha its share
      for (int y = 0; y < kImpCell; y++) {
        for (int x = 0; x < kImpCell; x++) {
          float c[3] = {0, 0, 0}, nn[3] = {0, 0, 0};
          int covered = 0;
          for (int sy = 0; sy < kImpSuper; sy++) {
            for (int sx = 0; sx < kImpSuper; sx++) {
              const size_t s = (size_t)(y * kImpSuper + sy) * n + x * kImpSuper + sx;
              if (depth[s] < 1e30f) {
                covered++;
                for (int k = 0; k < 3; k++) {
                  c[k] += scol[s][k];
                  nn[k] += snrm[s][k];
                }
              }
            }
          }
          // (the png from the top: cell row j counted from the bottom, y up)
          const int row = size - 1 - (j * kImpCell + y);
          const size_t o = ((size_t)row * size + i * kImpCell + x) * 4;
          if (!covered) {
            continue;
          }
          const V3 nv = norm({nn[0], nn[1], nn[2]});
          const float nf[3] = {nv.x, nv.y, nv.z};
          for (int k = 0; k < 3; k++) {
            color[o + k] = (u8)std::clamp(c[k] / covered * 255.f + 0.5f, 0.f, 255.f);
            normal[o + k] = (u8)std::clamp((nf[k] * 0.5f + 0.5f) * 255.f + 0.5f, 0.f, 255.f);
          }
          color[o + 3] = normal[o + 3] = (u8)(255 * covered / (kImpSuper * kImpSuper));
        }
      }
    }
  }
  // the colors spread a few pixels into the empty ones around (alpha stays 0): filtered, the
  // edges don't darken
  for (auto* atlas : {&color, &normal}) {
    for (int pass = 0; pass < 4; pass++) {
      std::vector<u8> next = *atlas;
      for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
          const size_t o = ((size_t)y * size + x) * 4;
          if ((*atlas)[o + 3] || ((*atlas)[o] | (*atlas)[o + 1] | (*atlas)[o + 2])) {
            continue;
          }
          int sum[3] = {0, 0, 0}, cnt = 0;
          for (int dy = -1; dy <= 1; dy++) {
            for (int dx = -1; dx <= 1; dx++) {
              const int xx = x + dx, yy = y + dy;
              // (not across into the next cell)
              if (xx < 0 || yy < 0 || xx >= size || yy >= size || xx / kImpCell != x / kImpCell ||
                  yy / kImpCell != y / kImpCell) {
                continue;
              }
              const size_t q = ((size_t)yy * size + xx) * 4;
              if ((*atlas)[q + 3] || ((*atlas)[q] | (*atlas)[q + 1] | (*atlas)[q + 2])) {
                for (int k = 0; k < 3; k++) {
                  sum[k] += (*atlas)[q + k];
                }
                cnt++;
              }
            }
          }
          if (cnt) {
            for (int k = 0; k < 3; k++) {
              next[o + k] = (u8)(sum[k] / cnt);
            }
          }
        }
      }
      *atlas = std::move(next);
    }
  }
  file_util::write_rgba_png(color_path, color.data(), size, size);
  file_util::write_rgba_png(normal_path, normal.data(), size, size);
}

}  // namespace

EnhancedImportResult enhanced_import_model(const std::string& src,
                                           const std::string& out_dir,
                                           const std::string& name,
                                           int max_faces,
                                           float scale) {
  EnhancedImportResult res;
  try {
    std::vector<Vtx> verts;
    std::vector<u32> inds;
    Image img;
    if (!load_model(src, &verts, &inds, &img, &res.materials, &res.error)) {
      return res;
    }
    res.faces_in = (int)(inds.size() / 3);
    // (the file's units to meters: the game's own models are in quarter meters)
    if (scale != 1.f) {
      for (auto& v : verts) {
        for (int k = 0; k < 3; k++) {
          v.p[k] *= scale;
        }
      }
    }
    // stood on the origin: the middle of its bounds, its lowest point at 0
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    for (const auto& v : verts) {
      for (int k = 0; k < 3; k++) {
        lo[k] = std::min(lo[k], v.p[k]);
        hi[k] = std::max(hi[k], v.p[k]);
      }
    }
    const float shift[3] = {(lo[0] + hi[0]) * 0.5f, lo[1], (lo[2] + hi[2]) * 0.5f};
    std::copy(shift, shift + 3, res.origin);
    for (auto& v : verts) {
      for (int k = 0; k < 3; k++) {
        v.p[k] -= shift[k];
      }
    }
    for (int k = 0; k < 3; k++) {
      res.size[k] = hi[k] - lo[k];
    }
    weld(&verts, &inds);
    std::vector<Vtx> model_v, far_v;
    std::vector<u32> model_i, far_i;
    compact(verts, reduce(verts, inds, (size_t)std::max(max_faces, 1)), &model_v, &model_i);
    res.faces = (int)(model_i.size() / 3);
    const size_t far_faces = std::max((size_t)(res.faces * kLodRatio), (size_t)std::min(res.faces, kLodMin));
    compact(model_v, reduce(model_v, model_i, far_faces), &far_v, &far_i);
    res.lod_faces = (int)(far_i.size() / 3);
    if (model_i.empty() || far_i.empty()) {
      res.error = "nothing left after reducing it";
      return res;
    }
    const fs::path dir(out_dir);
    fs::create_directories(dir);
    const auto png = png_bytes(img.rgba.data(), img.w, img.h);
    if (!write_glb(dir / (name + ".glb"), model_v, model_i, png, img.cutout) ||
        !write_glb(dir / (name + "_lod1.glb"), far_v, far_i, png, img.cutout)) {
      res.error = "can't write the model in " + out_dir;
      return res;
    }
    bake_impostor(model_v, model_i, img, dir / (name + "_imp.png"), dir / (name + "_nrm.png"));
    res.ok = true;
  } catch (const std::exception& ex) {
    res.error = ex.what();
  }
  return res;
}

std::string enhanced_export_level_glb(const std::vector<EnhancedLevelObject>& objs,
                                      const std::string& scenery_dir,
                                      const std::string& out_path,
                                      int* models) {
  *models = 0;
  tinygltf::Model out;
  out.asset.version = "2.0";
  out.asset.generator = "Jak Enhanced Trilogy editor";
  out.buffers.resize(1);
  auto& bin = out.buffers[0].data;
  tinygltf::Scene scene;
  // (a view of the source's bytes copied over, 4-aligned)
  auto copy_view = [&](const tinygltf::Model& src, int v) {
    const auto& sv = src.bufferViews.at(v);
    const auto& sb = src.buffers.at(sv.buffer).data;
    while (bin.size() % 4) {
      bin.push_back(0);
    }
    tinygltf::BufferView nv = sv;
    nv.buffer = 0;
    nv.byteOffset = bin.size();
    bin.insert(bin.end(), sb.begin() + sv.byteOffset, sb.begin() + sv.byteOffset + sv.byteLength);
    out.bufferViews.push_back(nv);
    return (int)out.bufferViews.size() - 1;
  };
  struct Copied {
    int mesh = -1;
    float foot[3] = {0, 0, 0};
  };
  std::unordered_map<std::string, Copied> copied;
  tinygltf::TinyGLTF io;
  for (const auto& o : objs) {
    auto it = copied.find(o.model);
    if (it == copied.end()) {
      Copied c;
      tinygltf::Model src;
      std::string err, warn;
      const auto path = (fs::path(scenery_dir) / o.model).string();
      if (!io.LoadBinaryFromFile(&src, &err, &warn, path) || src.meshes.empty()) {
        lg::warn("[enhanced] level export: {} not read ({})", o.model, err);
        copied[o.model] = c;
        continue;
      }
      // (its images, samplers, textures, materials: each copied, the indices moved on)
      std::vector<int> img_map, tex_map, mat_map, samp_map;
      for (const auto& im : src.images) {
        tinygltf::Image ni;
        ni.name = im.name;
        ni.mimeType = im.mimeType.empty() ? "image/png" : im.mimeType;
        if (im.bufferView >= 0) {
          ni.bufferView = copy_view(src, im.bufferView);
        } else if (!im.image.empty()) {
          // (decoded only: written again as a PNG)
          ni.image = im.image;
          ni.width = im.width;
          ni.height = im.height;
          ni.component = im.component;
          ni.bits = im.bits;
          ni.pixel_type = im.pixel_type;
        }
        out.images.push_back(ni);
        img_map.push_back((int)out.images.size() - 1);
      }
      for (const auto& s : src.samplers) {
        out.samplers.push_back(s);
        samp_map.push_back((int)out.samplers.size() - 1);
      }
      for (auto tx : src.textures) {
        if (tx.source >= 0) {
          tx.source = img_map.at(tx.source);
        }
        if (tx.sampler >= 0) {
          tx.sampler = samp_map.at(tx.sampler);
        }
        out.textures.push_back(tx);
        tex_map.push_back((int)out.textures.size() - 1);
      }
      for (auto m : src.materials) {
        auto& bc = m.pbrMetallicRoughness.baseColorTexture;
        if (bc.index >= 0) {
          bc.index = tex_map.at(bc.index);
        }
        m.normalTexture.index = -1;
        m.occlusionTexture.index = -1;
        m.emissiveTexture.index = -1;
        m.pbrMetallicRoughness.metallicRoughnessTexture.index = -1;
        out.materials.push_back(m);
        mat_map.push_back((int)out.materials.size() - 1);
      }
      // (the first mesh: the scenery's models have one)
      tinygltf::Mesh mesh = src.meshes[0];
      float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
      for (auto& prim : mesh.primitives) {
        std::map<std::string, int> attrs;
        for (const auto& [key, acc] : prim.attributes) {
          // (no skin: the level is still)
          if (key.rfind("JOINTS", 0) == 0 || key.rfind("WEIGHTS", 0) == 0) {
            continue;
          }
          auto a = src.accessors.at(acc);
          if (key == "POSITION" && a.minValues.size() == 3 && a.maxValues.size() == 3) {
            for (int k = 0; k < 3; k++) {
              lo[k] = std::min(lo[k], (float)a.minValues[k]);
              hi[k] = std::max(hi[k], (float)a.maxValues[k]);
            }
          }
          a.bufferView = copy_view(src, a.bufferView);
          out.accessors.push_back(a);
          attrs[key] = (int)out.accessors.size() - 1;
        }
        prim.attributes = attrs;
        if (prim.indices >= 0) {
          auto a = src.accessors.at(prim.indices);
          a.bufferView = copy_view(src, a.bufferView);
          out.accessors.push_back(a);
          prim.indices = (int)out.accessors.size() - 1;
        }
        if (prim.material >= 0) {
          prim.material = mat_map.at(prim.material);
        }
        prim.targets.clear();
      }
      mesh.name = fs::path(o.model).stem().string();
      out.meshes.push_back(mesh);
      c.mesh = (int)out.meshes.size() - 1;
      // (its foot: the middle of its bottom, where the editor stands it)
      if (lo[0] < 1e29f) {
        c.foot[0] = (lo[0] + hi[0]) * 0.5f;
        c.foot[1] = lo[1];
        c.foot[2] = (lo[2] + hi[2]) * 0.5f;
      }
      it = copied.emplace(o.model, c).first;
      (*models)++;
    }
    if (it->second.mesh < 0) {
      continue;
    }
    // the node: the turned and scaled axes, its foot at pos (column-major)
    const auto& c = it->second;
    tinygltf::Node node;
    node.mesh = c.mesh;
    node.name = it->second.mesh >= 0 ? out.meshes[c.mesh].name : o.model;
    node.matrix.assign(16, 0.0);
    for (int col = 0; col < 3; col++) {
      for (int r = 0; r < 3; r++) {
        node.matrix[col * 4 + r] = o.cols[col][r];
      }
    }
    for (int r = 0; r < 3; r++) {
      double t = o.pos[r];
      for (int col = 0; col < 3; col++) {
        t -= o.cols[col][r] * c.foot[col];
      }
      node.matrix[12 + r] = t;
    }
    node.matrix[15] = 1.0;
    out.nodes.push_back(node);
    scene.nodes.push_back((int)out.nodes.size() - 1);
  }
  if (scene.nodes.empty()) {
    return "no models to write";
  }
  out.scenes.push_back(scene);
  out.defaultScene = 0;
  fs::create_directories(fs::path(out_path).parent_path());
  if (!io.WriteGltfSceneToFile(&out, out_path, true, true, false, true)) {
    return "couldn't write " + out_path;
  }
  return "";
}
