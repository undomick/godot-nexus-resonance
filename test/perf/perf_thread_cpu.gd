extends RefCounted

## Per-thread CPU time of this process from /proc (Linux only). Empty results elsewhere.

## The direct worker and the reflections worker share one CPU budget.
const WORKER_THREAD_NAMES: Array[String] = ["NexusSimWorker", "NexusReflWorker"]

var _ticks_per_sec: float = 100.0


func is_supported() -> bool:
	return OS.get_name() == "Linux" and DirAccess.dir_exists_absolute("/proc/self/task")


## tid -> {"name": String, "ticks": int}
func snapshot() -> Dictionary:
	var out := {}
	if not is_supported():
		return out
	for tid in DirAccess.get_directories_at("/proc/self/task"):
		var line := _read_first_line("/proc/self/task/%s/stat" % tid)
		var parsed := _parse_stat_line(line)
		if not parsed.is_empty():
			out[int(tid)] = parsed
	return out


## CPU usage between two snapshots in percent of one core, grouped into main / worker / other.
func usage_between(before: Dictionary, after: Dictionary, wall_sec: float) -> Dictionary:
	var main_pid := OS.get_process_id()
	var main_ticks := 0
	var worker_ticks := 0
	var other_ticks := 0
	for tid in after:
		var delta: int = after[tid]["ticks"] - (before[tid]["ticks"] if before.has(tid) else 0)
		if tid == main_pid:
			main_ticks += delta
		elif after[tid]["name"] in WORKER_THREAD_NAMES:
			worker_ticks += delta
		else:
			other_ticks += delta
	var to_pct := 100.0 / (_ticks_per_sec * maxf(wall_sec, 0.001))
	return {
		"main_pct": main_ticks * to_pct,
		"worker_pct": worker_ticks * to_pct,
		"other_pct": other_ticks * to_pct,
		"total_pct": (main_ticks + worker_ticks + other_ticks) * to_pct,
		"worker_found": _has_worker(after),
	}


func _has_worker(snap: Dictionary) -> bool:
	for tid in snap:
		if snap[tid]["name"] in WORKER_THREAD_NAMES:
			return true
	return false


## Involuntary context switches of one thread since it started, or -1 when unknown.
func involuntary_switches(tid: int) -> int:
	if tid < 0 or not is_supported():
		return -1
	var f := FileAccess.open("/proc/self/task/%d/status" % tid, FileAccess.READ)
	if f == null:
		return -1
	while not f.eof_reached():
		var line := f.get_line()
		if line.begins_with("nonvoluntary_ctxt_switches:"):
			return int(line.get_slice(":", 1).strip_edges())
	return -1


## System-wide CPU ticks from the first line of /proc/stat: {"steal": int, "total": int}, empty when unknown.
func host_snapshot() -> Dictionary:
	if not is_supported():
		return {}
	var fields := _read_first_line("/proc/stat").split(" ", false)
	if fields.size() < 9 or fields[0] != "cpu":
		return {}
	# guest and guest_nice after steal are already counted in user and nice.
	var total := 0
	for i in range(1, 9):
		total += int(fields[i])
	return {"steal": int(fields[8]), "total": total}


## Share of all CPU time the hypervisor gave to other guests between two snapshots, in percent; -1 when unknown.
func steal_pct_between(before: Dictionary, after: Dictionary) -> float:
	if before.is_empty() or after.is_empty():
		return -1.0
	var total: int = after["total"] - before["total"]
	if total <= 0:
		return -1.0
	return 100.0 * float(after["steal"] - before["steal"]) / float(total)


func _read_first_line(path: String) -> String:
	var f := FileAccess.open(path, FileAccess.READ)
	if f == null:
		return ""
	return f.get_line()


# The thread name sits in parentheses and may contain spaces, so split after the last ')'.
# utime and stime are fields 14 and 15 of the stat line.
func _parse_stat_line(line: String) -> Dictionary:
	var open := line.find("(")
	var close := line.rfind(")")
	if open < 0 or close < open:
		return {}
	var rest := line.substr(close + 2).split(" ")
	if rest.size() < 13:
		return {}
	return {"name": line.substr(open + 1, close - open - 1), "ticks": int(rest[11]) + int(rest[12])}
