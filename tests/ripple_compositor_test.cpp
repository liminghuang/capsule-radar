#include <assert.h>
#include "ripple_compositor.h"

int main() {
    assert(rippleOpacity(0.0f, 220, 72) == 220);
    assert(rippleOpacity(1.0f, 220, 72) == 72);
    assert(rippleOpacity(-1.0f, 220, 72) == 220);
    assert(rippleOpacity(2.0f, 220, 72) == 72);

    const RippleRowSpans middle = rippleRowSpans(100, 100, 40.0f, 4.0f, 100, 0, 200);
    assert(middle.valid);
    assert(middle.leftStart == 58 && middle.leftEnd == 62);
    assert(middle.rightStart == 138 && middle.rightEnd == 142);

    const RippleRowSpans edge = rippleRowSpans(100, 100, 40.0f, 4.0f, 142, 0, 200);
    assert(edge.valid);
    assert(edge.leftStart == 100 && edge.rightEnd == 100);

    const RippleRowSpans outside = rippleRowSpans(100, 100, 40.0f, 4.0f, 143, 0, 200);
    assert(!outside.valid);

    const RippleRowSpans clipped = rippleRowSpans(5, 50, 20.0f, 4.0f, 50, 0, 30);
    assert(clipped.valid);
    assert(clipped.leftStart == 0 && clipped.rightEnd == 27);

    // A panel-edge ripple must still retain its topmost visible span after clipping.
    const RippleRowSpans panelEdge = rippleRowSpans(233, 233, 232.0f, 8.0f, 0, 0, 465);
    assert(panelEdge.valid);
    return 0;
}
