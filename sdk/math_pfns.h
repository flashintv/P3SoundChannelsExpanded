//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: 
//
//=====================================================================================//

#ifndef _MATH_PFNS_H_
#define _MATH_PFNS_H_

#pragma once

#include <math.h>

class Vector;
class QAngle;

// misyl: This is faster than doing fsincos these days.
inline void SinCos( float radians, float* __restrict sine, float* __restrict cosine )
{
	*sine = sinf( radians );
	*cosine = cosf( radians );
}

float Approach( float target, float value, float speed );
void AngleVectors( const QAngle& angles, Vector* forward );
void AngleVectors( const QAngle& angles, Vector* forward, Vector* right, Vector* up );

#define FastRSqrt( x ) ( 1.0f / ::sqrtf( x ) )

#define FastCos ::cosf
#define FastSqrt ::sqrtf
#define FastSinCos ::SinCos
#define FastRSqrtFast FastRSqrt

#endif // _MATH_PFNS_H_