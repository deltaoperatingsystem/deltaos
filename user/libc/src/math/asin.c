#include <math.h>

double asin(double x) {
    if (x >= 1.0) return PI / 2.0;
    if (x <= -1.0) return -PI / 2.0;

    return atan(x / sqrt(1.0 - x*x));
}
