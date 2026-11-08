extends SceneTree
# Uses the real Godot 3 physics server, not Brotato's recovered autoloads.
# Run: Godot --no-window --fixed-fps 60 --script tests/brotato_physics_fixture.gd
# Each JSON line is a measured engine frame. No game assets are required.

class DrivenBody:
	extends RigidBody2D
	var commanded_velocity = Vector2.ZERO
	func _integrate_forces(state):
		state.linear_velocity = commanded_velocity

class KinematicDriver:
	extends RigidBody2D
	var commanded_velocity = Vector2.ZERO
	func _physics_process(delta):
		var result = Physics2DTestMotionResult.new()
		var collided = test_motion(commanded_velocity * delta, true, 0.08, result)
		global_position += result.motion
		if collided:
			var slide = commanded_velocity.slide(result.collision_normal)
			test_motion(slide * delta, true, 0.08, result)
			global_position += result.motion
	func _integrate_forces(state):
		state.transform = global_transform

var fixture_root
var entered = 0
var exited = 0
var output
var frame_count = 36

func _initialize():
	output = File.new()
	var path = OS.get_environment("BROTATO_PHYSICS_TRACE")
	if path != "" and output.open(path, File.WRITE) != OK:
		printerr("Cannot open physics trace: ", path)
		quit(2)
		return
	call_deferred("_run")

func emit(row):
	var text = JSON.print(row)
	print(text)
	if output.is_open():
		output.store_line(text)

func circle(radius, offset):
	var shape = CircleShape2D.new()
	shape.radius = radius
	var collision = CollisionShape2D.new()
	collision.shape = shape
	collision.position = offset
	return collision

func body(at, velocity, radius, mass_value = 1.0, kinematic = false, layer = 4, mask = 132):
	var node
	if kinematic:
		node = KinematicDriver.new()
		node.mode = RigidBody2D.MODE_KINEMATIC
	else:
		node = DrivenBody.new()
		node.mode = RigidBody2D.MODE_CHARACTER
	node.position = at
	node.commanded_velocity = velocity
	node.mass = mass_value
	node.custom_integrator = true
	node.gravity_scale = 0.0
	node.linear_damp = 0.0
	node.angular_damp = 0.0
	node.can_sleep = false
	node.collision_layer = layer
	node.collision_mask = mask
	var material = PhysicsMaterial.new()
	material.friction = 0.0
	material.bounce = 0.0
	node.physics_material_override = material
	node.add_child(circle(radius, Vector2.ZERO))
	fixture_root.add_child(node)
	return node

func wall():
	var node = StaticBody2D.new()
	node.collision_layer = 128
	node.collision_mask = 0
	node.position = Vector2(-128.0, 512.0)
	var shape = RectangleShape2D.new()
	shape.extents = Vector2(128.0, 768.0)
	var collision = CollisionShape2D.new()
	collision.shape = shape
	node.add_child(collision)
	fixture_root.add_child(node)

func area(at, radius, layer, mask, monitoring):
	var node = Area2D.new()
	node.position = at
	node.collision_layer = layer
	node.collision_mask = mask
	node.monitoring = monitoring
	node.monitorable = not monitoring
	node.add_child(circle(radius, Vector2.ZERO))
	fixture_root.add_child(node)
	return node

func on_entered(_other):
	entered += 1

func on_exited(_other):
	exited += 1

func _run():
	emit({"metadata": {"engine": Engine.get_version_info(), "dt": 1.0 / 60.0,
		"gravity": 0, "friction": 0, "bounce": 0, "frames_per_case": frame_count}})
	for name in ["free", "overlap", "unequal_mass", "head_on", "coincident", "wall",
		"kinematic_wall", "filtered_player_enemy", "fast_discrete", "area_overlap"]:
		fixture_root = Node2D.new()
		get_root().add_child(fixture_root)
		var bodies = []
		var areas = []
		entered = 0
		exited = 0
		if name == "free":
			bodies.append(body(Vector2(300, 300), Vector2(120, 60), 20))
		elif name == "overlap" or name == "unequal_mass":
			bodies.append(body(Vector2(300, 300), Vector2.ZERO, 20))
			bodies.append(body(Vector2(330, 300), Vector2.ZERO, 20, 100.0 if name == "unequal_mass" else 1.0))
		elif name == "coincident":
			bodies.append(body(Vector2(300, 300), Vector2.ZERO, 20))
			bodies.append(body(Vector2(300, 300), Vector2.ZERO, 20))
		elif name == "head_on":
			bodies.append(body(Vector2(300, 300), Vector2(120, 0), 20))
			bodies.append(body(Vector2(380, 300), Vector2(-120, 0), 20))
		elif name == "wall" or name == "kinematic_wall":
			wall()
			bodies.append(body(Vector2(30, 300), Vector2(-120, 60), 24, 1.0,
				name == "kinematic_wall", 2 if name == "kinematic_wall" else 4,
				130 if name == "kinematic_wall" else 132))
		elif name == "filtered_player_enemy":
			bodies.append(body(Vector2(300, 300), Vector2(120, 0), 24, 1.0, true, 2, 130))
			bodies.append(body(Vector2(340, 300), Vector2(-120, 0), 22))
		elif name == "fast_discrete":
			bodies.append(body(Vector2(300, 300), Vector2(36000, 0), 20))
			bodies.append(body(Vector2(600, 300), Vector2.ZERO, 20))
		else:
			var hurt = area(Vector2(300, 300), 21, 0, 16, true)
			var hit = area(Vector2(400, 300), 17.0294, 16, 0, false)
			hurt.connect("area_entered", self, "on_entered")
			hurt.connect("area_exited", self, "on_exited")
			areas = [hurt, hit]
		for frame in range(frame_count):
			yield(self, "physics_frame")
			if not areas.empty():
				areas[1].position.x = 400.0 - float(frame) * 6.0
			var states = []
			for node in bodies:
				var p = node.global_position
				var v = node.linear_velocity
				if is_nan(p.x) or is_nan(p.y) or is_inf(p.x) or is_inf(p.y):
					printerr("Non-finite engine position in ", name)
					quit(3)
					return
				states.append({"x": p.x, "y": p.y, "vx": v.x, "vy": v.y})
			emit({"case": name, "frame": frame, "bodies": states, "entered": entered, "exited": exited})
		fixture_root.queue_free()
		yield(self, "idle_frame")
		yield(self, "physics_frame")
	if output.is_open():
		output.close()
	quit(0)
