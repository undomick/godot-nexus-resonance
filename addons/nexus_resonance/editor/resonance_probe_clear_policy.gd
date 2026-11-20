extends RefCounted

## Decides what "Clear Unreferenced Probe Data" may delete.


## A canceled scan may be incomplete, so it deletes nothing.
static func build_clear_plan(
	probe_files: PackedStringArray, referenced: PackedStringArray, scan_cancelled: bool
) -> Dictionary:
	if scan_cancelled:
		return {"aborted": true, "to_delete": PackedStringArray()}
	var to_delete: PackedStringArray = []
	for path in probe_files:
		if path not in referenced:
			to_delete.append(path)
	return {"aborted": false, "to_delete": to_delete}
