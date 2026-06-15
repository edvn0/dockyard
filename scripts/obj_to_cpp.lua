#!/usr/bin/env lua
-- obj_to_cpp.lua
-- Converts a Wavefront OBJ to a C++ vertex/index array using the
-- packed Vertex format (position, pack_uv, pack_normal x3).
--
-- Usage: lua obj_to_cpp.lua <input.obj> [var_name] > output.hpp
--
-- Tangents are computed with Lengyel's method and averaged per vertex.
-- If the OBJ has no normals, face normals are computed and averaged.
-- Polygons are fan-triangulated.

-- ── math ─────────────────────────────────────────────────────────────────────

local function sub3(a, b) return { a[1]-b[1], a[2]-b[2], a[3]-b[3] } end
local function add3(a, b) return { a[1]+b[1], a[2]+b[2], a[3]+b[3] } end
local function scale3(v, s) return { v[1]*s, v[2]*s, v[3]*s } end
local function dot3(a, b) return a[1]*b[1] + a[2]*b[2] + a[3]*b[3] end
local function cross3(a, b)
    return { a[2]*b[3]-a[3]*b[2], a[3]*b[1]-a[1]*b[3], a[1]*b[2]-a[2]*b[1] }
end
local function norm3(v)
    local l = math.sqrt(dot3(v, v))
    return l > 1e-12 and scale3(v, 1/l) or { 1, 0, 0 }
end

-- ── formatting ────────────────────────────────────────────────────────────────

local function fmt(n)
    local s = string.format("%.6g", n)
    -- Integers like "0" or "1" need ".0" inserted; scientific notation like
    -- "1e-05" is fine without a dot. Only bare integers lack both.
    if not s:find("[%.eE]") then s = s .. ".0" end
    return s .. "f"
end
local function v3(t) return string.format("{%s, %s, %s}", fmt(t[1]), fmt(t[2]), fmt(t[3])) end
local function v2(t) return string.format("{%s, %s}", fmt(t[1]), fmt(t[2])) end

-- ── OBJ parser ────────────────────────────────────────────────────────────────

local function parse_obj(path)
    local pos_list    = {}   -- {x,y,z}
    local uv_list     = {}   -- {u,v}
    local nrm_list    = {}   -- {x,y,z}
    local groups      = {}   -- { name=str, faces={} }
    local has_normals = false

    local cur = { name = "Default", faces = {} }
    table.insert(groups, cur)

    local fh = assert(io.open(path, "r"), "Cannot open: " .. path)
    for raw in fh:lines() do
        local line = raw:match("^%s*(.-)%s*$")
        local tok  = line:match("^(%S+)")
        if not tok or tok:sub(1,1) == "#" then
            -- blank / comment
        elseif tok == "v" then
            local a, b, c = line:match("^v%s+(%S+)%s+(%S+)%s+(%S+)")
            table.insert(pos_list, { tonumber(a), tonumber(b), tonumber(c) })
        elseif tok == "vt" then
            local a, b = line:match("^vt%s+(%S+)%s+(%S+)")
            table.insert(uv_list, { tonumber(a), tonumber(b) or 0 })
        elseif tok == "vn" then
            local a, b, c = line:match("^vn%s+(%S+)%s+(%S+)%s+(%S+)")
            table.insert(nrm_list, { tonumber(a), tonumber(b), tonumber(c) })
            has_normals = true
        elseif tok == "g" or tok == "o" then
            local name = line:match("^[go]%s+(.+)$")
            if name then name = name:match("^%s*(.-)%s*$") end
            if name and name ~= "" then
                if #cur.faces > 0 then
                    cur = { name = name, faces = {} }
                    table.insert(groups, cur)
                else
                    cur.name = name
                end
            end
        elseif tok == "f" then
            local verts = {}
            for spec in line:gmatch("%s+(%S+)") do
                local vi, ti, ni
                vi, ti, ni = spec:match("^(%d+)/(%d+)/(%d+)$")
                if not vi then vi, ni = spec:match("^(%d+)//(%d+)$") end
                if not vi then vi, ti = spec:match("^(%d+)/(%d+)$")  end
                if not vi then vi     = spec:match("^(%d+)$")         end
                table.insert(verts, { tonumber(vi), tonumber(ti), tonumber(ni) })
            end
            -- fan-triangulate
            for i = 2, #verts - 1 do
                table.insert(cur.faces, { verts[1], verts[i], verts[i+1] })
            end
        end
    end
    fh:close()

    return pos_list, uv_list, nrm_list, groups, has_normals
end

-- ── build vertex / index buffers ─────────────────────────────────────────────

local function build(pos_list, uv_list, nrm_list, groups, has_normals)
    local verts      = {}   -- { pos, uv, nrm, tan, bitan }
    local key_map    = {}   -- "vi/ti/ni" → 0-based index
    local vert_group = {}   -- 0-based vertex index → first group name

    local function get_vert(vi, ti, ni, grp_name)
        local k = string.format("%d/%d/%d", vi, ti or 0, ni or 0)
        if key_map[k] then return key_map[k] end
        local idx = #verts
        key_map[k] = idx
        verts[idx+1] = {
            pos   = pos_list[vi] or { 0, 0, 0 },
            uv    = (ti and uv_list[ti]) or { 0, 0 },
            nrm   = (ni and nrm_list[ni]) or { 0, 0, 0 },
            tan   = { 0, 0, 0 },
            bitan = { 0, 0, 0 },
        }
        vert_group[idx] = grp_name
        return idx
    end

    local group_index_lists = {}  -- { name=str, list={i0,i1,...} }

    for _, grp in ipairs(groups) do
        if #grp.faces == 0 then goto skip end

        local g_list = {}
        table.insert(group_index_lists, { name = grp.name, list = g_list })

        for _, face in ipairs(grp.faces) do
            local f0, f1, f2 = face[1], face[2], face[3]
            local i0 = get_vert(f0[1], f0[2], f0[3], grp.name)
            local i1 = get_vert(f1[1], f1[2], f1[3], grp.name)
            local i2 = get_vert(f2[1], f2[2], f2[3], grp.name)
            table.insert(g_list, i0)
            table.insert(g_list, i1)
            table.insert(g_list, i2)

            local p0 = pos_list[f0[1]] or { 0,0,0 }
            local p1 = pos_list[f1[1]] or { 0,0,0 }
            local p2 = pos_list[f2[1]] or { 0,0,0 }
            local u0 = (f0[2] and uv_list[f0[2]]) or { 0,0 }
            local u1 = (f1[2] and uv_list[f1[2]]) or { 0,0 }
            local u2 = (f2[2] and uv_list[f2[2]]) or { 0,0 }

            local dp1, dp2 = sub3(p1, p0), sub3(p2, p0)

            -- face normal (accumulated when OBJ has no normals)
            local face_nrm = norm3(cross3(dp1, dp2))

            -- tangent / bitangent (Lengyel's method)
            local du1 = { u1[1]-u0[1], u1[2]-u0[2] }
            local du2 = { u2[1]-u0[1], u2[2]-u0[2] }
            local det = du1[1]*du2[2] - du2[1]*du1[2]
            local tan, bitan
            if math.abs(det) > 1e-10 then
                local r = 1 / det
                tan   = scale3(add3(scale3(dp1,  du2[2]), scale3(dp2, -du1[2])), r)
                bitan = scale3(add3(scale3(dp1, -du2[1]), scale3(dp2,  du1[1])), r)
            else
                -- degenerate UVs: derive frame from face normal
                tan = norm3(cross3(face_nrm, { 0, 1, 0 }))
                if dot3(tan, tan) < 0.5 then
                    tan = norm3(cross3(face_nrm, { 1, 0, 0 }))
                end
                bitan = cross3(face_nrm, tan)
            end

            for _, vi in ipairs({ i0, i1, i2 }) do
                local v = verts[vi+1]
                v.tan   = add3(v.tan,   tan)
                v.bitan = add3(v.bitan, bitan)
                if not has_normals then
                    v.nrm = add3(v.nrm, face_nrm)
                end
            end
        end
        ::skip::
    end

    for _, v in ipairs(verts) do
        v.nrm   = norm3(v.nrm)
        v.tan   = norm3(v.tan)
        v.bitan = norm3(v.bitan)
    end

    return verts, group_index_lists, vert_group
end

-- ── emit ──────────────────────────────────────────────────────────────────────

local function emit(verts, group_index_lists, vert_group, var_name)
    local out = io.output()

    out:write(string.format("static const std::vector<Vertex> %s_verts = {\n", var_name))
    local last_grp = nil
    for i, v in ipairs(verts) do
        local grp = vert_group[i-1]
        if grp ~= last_grp then
            out:write(string.format("    // %s\n", grp or ""))
            last_grp = grp
        end
        out:write(string.format(
            "    {%s,\n     pack_uv(%s),\n     pack_normal(%s),\n     pack_normal(%s),\n     pack_normal(%s)},\n",
            v3(v.pos), v2(v.uv), v3(v.nrm), v3(v.tan), v3(v.bitan)
        ))
    end
    out:write("};\n\n")

    out:write(string.format("static const std::vector<uint32_t> %s_indices = {\n", var_name))
    for _, g in ipairs(group_index_lists) do
        out:write(string.format("    // %s\n", g.name))
        -- 6 indices per line (two triangles)
        for i = 1, #g.list, 6 do
            local row = {}
            for j = 0, 5 do
                if g.list[i+j] then table.insert(row, g.list[i+j]) end
            end
            out:write("    " .. table.concat(row, ", ") .. ",\n")
        end
    end
    out:write("};\n")
end

-- ── main ──────────────────────────────────────────────────────────────────────

if not arg or not arg[1] then
    io.stderr:write("Usage: lua obj_to_cpp.lua <input.obj> [var_name] > output.hpp\n")
    os.exit(1)
end

local path     = arg[1]
local var_name = (arg[2] or path:match("([^/\\]+)%.obj$") or "mesh"):gsub("[^%w_]", "_")

local pos, uvs, nrm, groups, has_normals    = parse_obj(path)
local verts, group_index_lists, vert_group  = build(pos, uvs, nrm, groups, has_normals)
emit(verts, group_index_lists, vert_group, var_name)
