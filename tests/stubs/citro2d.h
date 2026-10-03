#pragma once
#include "3ds.h"
typedef struct { int unused; } C3D_Tex;
typedef struct {
	u16 width, height;
	float left, right, top, bottom;
} Tex3DS_SubTexture;
typedef struct {
	C3D_Tex *tex;
	Tex3DS_SubTexture *subtex;
} C2D_Image;
