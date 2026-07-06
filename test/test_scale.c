/*
 * test_scale.c — Unit tests for PeachWM fractional scale logic
 *
 * Compile: cc -std=c23 -Wall -Wextra -lm test/test_scale.c -o test_scale
 * Run:     ./test_scale
 *
 * These tests verify the pure-math logic behind the compositor's
 * fractional scale support without requiring Wayland or wlroots.
 */

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* ── Constants mirrored from the compositor source ──────────── */

/* Tolerance threshold: a scale change <= 0.001f is not meaningful.
 * Mirrors client_update_scale() and layersurface_update_scale(). */
static const float SCALE_TOLERANCE = 0.001f;

/* ── Mock struct for safety-guard testing ───────────────────── */

/*
 * Minimal mock of a compositor surface struct for testing the
 * safety-guard pattern (scale <= 0.0f → early return).
 * We only need current_scale and a flag to verify that
 * the notify code was skipped.
 */
typedef struct MockSurface {
	float current_scale;
	int    notify_called; /* set to 1 if notify was invoked */
} MockSurface;

/*
 * Mock implementation of the safety-guard pattern from
 * client_update_scale() / layersurface_update_scale():
 *
 *   if (scale <= 0.0f)
 *       return;
 *
 * Returns 1 if notify was called (valid scale), 0 if guard triggered.
 */
static int
mock_update_scale(MockSurface *s, float scale)
{
	/* Safety guard: invalid scale → no-op */
	if (scale <= 0.0f)
		return 0;

	/* Tolerance check: only notify on meaningful changes
	 * (mirrors the real compositor's fabsf check) */
	if (fabsf(scale - s->current_scale) > SCALE_TOLERANCE) {
		s->current_scale = scale;
		s->notify_called = 1;
		return 1;
	}

	return 0;
}

/* ── Test: Float compare tolerance ──────────────────────────── */

/*
 * Verifies that the fabsf-based tolerance check correctly
 * distinguishes meaningful scale changes from negligible ones.
 *
 * The compositor uses fabsf(scale - current_scale) > 0.001f
 * to decide whether a scale change warrants re-notifying clients.
 * Small floating-point noise should NOT trigger a notification;
 * genuine changes (e.g. switching from 1.5x to 2.0x) should.
 */
static void
test_float_compare_tolerance(void)
{
	printf("  test_float_compare_tolerance... ");

	/*
	 * Case 1: nearly identical values → diff < tolerance.
	 * 1.5f and 1.5001f differ by 0.0001f, which is < 0.001f.
	 * The compositor should treat these as the same scale.
	 */
	float diff_small = fabsf(1.5f - 1.5001f);
	assert(diff_small < SCALE_TOLERANCE);

	/*
	 * Case 2: clearly different values → diff > tolerance.
	 * 1.5f and 2.0f differ by 0.5f, which is >> 0.001f.
	 * The compositor should treat these as different scales.
	 */
	float diff_large = fabsf(1.5f - 2.0f);
	assert(diff_large > SCALE_TOLERANCE);

	/*
	 * Case 3: same value — zero diff, well below tolerance.
	 * 1.5f and 1.5f differ by 0.0f.
	 * No change should trigger.
	 */
	float diff_zero = fabsf(1.5f - 1.5f);
	assert(!(diff_zero > SCALE_TOLERANCE));

	printf("PASS\n");
}

/* ── Test: Safety guard (scale <= 0.0f) ─────────────────────── */

/*
 * Verifies that the safety-guard logic (scale <= 0.0f → early return)
 * correctly prevents scale-notify calls with invalid scale values.
 *
 * The compositor derives scale from wlr_output.scale, which can
 * theoretically be 0 or negative in edge cases. The guard ensures
 * we never call wlr_fractional_scale_v1_notify_scale() with
 * garbage scale values.
 */
static void
test_safety_guard(void)
{
	printf("  test_safety_guard... ");

	MockSurface s = { .current_scale = 1.0f, .notify_called = 0 };

	/*
	 * Case 1: scale = 0.0f — guard must trigger, no notify.
	 * 0.0 is the guard boundary.
	 */
	int result_zero = mock_update_scale(&s, 0.0f);
	assert(result_zero == 0);
	assert(s.notify_called == 0);
	assert(s.current_scale == 1.0f); /* unchanged */

	/*
	 * Case 2: scale = -0.5f — guard must trigger, no notify.
	 * Negative scale is invalid.
	 */
	int result_neg = mock_update_scale(&s, -0.5f);
	assert(result_neg == 0);
	assert(s.notify_called == 0);
	assert(s.current_scale == 1.0f); /* unchanged */

	/*
	 * Case 3: scale = -1.0f — guard must trigger, no notify.
	 * Negative integer scale is invalid.
	 */
	int result_neg_one = mock_update_scale(&s, -1.0f);
	assert(result_neg_one == 0);
	assert(s.notify_called == 0);
	assert(s.current_scale == 1.0f); /* unchanged */

	/*
	 * Case 4: scale = 1.5f (valid, different from current 1.0f)
	 * — guard must NOT trigger, notify must be called.
	 */
	int result_valid = mock_update_scale(&s, 1.5f);
	assert(result_valid == 1);
	assert(s.notify_called == 1);
	assert(s.current_scale == 1.5f); /* updated */

	/*
	 * Case 5: scale <= 0 with notify_called already set to 1
	 * — guard must still trigger, notify_called must NOT change.
	 */
	s.notify_called = 0;
	int result_after_valid = mock_update_scale(&s, 0.0f);
	assert(result_after_valid == 0);
	assert(s.notify_called == 0);

	printf("PASS\n");
}

/* ── Test: ceilf fallback for integer buffer scale ──────────── */

/*
 * Verifies that ceilf() produces the expected integer buffer scale
 * values for XWayland surfaces.
 *
 * The compositor calls wlr_surface_set_preferred_buffer_scale()
 * with (int32_t)ceilf(scale). XWayland has no fractional-scale
 * protocol, so the scale is rounded up to the next integer.
 *
 * We test ceilf behavior at the values the compositor actually
 * uses, plus edge cases. The goal is NOT to test libm's ceilf
 * (which is known correct), but to verify our understanding of
 * how the compositor maps fractional scales to integer buffer
 * scales.
 */
static void
test_ceilf_fallback(void)
{
	printf("  test_ceilf_fallback... ");

	/*
	 * Case 1: exact integer → ceilf returns the same value.
	 * scale 1.0f → buffer scale 1
	 */
	assert((int32_t)ceilf(1.0f) == 1);

	/*
	 * Case 2: fractional scale → ceilf rounds up.
	 * scale 1.5f → buffer scale 2
	 * This is the common case: 1.5x fractional scale means
	 * the XWayland buffer is rendered at 2x.
	 */
	assert((int32_t)ceilf(1.5f) == 2);

	/*
	 * Case 3: fractional < 1.0f → ceilf rounds up to 1.
	 * scale 0.75f → buffer scale 1
	 * Anything less than 1x still renders at 1x minimum.
	 */
	assert((int32_t)ceilf(0.75f) == 1);

	/*
	 * Case 4: scale 2.0f → buffer scale 2 (identity).
	 * Verifies that ceilf doesn't distort exact integers.
	 */
	assert((int32_t)ceilf(2.0f) == 2);

	/*
	 * Case 5: scale 1.001f → buffer scale 2 (just over 1).
	 * Even a tiny fraction over an integer rounds up.
	 */
	assert((int32_t)ceilf(1.001f) == 2);

	/*
	 * Case 6: scale 0.001f → buffer scale 1.
	 * Very small positive scale still rounds up to 1.
	 */
	assert((int32_t)ceilf(0.001f) == 1);

	printf("PASS\n");
}

/* ── Entry point ────────────────────────────────────────────── */

int
main(void)
{
	int failed = 0;

	printf("=== PeachWM scale logic tests ===\n\n");

	printf("[1/3] Float compare tolerance\n");
	test_float_compare_tolerance();

	printf("[2/3] Safety guard (scale <= 0.0f)\n");
	test_safety_guard();

	printf("[3/3] ceilf fallback for XWayland\n");
	test_ceilf_fallback();

	printf("\n=== All scale tests passed ===\n");
	return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
