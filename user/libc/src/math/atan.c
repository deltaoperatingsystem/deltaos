#include <math.h>

// approximate atan using a truncated taylor series
double atan(double x) {
    if (x > 1.0)
        return PI / 2.0 - atan(1.0 / x);
    if (x < -1.0)
        return -PI / 2.0 - atan(1.0 / x);

    double x2 = x * x;

    double t1 = x;
    double t2 = (x * x2) / 3.0;
    double t3 = (x * x2 * x2) / 5.0;
    double t4 = (x * x2 * x2 * x2) / 7.0;
    double t5 = (x * x2 * x2 * x2 * x2) / 9.0;

    return t1 - t2 + t3 - t4 + t5;
}
