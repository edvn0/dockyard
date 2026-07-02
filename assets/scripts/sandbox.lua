-- sandbox.lua — port of src/sandbox/sandbox.cpp
-- State lives in this table (persists across begin_play/tick/end_play; rebuilt on reload).
local game = {
	helmet_mesh = nil,
	fox_mesh = nil,
	helmets = {},
	player_id = nil,
	time = 0.0,
}

local function rand_range(a, b)
	return a + math.random() * (b - a)
end

local function make_wall(scene, assets, name, pos, scl)
	local e = scene:make(name)
	local mc = e:add_mesh()
	local handle, err = assets:load_mesh(VFSPath.create("engine://cube"))
	if handle then
		mc.handle = handle
		mc:set_source_path("engine://cube")
	else
		print("[sandbox] cube mesh error: " .. (err or "?"))
	end
	local tf = e:get_transform()
	tf.position = pos
	tf.scale = scl
	local mo = e:add_material_override()
	mo:set_albedo(rand_range(0, 1), rand_range(0, 1), rand_range(0, 1), 1)
	mo:set_roughness(rand_range(0, 1))
	mo:set_metallic(rand_range(0, 1))
	-- Static box collider matching "engine://cube"'s unit-cube extents
	-- (-0.5..0.5); the entity's Transform.scale (tf.scale above) is applied
	-- to the collision shape at Play time, same as the visual mesh.
	local col = e:add_collider()
	col.half_extents = vec3(0.5, 0.5, 0.5)
	e:add_rigid_body() -- mass defaults to 0 (static)
	return e
end

local function make_human(scene, assets, name, x, z)
	local human_scale_y = 1.75 / 7.826
	local human_foot_y = 3.913 * human_scale_y
	local e = scene:make(name)
	local mc = e:add_mesh()
	local handle, err = assets:load_mesh(VFSPath.create("engine://capsule"))
	if handle then
		mc.handle = handle
		mc:set_source_path("engine://capsule")
	else
		print("[sandbox] capsule mesh error: " .. (err or "?"))
	end
	local tf = e:get_transform()
	tf.position = vec3(x, human_foot_y, z)
	tf.scale = vec3(0.2, human_scale_y, 0.2)
	local mo = e:add_material_override()
	mo:set_albedo(rand_range(0, 1), rand_range(0, 1), rand_range(0, 1), 1)
	mo:set_roughness(rand_range(0, 1))
	mo:set_metallic(rand_range(0, 1))
	return e
end

local function make_light(scene, light_parent, x, z, intensity, radius)
	local e = scene:make("Light", light_parent)
	local pl = e:add_point_light()
	pl.color = vec3(rand_range(0.8, 1), rand_range(0.8, 1), rand_range(0.8, 1))
	pl.intensity = intensity
	pl.radius = radius
	local tf = e:get_transform()
	tf.position = vec3(x, 3.5, z)
	return e
end

local function load_scene(scene, assets)
	local wh = 4.0 -- wall height
	local wt = 0.5 -- wall thickness
	local wy = wh * 0.5

	-- Central room south wall (door gap at center)
	make_wall(scene, assets, "Wall_Central_S_L", vec3(-4.75, wy, -8.0), vec3(6.5, wh, wt))
	make_wall(scene, assets, "Wall_Central_S_R", vec3(4.75, wy, -8.0), vec3(6.5, wh, wt))
	-- Central room north wall (door gap for north corridor)
	make_wall(scene, assets, "Wall_Central_N_L", vec3(-5.0, wy, 8.0), vec3(6.0, wh, wt))
	make_wall(scene, assets, "Wall_Central_N_R", vec3(5.0, wy, 8.0), vec3(6.0, wh, wt))
	-- Central room west/east walls
	make_wall(scene, assets, "Wall_Central_W", vec3(-8.0, wy, 0.0), vec3(wt, wh, 16.0))
	make_wall(scene, assets, "Wall_Central_E_N", vec3(8.0, wy, 6.5), vec3(wt, wh, 3.0))
	make_wall(scene, assets, "Wall_Central_E_S", vec3(8.0, wy, -6.5), vec3(wt, wh, 3.0))
	-- Central room columns
	make_wall(scene, assets, "Column_NE", vec3(4.0, wy, 4.0), vec3(0.8, wh, 0.8))
	make_wall(scene, assets, "Column_NW", vec3(-4.0, wy, 4.0), vec3(0.8, wh, 0.8))
	make_wall(scene, assets, "Column_SE", vec3(4.0, wy, -4.0), vec3(0.8, wh, 0.8))
	make_wall(scene, assets, "Column_SW", vec3(-4.0, wy, -4.0), vec3(0.8, wh, 0.8))
	-- Floor
	make_wall(scene, assets, "Floor", vec3(0.0, -0.5, 0.0), vec3(60.0, 1.0, 60.0))
	-- North corridor
	make_wall(scene, assets, "Wall_CorridorN_W", vec3(-2.0, wy, 14.0), vec3(wt, wh, 12.0))
	make_wall(scene, assets, "Wall_CorridorN_E", vec3(2.0, wy, 14.0), vec3(wt, wh, 12.0))
	make_wall(scene, assets, "Wall_CorridorN_End", vec3(0.0, wy, 20.0), vec3(4.0, wh, wt))
	-- South room
	make_wall(scene, assets, "Wall_South_ExtW", vec3(-9.0, wy, -8.0), vec3(2.0, wh, wt))
	make_wall(scene, assets, "Wall_South_ExtE", vec3(9.0, wy, -8.0), vec3(2.0, wh, wt))
	make_wall(scene, assets, "Wall_South_W", vec3(-10.0, wy, -14.0), vec3(wt, wh, 12.0))
	make_wall(scene, assets, "Wall_South_E", vec3(10.0, wy, -14.0), vec3(wt, wh, 12.0))
	make_wall(scene, assets, "Wall_South_End", vec3(0.0, wy, -20.0), vec3(20.0, wh, wt))
	make_wall(scene, assets, "Wall_South_Div_L", vec3(-5.5, wy, -14.0), vec3(9.0, wh, wt))
	make_wall(scene, assets, "Wall_South_Div_R", vec3(5.5, wy, -14.0), vec3(9.0, wh, wt))
	-- East wing
	make_wall(scene, assets, "Wall_East_N", vec3(14.0, wy, 5.0), vec3(12.0, wh, wt))
	make_wall(scene, assets, "Wall_East_S", vec3(14.0, wy, -5.0), vec3(12.0, wh, wt))
	make_wall(scene, assets, "Wall_East_End", vec3(20.0, wy, 0.0), vec3(wt, wh, 10.0))
	make_wall(scene, assets, "Column_East", vec3(14.0, wy, 0.0), vec3(0.8, wh, 0.8))

	-- Humans
	make_human(scene, assets, "Human_01", -6.0, 1.0)
	make_human(scene, assets, "Human_02", 6.0, -1.0)
	make_human(scene, assets, "Human_03", 0.0, -6.0)
	make_human(scene, assets, "Human_04", 3.5, 3.5)
	make_human(scene, assets, "Human_05", -3.5, -3.5)
	make_human(scene, assets, "Human_06", 0.5, 0.0)
	make_human(scene, assets, "Human_07", 0.0, 10.0)
	make_human(scene, assets, "Human_08", -0.5, 14.5)
	make_human(scene, assets, "Human_09", 0.5, 18.5)
	make_human(scene, assets, "Human_10", -6.0, -10.0)
	make_human(scene, assets, "Human_11", 6.0, -11.0)
	make_human(scene, assets, "Human_12", -1.5, -12.5)
	make_human(scene, assets, "Human_13", 3.0, -17.0)
	make_human(scene, assets, "Human_14", 10.5, 2.0)
	make_human(scene, assets, "Human_15", 17.0, -1.0)

	-- Lights
	local lights_parent = scene:make("Lights")
	make_light(scene, lights_parent, 0.0, 0.0, 3.0, 14.0)
	make_light(scene, lights_parent, 5.0, 5.0, 2.0, 8.0)
	make_light(scene, lights_parent, -5.0, -5.0, 2.0, 8.0)
	make_light(scene, lights_parent, 0.0, 11.0, 2.0, 6.0)
	make_light(scene, lights_parent, 0.0, 18.0, 2.0, 6.0)
	make_light(scene, lights_parent, 0.0, -10.0, 2.5, 12.0)
	make_light(scene, lights_parent, -4.0, -17.0, 2.0, 8.0)
	make_light(scene, lights_parent, 4.0, -17.0, 2.0, 8.0)
	make_light(scene, lights_parent, 12.0, 0.0, 2.5, 8.0)
	make_light(scene, lights_parent, 18.5, 0.0, 2.0, 5.0)

	assets:notify_material_overrides_added()
end

local function make_foxes(scene, assets)
	if not (game.fox_mesh and game.fox_mesh:valid()) then
		return
	end
	for i = 0, 9 do
		local fox = scene:make("Fox_" .. i)
		local mc = fox:add_mesh()
		mc.handle = game.fox_mesh
		mc:set_source_path("meshes://fox/Fox.glb")
		local tf = fox:get_transform()
		tf.scale = vec3(0.01, 0.01, 0.01)
		tf.position = vec3(rand_range(-25.0, 25.0), 0.0, rand_range(-25.0, 25.0))
		local anim, err = assets:make_animation_state(game.fox_mesh, 0, 0)
		if anim then
			fox:add_animation_state(anim)
		else
			print("[sandbox] fox animation error: " .. (err or "?"))
		end
	end
end

-- ---------------------------------------------------------------------------

function game.on_scene_load(scene, assets)
    local handle, err = assets:load_mesh(
        VFSPath.create("meshes://damaged_helmet/DamagedHelmet.glb"))
    if handle then
        game.helmet_mesh = handle
        print("[Sandbox] Helmet mesh ready (index " .. handle:index() .. ")")
    else
        print("[Sandbox] Failed to preload helmet mesh: " .. (err or "?"))
    end


    --  Spawn 300 helmets if the mesh was preloaded
    if game.helmet_mesh and game.helmet_mesh:valid() then
        local size   = 100.0
        local parent = scene:make("Helmet parent")
        for _ = 1, 500 do
            local child = scene:make("Helmet", parent)
            local mc    = child:add_mesh()
            mc.handle   = game.helmet_mesh
            mc:set_source_path("meshes://damaged_helmet/DamagedHelmet.glb")
            local tf    = child:get_transform()
            tf.position = vec3(rand_range(-size, size),
                rand_range(-size, size),
                rand_range(-size, size))
        end
    end
	--[[

    local fox_handle, fox_err = assets:load_mesh(VFSPath.create("meshes://fox/Fox.glb"))
    if fox_handle then
        game.fox_mesh = fox_handle
        print("[Sandbox] Fox mesh ready (index " .. fox_handle:index() .. ")")
    else
        print("[Sandbox] Failed to preload fox mesh: " .. (fox_err or "?"))
    end

    make_foxes(scene, assets)

    local player   = scene:make("Player 4")
    game.player_id = player:id()


	-- Walls/columns/humans test level disabled — default editor scene is a
	-- single DamagedHelmet (see dockforge_init.cpp) for isolated PBR review.
	-- load_scene(scene, assets)

	--[[ local bistro_path = "meshes://Bistro/BistroExterior.glb.zst"
	local bistro_handle, err = assets:load_mesh(VFSPath.create(bistro_path), true)
	if bistro_handle then
		local e = scene:make("Bistro")
		local mc = e:add_mesh()
		mc.handle = bistro_handle
		mc:set_source_path(bistro_path)

		local tf = e:get_transform()
		-- BistroExterior is already authored at real-world (meter) scale —
		-- its node origins span roughly 85x30x110 units. Unlike the Fox model
		-- below (authored in cm, needs 0.01), scaling this down would shrink
		-- an ~85m courtyard to under a meter, leaving the player's spawn
		-- point nowhere near the (now tiny) collision geometry.
		tf.scale = vec3(0.01, 0.01, 0.01)
		tf.rotation = quat_from_euler(0, 0, 0)

		-- Static mesh collider built from the same asset's LOD0 geometry, so
		-- the player collides with the actual courtyard shape rather than
		-- falling through it. The entity's Transform (position/rotation/scale
		-- above) is applied to the collision shape at Play time, same as the
		-- visual mesh.
		local col = e:add_collider()
		col.shape = ColliderShape.Mesh
		col:set_mesh_source_path(bistro_path)
		e:add_rigid_body() -- mass defaults to 0 (static)
	else
		print("[sandbox] bistro load error: " .. (err or "?"))
	end ]]
end

function game.begin_play(scene)
	game.time = 0.0

	-- Collect all entities with valid mesh handles into the orbit list
	game.helmets = {}
	scene:each_mesh(function(id, transform, mesh)
		if mesh.handle:valid() then
			local pos = transform.position
			game.helmets[#game.helmets + 1] = {
				id = id,
				ox = pos.x,
				oy = pos.y,
				oz = pos.z,
				phase = 0.0,
			}
		end
	end)

	-- Distribute phases evenly across all orbiting entities
	local count = #game.helmets
	local tau = 2 * math.pi
	for i, hd in ipairs(game.helmets) do
		hd.phase = (i - 1) / count * tau
	end
end

function game.tick(scene, dt)
	game.time = game.time + dt
end

function game.end_play(scene)
	if game.player_id and scene:is_valid(game.player_id) then
		scene:destroy_entity(game.player_id)
	end
end

function game.on_scene_unload(scene)
	game.helmet_mesh = nil
	game.fox_mesh = nil
	game.helmets = {}
	game.player_id = nil
	game.time = 0.0
end

return game
