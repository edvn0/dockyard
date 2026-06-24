-- LuaCATS type annotations for the dockyard C++ API (sol2 bindings).
-- These are declaration-only — the actual implementations are in C++.
-- lua-language-server reads these to provide hover, completion, and type checking.

---@meta

-- ---------------------------------------------------------------------------
-- Math
-- ---------------------------------------------------------------------------

---@class vec3
---@field x number
---@field y number
---@field z number
---@operator add(vec3): vec3
---@operator sub(vec3): vec3
---@operator mul(vec3|number): vec3
---@operator unm: vec3
vec3 = {}

---@param x number
---@param y number
---@param z number
---@return vec3
function vec3(x, y, z) end

---@class vec4
---@field x number
---@field y number
---@field z number
---@field w number
vec4 = {}

---@param x number
---@param y number
---@param z number
---@param w number
---@return vec4
function vec4(x, y, z, w) end

---@class quat
---@field x number
---@field y number
---@field z number
---@field w number
quat = {}

---@param x number
---@param y number
---@param z number
---@param w number
---@return quat
function quat(x, y, z, w) end

---@param x number  Euler angle in radians
---@param y number
---@param z number
---@return quat
function quat_from_euler(x, y, z) end

-- ---------------------------------------------------------------------------
-- VFS
-- ---------------------------------------------------------------------------

---@class VFSPath
VFSPath = {}

---@param path string
---@return VFSPath
function VFSPath.create(path) end

---@return string
function VFSPath:view() end

-- ---------------------------------------------------------------------------
-- Handles
-- ---------------------------------------------------------------------------

---@class MeshAssetHandle
MeshAssetHandle = {}

---@return number
function MeshAssetHandle:index() end

---@return boolean
function MeshAssetHandle:valid() end

-- ---------------------------------------------------------------------------
-- AnimationState — opaque; create via assets:make_animation_state()
-- ---------------------------------------------------------------------------

---@class AnimationState
AnimationState = {}

-- ---------------------------------------------------------------------------
-- Components
-- ---------------------------------------------------------------------------

---@class Mesh
---@field handle MeshAssetHandle
Mesh = {}

---@param path string
function Mesh:set_source_path(path) end

---@class Transform
---@field position vec3
---@field rotation quat
---@field scale vec3
Transform = {}

---@class MaterialOverride
MaterialOverride = {}

---@param r number
---@param g number
---@param b number
---@param a number
function MaterialOverride:set_albedo(r, g, b, a) end

---@param v number
function MaterialOverride:set_roughness(v) end

---@param v number
function MaterialOverride:set_metallic(v) end

---@class PointLight
---@field color vec3
---@field intensity number
---@field radius number
PointLight = {}

-- ---------------------------------------------------------------------------
-- Entity
-- ---------------------------------------------------------------------------

---@class Entity
Entity = {}

---@return number
function Entity:id() end

---@return boolean
function Entity:valid() end

---@return Mesh
function Entity:add_mesh() end

---@return Mesh|nil
function Entity:get_mesh() end

---@return Transform
function Entity:get_transform() end

---@return MaterialOverride
function Entity:add_material_override() end

---@return PointLight
function Entity:add_point_light() end

---@param state AnimationState
---@return AnimationState
function Entity:add_animation_state(state) end

-- ---------------------------------------------------------------------------
-- Scene
-- ---------------------------------------------------------------------------

---@class Scene
Scene = {}

---@param name string
---@param parent? Entity
---@return Entity
function Scene:make(name, parent) end

---@param id number
function Scene:destroy_entity(id) end

---@param id number
---@return boolean
function Scene:is_valid(id) end

---@param id number
---@return Transform|nil
function Scene:get_transform(id) end

---@param callback fun(id: number, transform: Transform, mesh: Mesh)
function Scene:each_mesh(callback) end

-- ---------------------------------------------------------------------------
-- IAssetLoader
-- ---------------------------------------------------------------------------

---@class IAssetLoader
IAssetLoader = {}

---@param path VFSPath
---@return MeshAssetHandle|nil, string|nil
function IAssetLoader:load_mesh(path) end

---@param handle MeshAssetHandle
---@param skel_idx number  0-based skeleton index
---@param clip_idx number  0-based animation clip index
---@return AnimationState|nil, string|nil
function IAssetLoader:make_animation_state(handle, skel_idx, clip_idx) end

function IAssetLoader:notify_material_overrides_added() end
