#define M_PI 3.14159265358979323846

double mod(double a, double b) {
    double res;
    if (a < 0) res = -a;
    else res = a;
    if (b < 0) b = -b;

    while (res >= b) res -= b;

    if (a < 0) return -res;
    return res;
}

// approximate sin using a truncated taylor series.
double sin(double x) {
    x = mod(x, M_PI);
    double x2 = x * x;

    double t1 = x;
    double t2 = (x * x2) / 6.0;                     // x^3 / 3!
    double t3 = (x * x2 * x2) / 120.0;              // x^5 / 5!
    double t4 = (x * x2 * x2 * x2) / 5040.0;        // x^7 / 7!
    double t5 = (x * x2 * x2 * x2 * x2) / 362880.0; // x^9 / 9!

    return t1 - t2 + t3 - t4 + t5;
}

// approximate cos using a truncated taylor series.
double cos(double x) {
    x = mod(x, M_PI);
    double x2 = x * x;

    double t1 = 1;
    double t2 = (x2) / 2.0;                     // x^2 / 2!
    double t3 = (x2 * x2) / 24.0;               // x^4 / 4!
    double t4 = (x2 * x2 * x2) / 720.0;         // x^6 / 6!
    double t5 = (x2 * x2 * x2 * x2) / 40320.0;  // x^8 / 8!

    return t1 - t2 + t3 - t4 + t5;
}