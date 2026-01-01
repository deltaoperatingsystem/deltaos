// https://en.wikipedia.org/wiki/Fast_inverse_square_root
double isqrt(double x) {    
    double x2 = x * 0.5;
    double y = x;
    long long i = *(long long*)&y;
    i = 0x5fe6eb50c7b537a9 - (i >> 1);
    y = *(double *)&i;
    y = y * (1.5 - (x2 * y * y));
    y = y * (1.5 - (x2 * y * y)); // needed because of double precision

    return y;
}