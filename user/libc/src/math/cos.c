#include <math.h>

// approximate cos using a truncated taylor series.
double cos(double x) {
    x = mod(x, PI);
    double x2 = x * x;

    double t1 = 1;
    double t2 = (x2) / 2.0;                     // x^2 / 2!
    double t3 = (x2 * x2) / 24.0;               // x^4 / 4!
    double t4 = (x2 * x2 * x2) / 720.0;         // x^6 / 6!
    double t5 = (x2 * x2 * x2 * x2) / 40320.0;  // x^8 / 8!

    return t1 - t2 + t3 - t4 + t5;
}