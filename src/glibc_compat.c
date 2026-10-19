// The launcher is built on a rolling-release distribution, and that used to decide
// where it could run: libm's float maths functions there carry the newest symbol
// version, GLIBC_2.43, which no Ubuntu LTS has. The launcher then refused to start on
// a player's machine with "version `GLIBC_2.43' not found" - a Linux launcher that
// will not start on Linux is no launcher at all.
//
// These wrappers are what fixes that. --wrap is a link-time option, so it catches
// every reference to them - raylib's as well as ours - and each one is implemented
// here on top of the double-precision version, which is as old as glibc itself.
//
// fmod and fmodf are here too, for the same reason one tier down (GLIBC_2.38): they
// are only used for wrapping angles and positions, where computing the remainder
// through trunc is more than accurate enough.
#include <math.h>

float __wrap_sqrtf(float x) { return (float)sqrt((double)x); }
float __wrap_acosf(float x) { return (float)acos((double)x); }
float __wrap_asinf(float x) { return (float)asin((double)x); }
float __wrap_atan2f(float y, float x) { return (float)atan2((double)y, (double)x); }
double __wrap_fmod(double x, double y) { return x - trunc(x / y) * y; }
float __wrap_fmodf(float x, float y)
{
    return (float)((double)x - trunc((double)x / (double)y) * (double)y);
}
