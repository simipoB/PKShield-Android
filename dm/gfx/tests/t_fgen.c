// Frame generation's decisions (nvn_fgen.h, the pure part): when a frame gets an in-between picture, when the real frame goes out, when the GPU is too slow
#include <stdio.h>
#include <stdint.h>
#define FG_PLAN_ONLY
#include "../nvn/nvn_fgen.h"
static int fails;
#define CHECK(c) do { if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define MS 1000000ull
int main(void) {
    const uint64_t P = 33333333ull, R120 = 8333333ull, R60 = 16666667ull;
    { fg_plan s = { 0 };   // steady 30 fps: every frame but the first
      CHECK(!fg_plan_frame(&s, 1000 * MS, P)); CHECK(fg_plan_frame(&s, 1000 * MS + P, P)); CHECK(fg_plan_frame(&s, 1000 * MS + 2 * P + 3 * MS, P));
      CHECK(!fg_plan_frame(&s, 1000 * MS + 2 * P + 3 * MS + 80 * MS, P));   // after a hitch of 80 ms: no in-between of two far-apart frames
      CHECK(fg_plan_frame(&s, 1000 * MS + 3 * P + 83 * MS, P));             // and again once frames come evenly
      CHECK(!fg_plan_frame(&s, 1000 * MS + 3 * P + 83 * MS + 5 * MS, P)); } // two frames 5 ms apart (the game caught up): nothing between
    { // the real frame: half a period after the in-between; with asked display times on a 120 Hz screen a part of a refresh before the in-between's time
      CHECK(fg_real_due(100 * MS, 0, P / 2, 0, 50) == 100 * MS + P / 2);
      CHECK(fg_real_due(100 * MS, 0, P / 2, R120, 50) == 100 * MS + P / 2);
      CHECK(fg_real_due(100 * MS, 116 * MS, P / 2, R120, 50) == 116 * MS - R120 / 2);
      CHECK(fg_real_due(100 * MS, 116 * MS, P / 2, R120, -100) == 116 * MS + R120);   // (a negative lead: after the in-between's time)
      CHECK(fg_real_due(100 * MS, 116 * MS, P / 2, R60, 50) == 100 * MS + P / 2);     // 60 Hz: half a period is one refresh, times are not asked
      CHECK(fg_real_due(200 * MS, 116 * MS, P / 2, R120, 50) == 200 * MS); }          // never in the past
    { fg_plan s = { 0 }; uint64_t t = 5000 * MS; int off = 0;   // 3 ms a picture against an 8 ms budget: never off
      for (int i = 0; i < 600; i++, t += P) off |= fg_plan_cost(&s, 3 * MS, t, 8 * MS);
      CHECK(!off); }
    { fg_plan s = { 0 }; uint64_t t = 5000 * MS; int off = 0, at = -1;   // 12 ms: off after two windows of 2 s, not at the first slow picture
      for (int i = 0; i < 600 && !off; i++, t += P) { off = fg_plan_cost(&s, 12 * MS, t, 8 * MS); if (off) at = i; }
      CHECK(off && at >= 119 && at <= 125); }
    { fg_plan s = { 0 }; uint64_t t = 5000 * MS; int off = 0;   // one slow window between fast ones (a pipeline built in play): stays on
      for (int i = 0; i < 600; i++, t += P) off |= fg_plan_cost(&s, i >= 61 && i < 121 ? 12 * MS : 3 * MS, t, 8 * MS);
      CHECK(!off); }
    printf(fails ? "t_fgen: %d FAILED\n" : "t_fgen: all passed\n", fails);
    return fails != 0;
}
