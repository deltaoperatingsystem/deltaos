#include <math.h>

// approximate sin using a truncated taylor series.
double sin(double x) {
    x = mod(x, PI);
    double x2 = x * x;

    double t1 = x;
    double t2 = (x * x2) / 6.0;                     // x^3 / 3!
    double t3 = (x * x2 * x2) / 120.0;              // x^5 / 5!
    double t4 = (x * x2 * x2 * x2) / 5040.0;        // x^7 / 7!
    double t5 = (x * x2 * x2 * x2 * x2) / 362880.0; // x^9 / 9!

    return t1 - t2 + t3 - t4 + t5;
}