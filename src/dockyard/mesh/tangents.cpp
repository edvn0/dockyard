#include <dockyard/mesh/tangents.hpp>

#include <mikktspace.h>

#include <glm/glm.hpp>
#include <glm/packing.hpp>

#include <vector>

namespace dy::mesh::detail {

namespace {

struct MikkContext {
  PrimitiveData *prim = nullptr;
  std::vector<glm::vec3> positions;
  std::vector<glm::vec3> normals;
  std::vector<glm::vec2> uvs;
};

auto mikk_get_num_faces(const SMikkTSpaceContext *ctx) -> int {
  auto *c = static_cast<MikkContext *>(ctx->m_pUserData);
  return static_cast<int>(c->prim->indices.size() / 3);
}

auto mikk_get_num_verts_of_face(const SMikkTSpaceContext *, int) -> int {
  return 3;
}

void mikk_get_position(const SMikkTSpaceContext *ctx, float out[], int face,
                       int vert) {
  auto *c = static_cast<MikkContext *>(ctx->m_pUserData);
  u32 idx = c->prim->indices[static_cast<u32>(face * 3 + vert)];
  auto &p = c->positions[idx];
  out[0] = p.x;
  out[1] = p.y;
  out[2] = p.z;
}

void mikk_get_normal(const SMikkTSpaceContext *ctx, float out[], int face,
                     int vert) {
  auto *c = static_cast<MikkContext *>(ctx->m_pUserData);
  u32 idx = c->prim->indices[static_cast<u32>((face * 3) + vert)];
  auto &n = c->normals[idx];
  out[0] = n.x;
  out[1] = n.y;
  out[2] = n.z;
}

void mikk_get_uv(const SMikkTSpaceContext *ctx, float out[], int face,
                 int vert) {
  auto *c = static_cast<MikkContext *>(ctx->m_pUserData);
  u32 idx = c->prim->indices[static_cast<u32>((face * 3) + vert)];
  auto &uv = c->uvs[idx];
  out[0] = uv.x;
  out[1] = uv.y;
}

void mikk_set_tspace(const SMikkTSpaceContext *ctx, const float tangent[],
                     float sign, int face, int vert) {
  auto *c = static_cast<MikkContext *>(ctx->m_pUserData);
  u32 idx = c->prim->indices[static_cast<u32>(face * 3 + vert)];

  c->prim->vertices[idx].tangent =
      glm::packSnorm4x8(glm::vec4{tangent[0], tangent[1], tangent[2], sign});

  glm::vec3 n = c->normals[idx];
  glm::vec3 t{tangent[0], tangent[1], tangent[2]};
  glm::vec3 bt = glm::normalize(glm::cross(n, t)) * sign;
  c->prim->vertices[idx].bitangent = glm::packSnorm4x8(glm::vec4{bt, 0.0f});
}

} // namespace

auto generate_mikktspace_tangents(PrimitiveData &prim)
    -> std::expected<void, std::string> {
  MikkContext mctx;
  mctx.prim = &prim;

  const usize vtx_count = prim.vertices.size();
  mctx.positions.resize(vtx_count);
  mctx.normals.resize(vtx_count);
  mctx.uvs.resize(vtx_count);

  for (usize i = 0; i < vtx_count; ++i) {
    auto &v = prim.vertices[i];
    mctx.positions[i] = {v.position[0], v.position[1], v.position[2]};
    mctx.normals[i] = glm::unpackSnorm4x8(v.normal);
    mctx.uvs[i] = glm::unpackHalf2x16(v.uvs);
  }

  SMikkTSpaceInterface iface{};
  iface.m_getNumFaces = mikk_get_num_faces;
  iface.m_getNumVerticesOfFace = mikk_get_num_verts_of_face;
  iface.m_getPosition = mikk_get_position;
  iface.m_getNormal = mikk_get_normal;
  iface.m_getTexCoord = mikk_get_uv;
  iface.m_setTSpaceBasic = mikk_set_tspace;

  SMikkTSpaceContext ctx{};
  ctx.m_pInterface = &iface;
  ctx.m_pUserData = &mctx;

  if (genTangSpaceDefault(&ctx) == 0)
    return std::unexpected("mikktspace tangent generation failed");

  return {};
}

} // namespace dy::mesh::detail
