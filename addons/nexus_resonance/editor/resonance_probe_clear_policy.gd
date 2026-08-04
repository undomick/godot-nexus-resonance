extends RefCounted
class_name ResonanceProbeClearPolicy

## Pure helpers for "Clear Unreferenced Probe Data". Keeps cancel / partial-scan
## decisions out of the editor UI path so they can be unit-tested.


## Builds the deletion plan after a project reference scan.
## When [param scan_cancelled] is true the scan may be incomplete — never offer deletes.
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
