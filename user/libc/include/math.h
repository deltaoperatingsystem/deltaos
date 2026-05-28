#ifndef __MATH_H
#define __MATH_H

#define PI 3.14159265358979323846
#define max(a, b) ((a) > (b) ? (a) : (b))
#define min(a, b) ((a) < (b) ? (a) : (b))
#define INFINITY ((float)(1.0 / 0.0))

double mod(double a, double b);

double sin(double x); // uses radians
double cos(double x); // uses radians
double tan(double x); // also uses radians

double asin(double x);
double acos(double x);
double atan(double x);

double isqrt(double x);
double sqrt(double x);
double pow(double x, double y);
double exp(double x);
double tanh(double x);

double floor(double x);
double ceil(double x);
double fabs(double x);

float sinf(float x);
float cosf(float x);
float sqrtf(float x);
float fabsf(float x);
float expf(float x);
float tanhf(float x);
float powf(float x, float y);
float roundf(float x);

#endif
