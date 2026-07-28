#include "vector.h"

Vector vec3_origin( 0, 0, 0 );

#ifndef M_PI
#define M_PI		3.14159265358979323846	// matches value in gcc v2 math.h
#endif
#define M_PI_F		((float)(M_PI))	// Shouldn't collide with anything.

#ifndef RAD2DEG
#define RAD2DEG( x  )  ( (float)(x) * (float)(180.f / M_PI_F) )
#endif

#ifndef DEG2RAD
#define DEG2RAD( x  )  ( (float)(x) * (float)(M_PI_F / 180.f) )
#endif

// MOVEMENT INFO
enum
{
	PITCH = 0,	// up / down
	YAW,		// left / right
	ROLL		// fall over
};

float Approach( float target, float value, float speed )
{
	float delta = target - value;

	if ( delta > speed )
		value += speed;
	else if ( delta < -speed )
		value -= speed;
	else
		value = target;

	return value;
}

void AngleVectors( const QAngle& angles, Vector* forward )
{
	Assert( s_bMathlibInitialized );
	Assert( forward );

	float	sp, sy, cp, cy;

	SinCos( DEG2RAD( angles[YAW] ), &sy, &cy );
	SinCos( DEG2RAD( angles[PITCH] ), &sp, &cp );

	forward->x = cp * cy;
	forward->y = cp * sy;
	forward->z = -sp;
}

//-----------------------------------------------------------------------------
// Euler QAngle -> Basis Vectors.  Each vector is optional
//-----------------------------------------------------------------------------
void AngleVectors( const QAngle& angles, Vector* forward, Vector* right, Vector* up )
{
	Assert( s_bMathlibInitialized );

	float sr, sp, sy, cr, cp, cy;

#ifdef _X360
	fltx4 radians, scale, sine, cosine;
	radians = LoadUnaligned3SIMD( angles.Base() );
	scale = ReplicateX4( M_PI_F / 180.f );
	radians = MulSIMD( radians, scale );
	SinCos3SIMD( sine, cosine, radians );
	sp = SubFloat( sine, 0 );	sy = SubFloat( sine, 1 );	sr = SubFloat( sine, 2 );
	cp = SubFloat( cosine, 0 );	cy = SubFloat( cosine, 1 );	cr = SubFloat( cosine, 2 );
#else
	SinCos( DEG2RAD( angles[YAW] ), &sy, &cy );
	SinCos( DEG2RAD( angles[PITCH] ), &sp, &cp );
	SinCos( DEG2RAD( angles[ROLL] ), &sr, &cr );
#endif

	if ( forward )
	{
		forward->x = cp * cy;
		forward->y = cp * sy;
		forward->z = -sp;
	}

	if ( right )
	{
		right->x = (-1 * sr * sp * cy + -1 * cr * -sy);
		right->y = (-1 * sr * sp * sy + -1 * cr * cy);
		right->z = -1 * sr * cp;
	}

	if ( up )
	{
		up->x = (cr * sp * cy + -sr * -sy);
		up->y = (cr * sp * sy + -sr * cy);
		up->z = cr * cp;
	}
}