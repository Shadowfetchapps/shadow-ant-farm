class_name CheckpointStore
extends RefCounted
## Versioned colony checkpoints in the user data folder. Writes are atomic (done in the extension:
## temporary file, fsync, rename); only the newest few are kept, plus one from each of the last hours.

const EXT := ".antfarm"
const KEEP_RECENT := 4
const KEEP_HOURLY := 8
const KEEP_OTHER_COLONIES := 2

var dir := ""


func _init(p_dir: String) -> void:
	dir = p_dir
	DirAccess.make_dir_recursive_absolute(dir)


func save(sim: AntFarmSim) -> String:
	var name := "colony-%016x-t%012d%s" % [sim.get_seed(), sim.get_tick(), EXT]
	var path := dir.path_join(name)
	if not sim.save_checkpoint(path):
		AppLog.warn("checkpoint could not be written: %s" % path)
		return ""
	_prune(sim.get_seed())
	return path


## Newest first.
func list_all() -> Array[String]:
	var out: Array[String] = []
	var d := DirAccess.open(dir)
	if d == null:
		return out
	for f in d.get_files():
		if f.ends_with(EXT):
			out.append(dir.path_join(f))
	out.sort_custom(func(a, b): return FileAccess.get_modified_time(a) > FileAccess.get_modified_time(b) or (FileAccess.get_modified_time(a) == FileAccess.get_modified_time(b) and a > b))
	return out


## Loads the newest checkpoint that passes its integrity checks.
func resume_latest(sim: AntFarmSim) -> String:
	for p in list_all():
		if sim.load_checkpoint(p):
			return p
		AppLog.warn("skipping unreadable checkpoint %s: %s" % [p, sim.get_last_error()])
	return ""


func _prune(seed: int) -> void:
	var prefix := "colony-%016x-" % seed
	var mine: Array[String] = []
	for p in list_all():
		if p.get_file().begins_with(prefix):
			mine.append(p)
	# keep the newest few, then at most one per sim hour for the last KEEP_HOURLY hours
	var hours_kept := {}
	for i in mine.size():
		var p := mine[i]
		if i < KEEP_RECENT:
			continue
		var tick := int(p.get_file().get_slice("-t", 1).trim_suffix(EXT))
		var hour := tick / (30 * 3600)
		if not hours_kept.has(hour) and hours_kept.size() < KEEP_HOURLY:
			hours_kept[hour] = true
			continue
		DirAccess.remove_absolute(p)
	# other colonies: keep only their newest checkpoint (a finished colony can still be resumed)
	var seen := {}
	for p in list_all():
		var f := p.get_file()
		if f.begins_with(prefix):
			continue
		var s := f.get_slice("-t", 0)
		if seen.has(s) or seen.size() >= KEEP_OTHER_COLONIES:
			DirAccess.remove_absolute(p)
		else:
			seen[s] = true
