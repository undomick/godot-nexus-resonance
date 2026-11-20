extends RefCounted


static func percentile(values: Array[float], pct: float) -> float:
	if values.is_empty():
		return 0.0
	var sorted := values.duplicate()
	sorted.sort()
	var idx := clampi(int(ceil(pct / 100.0 * sorted.size())) - 1, 0, sorted.size() - 1)
	return sorted[idx]


static func max_of(values: Array[float]) -> float:
	return values.max() if not values.is_empty() else 0.0


static func min_of(values: Array[float]) -> float:
	return values.min() if not values.is_empty() else 0.0


## Upper bucket edge at which the cumulative count reaches pct, so up to one bucket width high.
static func histogram_percentile(
	counts: PackedInt64Array, upper: PackedInt64Array, pct: float
) -> float:
	var total := 0
	for c in counts:
		total += c
	if total == 0 or upper.size() < counts.size():
		return 0.0
	var rank := ceili(pct / 100.0 * total)
	var seen := 0
	for i in counts.size():
		seen += counts[i]
		if seen >= rank:
			return float(upper[i])
	return float(upper[counts.size() - 1])
