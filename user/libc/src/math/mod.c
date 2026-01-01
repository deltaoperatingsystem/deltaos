double mod(double a, double b) {
    double res;
    if (a < 0) res = -a;
    else res = a;
    if (b < 0) b = -b;

    while (res >= b) res -= b;

    if (a < 0) return -res;
    return res;
}