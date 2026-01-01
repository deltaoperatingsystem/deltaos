#include <math.h>

double acos(double x) {
    if (x >= 1.0) return 0.0;
    if (x <= -1.0) return PI;

    return PI / 2.0 - asin(x);
}
