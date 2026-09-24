class_name AppLog
extends RefCounted
## Plain-text application log in the user data folder, rotated by size (bounded disk use).

const MAX_BYTES := 4 * 1024 * 1024
const KEEP := 5

static var _file: FileAccess
static var _path := ""


static func open(dir: String) -> void:
	DirAccess.make_dir_recursive_absolute(dir)
	_path = dir.path_join("shadow-ant-farm.log")
	_rotate_if_needed(true)


static func info(msg: String) -> void:
	_write("INFO", msg)


static func warn(msg: String) -> void:
	_write("WARN", msg)
	push_warning(msg)


static func error(msg: String) -> void:
	_write("ERROR", msg)
	printerr(msg)


static func path() -> String:
	return _path


static func _write(level: String, msg: String) -> void:
	var line := "%s %s %s" % [Time.get_datetime_string_from_system(false, true), level, msg]
	print(line)
	if _path.is_empty():
		return
	if _file == null:
		_file = FileAccess.open(_path, FileAccess.READ_WRITE if FileAccess.file_exists(_path) else FileAccess.WRITE)
		if _file == null:
			return
		_file.seek_end()
	_file.store_line(line)
	_file.flush()
	if _file.get_length() > MAX_BYTES:
		_rotate_if_needed(false)


static func _rotate_if_needed(at_start: bool) -> void:
	if _file != null:
		_file.close()
		_file = null
	if not FileAccess.file_exists(_path):
		return
	var f := FileAccess.open(_path, FileAccess.READ)
	var size := f.get_length() if f else 0
	if f:
		f.close()
	if size <= MAX_BYTES and at_start:
		return
	for i in range(KEEP - 1, 0, -1):
		var src := "%s.%d" % [_path, i]
		if FileAccess.file_exists(src):
			DirAccess.rename_absolute(src, "%s.%d" % [_path, i + 1])
	DirAccess.rename_absolute(_path, _path + ".1")
	var oldest := "%s.%d" % [_path, KEEP]
	if FileAccess.file_exists(oldest):
		DirAccess.remove_absolute(oldest)
