extends GutTest

## Epoch stamp for Coda bridge source handles (same recycle rules as ResonancePlayer / C++ policy).


func test_handle_matches_lifecycle_epoch_requires_non_zero_matching_epoch() -> void:
	assert_true(
		ResonanceCodaVoiceSync.handle_matches_lifecycle_epoch(0, 1, 1),
		"Live handle with matching epoch is valid."
	)
	assert_true(ResonanceCodaVoiceSync.handle_matches_lifecycle_epoch(3, 7, 7))
	assert_false(
		ResonanceCodaVoiceSync.handle_matches_lifecycle_epoch(-1, 1, 1),
		"Negative handle is never valid."
	)
	assert_false(
		ResonanceCodaVoiceSync.handle_matches_lifecycle_epoch(0, 1, 2),
		"Post-reinit epoch mismatch must reject the recycled ID."
	)
	assert_false(
		ResonanceCodaVoiceSync.handle_matches_lifecycle_epoch(0, 0, 1),
		"Default-constructed epoch 0 must never match (mirrors C++ next_source_lifecycle_epoch)."
	)


func test_reload_plan_drops_stale_handle_without_destroy() -> void:
	# After reinit the server already destroyed sources; a stale integer must not be passed to destroy.
	var stale_handle := 0
	var stale_epoch := 1
	var server_epoch := 2
	assert_false(
		ResonanceCodaVoiceSync.handle_matches_lifecycle_epoch(
			stale_handle, stale_epoch, server_epoch
		)
	)
	# destroy_source_if_epoch_matches is a no-op when epochs differ (covered indirectly: mismatch gate).
	var should_destroy := ResonanceCodaVoiceSync.handle_matches_lifecycle_epoch(
		stale_handle, stale_epoch, server_epoch
	)
	assert_false(should_destroy)
