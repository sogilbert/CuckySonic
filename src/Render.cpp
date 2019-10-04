#include "SDL_render.h"
#include "SDL_timer.h"

#include "Render.h"
#include "Error.h"
#include "Log.h"
#include "Path.h"
#include "GameConstants.h"

//Window and renderer
SDL_Window *gWindow;
SDL_Renderer *gRenderer;
SOFTWAREBUFFER *gSoftwareBuffer;

SDL_PixelFormat *gNativeFormat;

//Current render specifications
RENDERSPEC gRenderSpec = {426, 240, 2};

//VSync / framerate limiter
unsigned int vsyncMultiple = 0;
const long double framerateMilliseconds = (1000.0 / FRAMERATE);

//Texture class
TEXTURE::TEXTURE(TEXTURE **linkedList, const char *path)
{
	LOG(("Loading texture from %s... ", path));
	memset(this, 0, sizeof(TEXTURE));
	
	//Load bitmap
	source = path;
	
	GET_GLOBAL_PATH(filepath, path);
	
	SDL_Surface *bitmap = SDL_LoadBMP(filepath);
	if (bitmap == nullptr)
	{
		fail = SDL_GetError();
		return;
	}
	
	//Check if this format is valid
	if (bitmap->format->palette == nullptr || bitmap->format->BytesPerPixel > 1)
	{
		SDL_FreeSurface(bitmap);
		fail = "Bitmap is not an 8-bit indexed .bmp";
		return;
	}
	
	//Create our palette using the image's palette
	if ((loadedPalette = new PALETTE) == nullptr)
	{
		SDL_FreeSurface(bitmap);
		fail = "Failed to allocate palette for texture";
		return;
	}
	
	int i;
	for (i = 0; i < bitmap->format->palette->ncolors; i++)
		SetPaletteColour(&loadedPalette->colour[i], bitmap->format->palette->colors[i].r, bitmap->format->palette->colors[i].g, bitmap->format->palette->colors[i].b);
	for (; i < 0x100; i++)
	{
		if (bitmap->format->palette->ncolors)
			loadedPalette->colour[i] = loadedPalette->colour[0];
		else
			SetPaletteColour(&loadedPalette->colour[i], 0, 0, 0);
	}
	
	//Allocate texture data
	texture = (uint8_t*)malloc(bitmap->pitch * bitmap->h);
	
	if (texture == nullptr)
	{
		SDL_FreeSurface(bitmap);
		delete loadedPalette;
		
		fail = "Failed to allocate texture buffer";
		return;
	}
	
	//Copy our data
	memcpy(texture, bitmap->pixels, bitmap->pitch * bitmap->h);
	width = bitmap->pitch;
	height = bitmap->h;
	
	//Free bitmap surface
	SDL_FreeSurface(bitmap);
	
	//Attach to linked list if given
	if (linkedList != nullptr)
	{
		list = linkedList;
		next = *linkedList;
		*linkedList = this;
	}
	
	LOG(("Success!\n"));
}

TEXTURE::TEXTURE(TEXTURE **linkedList, uint8_t *data, int dWidth, int dHeight)
{
	LOG(("Loading texture from memory location %p dimensions %dx%d... ", (void*)data, dWidth, dHeight));
	memset(this, 0, sizeof(TEXTURE));
	
	//Allocate texture data
	texture = (uint8_t*)malloc(dWidth * dHeight);
	
	if (texture == nullptr)
	{
		fail = "Failed to allocate texture buffer";
		return;
	}
	
	//Copy our data
	memcpy(texture, data, dWidth * dHeight);
	width = dWidth;
	height = dHeight;
	
	//Attach to linked list if given
	if (linkedList != nullptr)
	{
		list = linkedList;
		next = *linkedList;
		*linkedList = this;
	}
	
	LOG(("Success!\n"));
}

TEXTURE::~TEXTURE()
{
	//Check if any software buffers are referencing us and remove said entries
	for (int i = 0; i < RENDERLAYERS; i++)
	{
		for (RENDERQUEUE *entry = gSoftwareBuffer->queue[i]; entry != nullptr; entry = entry->next)
		{
			if (entry->type == RENDERQUEUE_TEXTURE && entry->texture.texture == this)
			{
				//Remove from linked list then delete entry
				for (RENDERQUEUE **dcEntry = &gSoftwareBuffer->queue[i]; *dcEntry != nullptr; dcEntry = &(*dcEntry)->next)
				{
					if (*dcEntry == entry)
					{
						*dcEntry = entry->next;
						break;
					}
				}
				
				delete entry;
			}
		}
	}
	//Free data
	free(texture);
	if (loadedPalette)
		delete loadedPalette;
}

//Palette functions
void SetPaletteColour(PALCOLOUR *palColour, uint8_t r, uint8_t g, uint8_t b)
{
	//Set formatted colour
	palColour->colour = SDL_MapRGB(gNativeFormat, r, g, b);
	
	//Set colours
	palColour->r = r;
	palColour->g = g;
	palColour->b = b;
	
	//Set original colours
	palColour->ogr = r;
	palColour->ogg = g;
	palColour->ogb = b;
}

void ModifyPaletteColour(PALCOLOUR *palColour, uint8_t r, uint8_t g, uint8_t b)
{
	//Set formatted colour
	palColour->colour = SDL_MapRGB(gNativeFormat, r, g, b);
	
	//Set colours
	palColour->r = r;
	palColour->g = g;
	palColour->b = b;
}

void RegenPaletteColour(PALCOLOUR *palColour)
{
	//Set formatted colour
	palColour->colour = SDL_MapRGB(gNativeFormat, palColour->r, palColour->g, palColour->b);
}

//Software buffer class
SOFTWAREBUFFER::SOFTWAREBUFFER(int bufWidth, int bufHeight)
{
	//Set our properties
	memset(this, 0, sizeof(SOFTWAREBUFFER));
	width = bufWidth;
	height = bufHeight;
	
	//Create the render texture
	if ((texture = SDL_CreateTexture(gRenderer, gNativeFormat->format, SDL_TEXTUREACCESS_STREAMING, width, height)) == nullptr)
	{
		fail = SDL_GetError();
		return;
	}
}

SOFTWAREBUFFER::~SOFTWAREBUFFER()
{
	//Free our render texture
	SDL_DestroyTexture(texture);
}

//Drawing functions
void SOFTWAREBUFFER::DrawPoint(int layer, int x, int y, PALCOLOUR *colour)
{
	//Check if this is in view bounds (if not, just return, no point in clogging the queue with stuff that will not be rendered)
	if (x < 0 || x >= width)
		return;
	if (y < 0 || y >= height)
		return;
	
	//Allocate our new queue entry
	RENDERQUEUE *newEntry = new RENDERQUEUE;
	newEntry->type = RENDERQUEUE_SOLID;
	newEntry->dest = {x, y, 1, 1};
	newEntry->solid.colour = colour;
	
	//Link to the queue
	newEntry->next = queue[layer];
	queue[layer] = newEntry;
}

void SOFTWAREBUFFER::DrawQuad(int layer, const SDL_Rect *quad, PALCOLOUR *colour)
{
	//Allocate our new queue entry
	RENDERQUEUE *newEntry = new RENDERQUEUE;
	newEntry->type = RENDERQUEUE_SOLID;
	newEntry->dest = *quad;
	
	//Clip top & left
	if (quad->x < 0)
	{
		newEntry->dest.x -= quad->x;
		newEntry->dest.w += quad->x;
	}
	
	if (quad->y < 0)
	{
		newEntry->dest.y -= quad->y;
		newEntry->dest.h += quad->y;
	}
	
	//Clip right and bottom
	if (newEntry->dest.x > (width - newEntry->dest.w))
		newEntry->dest.w -= newEntry->dest.x - (width - newEntry->dest.w);
	if (newEntry->dest.y > (height - newEntry->dest.h))
		newEntry->dest.h -= newEntry->dest.y - (height - newEntry->dest.h);
	
	//Quit if clipped off-screen
	if (newEntry->dest.w <= 0 || newEntry->dest.h <= 0)
		return;
	
	//Set colour reference
	newEntry->solid.colour = colour;
	
	//Link to the queue
	newEntry->next = queue[layer];
	queue[layer] = newEntry;
}

void SOFTWAREBUFFER::DrawTexture(TEXTURE *texture, PALETTE *palette, const SDL_Rect *src, int layer, int x, int y, bool xFlip, bool yFlip)
{
	//Get the source rect to use (nullptr = entire texture)
	SDL_Rect newSrc;
	if (src != nullptr)
		newSrc = *src;
	else
		newSrc = {0, 0, texture->width, texture->height};
	
	//Clip to the destination
	if (x < 0)
	{
		if (!xFlip)
			newSrc.x -= x;
		newSrc.w += x;
		x -= x;
	}
	
	int dx = x + newSrc.w - width;
	if (dx > 0)
	{
		if (xFlip)
			newSrc.x += dx;
		newSrc.w -= dx;
	}
	
	if (y < 0)
	{
		if (!yFlip)
			newSrc.y -= y;
		newSrc.h += y;
		y -= y;
	}
	
	int dy = y + newSrc.h - height;
	if (dy > 0)
	{
		if (yFlip)
			newSrc.y += dy;
		newSrc.h -= dy;
	}
	
	//Allocate our new queue entry
	RENDERQUEUE *newEntry = new RENDERQUEUE;
	newEntry->type = RENDERQUEUE_TEXTURE;
	newEntry->dest = {x, y, newSrc.w, newSrc.h};
	newEntry->texture.srcX = newSrc.x;
	newEntry->texture.srcY = newSrc.y;
	
	//Set palette and texture references
	newEntry->texture.palette = palette;
	newEntry->texture.texture = texture;
	
	//Flipping
	newEntry->texture.xFlip = xFlip;
	newEntry->texture.yFlip = yFlip;
	
	//Link to the queue
	newEntry->next = queue[layer];
	queue[layer] = newEntry;
}

//Primary render function
bool SOFTWAREBUFFER::RenderToScreen(PALCOLOUR *backgroundColour)
{
	//Lock texture
	void *writeBuffer;
	int writePitch;
	if (SDL_LockTexture(texture, nullptr, &writeBuffer, &writePitch) < 0)
		return Error(SDL_GetError());
	
	//Render to our buffer
	const uint8_t bpp = gNativeFormat->BytesPerPixel;
	
	switch (bpp)
	{
		case 1:
			Blit8 (backgroundColour,  (uint8_t*)writeBuffer, writePitch / 1);
			break;
		case 2:
			Blit16(backgroundColour, (uint16_t*)writeBuffer, writePitch / 2);
			break;
		case 4:
			Blit32(backgroundColour, (uint32_t*)writeBuffer, writePitch / 4);
			break;
		default:
			return Error("Unsupported BPP");
	}
	
	//Clear all layers
	for (int i = 0; i < RENDERLAYERS; i++)
	{
		//Unload all entries
		for (RENDERQUEUE *entry = queue[i]; entry != nullptr;)
		{
			RENDERQUEUE *next = entry->next;
			delete entry;
			entry = next;
		}
		
		//Reset queue
		queue[i] = nullptr;
	}
	
	//Unlock
	SDL_UnlockTexture(texture);
	
	//Copy to renderer (this is also where the scaling happens, incidentally)
	if (SDL_RenderCopy(gRenderer, texture, nullptr, nullptr) < 0)
		return Error(SDL_GetError());
	
	//Present renderer then wait for next frame (either use VSync if applicable, or just wait)
	if (vsyncMultiple != 0)
	{
		//Present gRenderer X amount of times, to call VSync that many times (hack-ish)
		for (unsigned int iteration = 0; iteration < vsyncMultiple; iteration++)
			SDL_RenderPresent(gRenderer);
	}
	else
	{
		//Present gRenderer, then wait for next frame
		SDL_RenderPresent(gRenderer);
		
		//Framerate limiter
		static long double timePrev;
		const uint32_t timeNow = SDL_GetTicks();
		const long double timeNext = timePrev + framerateMilliseconds;
		
		if (timeNow >= timePrev + 100)
		{
			timePrev = (long double)timeNow;
		}
		else
		{
			if (timeNow < timeNext)
				SDL_Delay(timeNext - timeNow);
			timePrev += framerateMilliseconds;
		}
	}
	
	return true;
}

//Sub-system
bool RefreshRenderer()
{
	//Destroy old renderer and software buffer
	if (gSoftwareBuffer)
		delete gSoftwareBuffer;
	if (gRenderer != nullptr)
		SDL_DestroyRenderer(gRenderer);
	
	//Create the renderer based off of our VSync settings
	if ((gRenderer = SDL_CreateRenderer(gWindow, -1, SDL_RENDERER_ACCELERATED | (vsyncMultiple > 0 ? SDL_RENDERER_PRESENTVSYNC : 0))) == nullptr)
		return Error(SDL_GetError());
	
	//Create our software buffer
	gSoftwareBuffer = new SOFTWAREBUFFER(gRenderSpec.width, gRenderSpec.height);
	if (gSoftwareBuffer->fail)
		return Error(gSoftwareBuffer->fail);
	return true;
}

bool RefreshWindow()
{
	//Destroy old window
	if (gWindow != nullptr)
		SDL_DestroyWindow(gWindow);
	
	//Create our window
	if ((gWindow = SDL_CreateWindow(WINDOW_TITLE, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, gRenderSpec.width * gRenderSpec.scale, gRenderSpec.height * gRenderSpec.scale, 0)) == nullptr)
		return Error(SDL_GetError());
	
	//Free old window pixel format and get new one
	if (gNativeFormat != nullptr)
		SDL_FreeFormat(gNativeFormat);
	if ((gNativeFormat = SDL_AllocFormat(SDL_GetWindowPixelFormat(gWindow))) == nullptr)
		return Error(SDL_GetError());
	
	//Regenerate our renderer and software buffer
	return RefreshRenderer();
}

bool RenderCheckVSync()
{
	//Use display mode to detect VSync
	SDL_DisplayMode mode;
	if (SDL_GetWindowDisplayMode(gWindow, &mode) < 0)
		return Error(SDL_GetError());
	
	long double refreshIntegral;
	long double refreshFractional = std::modf((long double)mode.refresh_rate / FRAMERATE, &refreshIntegral);
	
	if (refreshIntegral >= 1.0 && refreshFractional == 0.0)
		vsyncMultiple = (unsigned int)refreshIntegral;
	
	//Regenerate our renderer and software buffer
	return RefreshRenderer();
}

bool InitializeRender()
{
	LOG(("Initializing renderer... "));
	
	//Create window, renderer, and software buffer
	if (!RefreshWindow())
		return false;
	
	LOG(("Success!\n"));
	return true;
}

void QuitRender()
{
	LOG(("Ending renderer... "));
	
	//Destroy window, renderer, and software buffer
	if (gSoftwareBuffer)
		delete gSoftwareBuffer;
	SDL_DestroyRenderer(gRenderer);
	SDL_DestroyWindow(gWindow);
	SDL_FreeFormat(gNativeFormat);
	
	LOG(("Success!\n"));
}
