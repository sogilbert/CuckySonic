#include "Camera.h"
#include "Game.h"
#include "Level.h"
#include "MathUtil.h"
#include "Log.h"

//Define things
//#define CAMERA_CD_PAN

//Constants
#define CAMERA_HSCROLL_LEFT		-16
#define CAMERA_HSCROLL_SIZE		 16

#define CAMERA_VSCROLL_OFFSET	-16
#define CAMERA_VSCROLL_UP		 32
#define CAMERA_VSCROLL_DOWN		 32

#define LOOK_PANTIME 120

CAMERA::CAMERA(PLAYER *trackPlayer)
{
	//Clear memory
	memset(this, 0, sizeof(CAMERA));
	
	//Move to our given player
	x = trackPlayer->x.pos - (SCREEN_WIDTH / 2);
	y = trackPlayer->y.pos - (SCREEN_HEIGHT / 2 - 16);
	
	//Keep inside level boundaries
	if (x < gLevel->leftBoundary)
		x = gLevel->leftBoundary;
	if (x + SCREEN_WIDTH > gLevel->rightBoundary)
		x = gLevel->rightBoundary - SCREEN_WIDTH;
	if (y < gLevel->topBoundary)
		y = gLevel->topBoundary;
	if (y + SCREEN_HEIGHT > gLevel->bottomBoundary)
		y = gLevel->bottomBoundary - SCREEN_HEIGHT;
	return;
}

CAMERA::~CAMERA()
{
	return;
}

void CAMERA::Track(PLAYER *trackPlayer)
{
	//Don't move if locked
	if (trackPlayer->cameraLock)
		return;
	
	//Scroll horizontally to the player
	int16_t trackX = trackPlayer->x.pos;
	
	if (trackPlayer->scrollDelay)
	{
		trackPlayer->scrollDelay -= 0x100;
		trackX = trackPlayer->posRecord[(trackPlayer->recordPos - ((trackPlayer->scrollDelay / 0x100) + 1)) % PLAYER_RECORD_LENGTH].x;
	}
	
	int16_t hScrollOffset = trackX - x - xPan;
	
	if ((hScrollOffset -= (SCREEN_WIDTH / 2 + CAMERA_HSCROLL_LEFT)) < 0) //Scroll to the left
	{
		//Cap our scrolling to 16 pixels per frame
		if (hScrollOffset <= -16)
			hScrollOffset = -16;
		
		//Scroll and keep within level boundaries
		x += hScrollOffset;
		if (x < gLevel->leftBoundary)
			x = gLevel->leftBoundary;
	}
	else if ((hScrollOffset -= CAMERA_HSCROLL_SIZE) >= 0) //Scroll to the right
	{
		//Cap our scrolling to 16 pixels per frame
		if (hScrollOffset > 16)
			hScrollOffset = 16;
		
		//Scroll and keep within level boundaries
		x += hScrollOffset;
		if ((x + SCREEN_WIDTH) > gLevel->rightBoundary)
			x = gLevel->rightBoundary - SCREEN_WIDTH;
	}
	
	//Scroll vertically to the player
	int16_t vScrollOffset = trackPlayer->y.pos - y - (SCREEN_HEIGHT / 2 + CAMERA_VSCROLL_OFFSET) - lookPan;
	
	if (trackPlayer->status.inBall)
		vScrollOffset -= 5; //Shift up 5 pixels if in ball-form
	
	//Handle our specific scrolling
	uint16_t scrollSpeed = 16;
	
	if (trackPlayer->status.inAir)
	{
		//Scrolling in mid-air
		if (vScrollOffset < -CAMERA_VSCROLL_UP)
			vScrollOffset += CAMERA_VSCROLL_UP;
		else if (vScrollOffset >= CAMERA_VSCROLL_DOWN)
			vScrollOffset -= CAMERA_VSCROLL_DOWN;
		else
			vScrollOffset = 0;
	}
	else
	{
		//Get our scroll speed
		if (lookPan)
			scrollSpeed = 2;
		else
			scrollSpeed = (abs(trackPlayer->inertia) >= 0x800) ? 16 : 6;
	}
	
	if (vScrollOffset < 0)
	{
		//Scroll upwards (cap to scrollSpeed)
		if (vScrollOffset <= -scrollSpeed)
			vScrollOffset = -scrollSpeed;
		y += vScrollOffset;
		
		//Keep within level boundaries
		if (y < gLevel->topBoundary)
			y = gLevel->topBoundary;
	}
	else if (vScrollOffset > 0)
	{
		//Scroll downwards (cap to scrollSpeed)
		if (vScrollOffset > scrollSpeed)
			vScrollOffset = scrollSpeed;
		y += vScrollOffset;
		
		//Keep within level boundaries
		if (y + SCREEN_HEIGHT > gLevel->bottomBoundary)
			y = gLevel->bottomBoundary - SCREEN_HEIGHT;
	}
	
	#ifdef CAMERA_CD_PAN
		//Handle CD panning
		if (trackPlayer->spindashing)
		{
			if (trackPlayer->status.xFlip)
				xPan = min(xPan + 2,  64);
			else
				xPan = max(xPan - 2, -64);
		}
		else if (abs(trackPlayer->inertia) >= 0x600)
		{
			if (trackPlayer->inertia < 0)
				xPan = min(xPan + 2,  64);
			else
				xPan = max(xPan - 2, -64);
		}
		else
		{
			//Pan back to the center
			if (xPan > 0)
				xPan = max(xPan - 2,   0);
			else if (xPan < 0)
				xPan = min(xPan + 2,   0);
		}
	#endif
}
