/*
	Copyright (C) 2006 yopyop
	Copyright (C) 2006-2007 Theo Berkau
	Copyright (C) 2007 shash
	Copyright (C) 2008-2015 DeSmuME team

	This file is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 2 of the License, or
	(at your option) any later version.

	This file is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with the this software.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "GPU.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <iostream>

#include "MMU.h"
#include "FIFO.h"
#include "debug.h"
#include "render3D.h"
#include "registers.h"
#include "gfx3d.h"
#include "debug.h"
#include "GPU_osd.h"
#include "NDSSystem.h"
#include "readwrite.h"
#include "matrix.h"
#include "emufile.h"

#ifdef FASTBUILD
	#undef FORCEINLINE
	#define FORCEINLINE
	//compilation speed hack (cuts time exactly in half by cutting out permutations)
	#define DISABLE_MOSAIC
#endif

NDS_Screen MainScreen;
NDS_Screen SubScreen;

//instantiate static instance
GPU::MosaicLookup GPU::mosaicLookup;

//#define DEBUG_TRI

u16 *GPU_screen = NULL;
static size_t _gpuFramebufferWidth = 256;
static size_t _gpuFramebufferHeight = 192;

CACHE_ALIGN u8 sprWin[256];

u16 gpu_angle = 0;

const size sprSizeTab[4][4] = 
{
     {{8, 8}, {16, 8}, {8, 16}, {8, 8}},
     {{16, 16}, {32, 8}, {8, 32}, {8, 8}},
     {{32, 32}, {32, 16}, {16, 32}, {8, 8}},
     {{64, 64}, {64, 32}, {32, 64}, {8, 8}},
};

const BGType GPU_mode2type[8][4] = 
{
      {BGType_Text, BGType_Text, BGType_Text, BGType_Text},
      {BGType_Text, BGType_Text, BGType_Text, BGType_Affine},
      {BGType_Text, BGType_Text, BGType_Affine, BGType_Affine},
      {BGType_Text, BGType_Text, BGType_Text, BGType_AffineExt},
      {BGType_Text, BGType_Text, BGType_Affine, BGType_AffineExt},
      {BGType_Text, BGType_Text, BGType_AffineExt, BGType_AffineExt},
      {BGType_Invalid, BGType_Invalid, BGType_Large8bpp, BGType_Invalid},
      {BGType_Invalid, BGType_Invalid, BGType_Invalid, BGType_Invalid}
};

//dont ever think of changing these to bits because you could avoid the multiplies in the main tile blitter.
//it doesnt really help any
const short sizeTab[8][4][2] =
{
	{{0, 0}, {0, 0}, {0, 0}, {0, 0}}, //Invalid
    {{256,256}, {512,256}, {256,512}, {512,512}}, //text
    {{128,128}, {256,256}, {512,512}, {1024,1024}}, //affine
    {{512,1024}, {1024,512}, {0,0}, {0,0}}, //large 8bpp
	{{0, 0}, {0, 0}, {0, 0}, {0, 0}}, //affine ext (to be elaborated with another value)
	{{128,128}, {256,256}, {512,512}, {1024,1024}}, //affine ext 256x16
	{{128,128}, {256,256}, {512,256}, {512,512}}, //affine ext 256x1
	{{128,128}, {256,256}, {512,256}, {512,512}}, //affine ext direct
};

static u8 *win_empty = NULL;
static CACHE_ALIGN u16 fadeInColors[17][0x8000];
CACHE_ALIGN u16 fadeOutColors[17][0x8000];

//this should be public, because it gets used somewhere else
CACHE_ALIGN u8 gpuBlendTable555[17][17][32][32];


/*****************************************************************************/
//			INITIALIZATION
/*****************************************************************************/


static void GPU_InitFadeColors()
{
	/*
	NOTE: gbatek (in the reference above) seems to expect 6bit values 
	per component, but as desmume works with 5bit per component, 
	we use 31 as top, instead of 63. Testing it on a few games, 
	using 63 seems to give severe color wraping, and 31 works
	nicely, so for now we'll just that, until proven wrong.

	i have seen pics of pokemon ranger getting white with 31, with 63 it is nice.
	it could be pb of alpha or blending or...

	MightyMax> created a test NDS to check how the brightness values work,
	and 31 seems to be correct. FactorEx is a override for max brighten/darken
	See: http://mightymax.org/gfx_test_brightness.nds
	The Pokemon Problem could be a problem with 8/32 bit writes not recognized yet,
	i'll add that so you can check back.

	*/

	for(int i = 0; i <= 16; i++)
	{
		for(int j = 0x8000; j < 0x10000; j++)
		{
			COLOR cur;

			cur.val = j;
			cur.bits.red = (cur.bits.red + ((31 - cur.bits.red) * i / 16));
			cur.bits.green = (cur.bits.green + ((31 - cur.bits.green) * i / 16));
			cur.bits.blue = (cur.bits.blue + ((31 - cur.bits.blue) * i / 16));
			cur.bits.alpha = 0;
			fadeInColors[i][j & 0x7FFF] = cur.val;

			cur.val = j;
			cur.bits.red = (cur.bits.red - (cur.bits.red * i / 16));
			cur.bits.green = (cur.bits.green - (cur.bits.green * i / 16));
			cur.bits.blue = (cur.bits.blue - (cur.bits.blue * i / 16));
			cur.bits.alpha = 0;
			fadeOutColors[i][j & 0x7FFF] = cur.val;
		}
	}


	for(int c0=0;c0<=31;c0++) 
		for(int c1=0;c1<=31;c1++) 
			for(int eva=0;eva<=16;eva++)
				for(int evb=0;evb<=16;evb++)
				{
					int blend = ((c0 * eva) + (c1 * evb) ) / 16;
					int final = std::min<int>(31,blend);
					gpuBlendTable555[eva][evb][c0][c1] = final;
				}
}

static CACHE_ALIGN GPU GPU_main, GPU_sub;

GPU* GPU_Init(const GPUCoreID coreID)
{
	GPU *gpu = (coreID == GPUCOREID_MAIN) ? &GPU_main : &GPU_sub;
	gpu->core = coreID;
	gpu->tempScanlineBuffer = NULL;
	gpu->bgPixels = NULL;
	gpu->h_win[0] = NULL;
	gpu->h_win[1] = NULL;

	GPU_Reset(gpu);
	GPU_InitFadeColors();

	gpu->curr_win[0] = win_empty;
	gpu->curr_win[1] = win_empty;
	gpu->need_update_winh[0] = true;
	gpu->need_update_winh[1] = true;
	gpu->setFinalColorBck_funcNum = 0;
	gpu->setFinalColor3d_funcNum = 0;
	gpu->setFinalColorSpr_funcNum = 0;

	return gpu;
}

void GPU_Reset(GPU *gpu)
{
	// TODO: The memset() here completely wipes out the GPU structure, so any states
	// that need to be persistent will be lost here. So we're just going to save
	// whatever persistent states we need now, do the memset(), and then restore them.
	// It would be better if we were more precise about GPU resets.
	const GPUCoreID currentCoreID = gpu->core;
	u16 *currentTempScanlineBuffer = gpu->tempScanlineBuffer;
	u8 *currentBGPixels = gpu->bgPixels;
	u8 *currentHWin0 = gpu->h_win[0];
	u8 *currentHWin1 = gpu->h_win[1];
	
	memset(gpu, 0, sizeof(GPU));
	
	gpu->core = currentCoreID;
	gpu->tempScanlineBuffer = currentTempScanlineBuffer;
	gpu->bgPixels = currentBGPixels;
	gpu->h_win[0] = currentHWin0;
	gpu->h_win[1] = currentHWin1;
	
	// Clear the separate memory blocks that weren't cleared in the last memset()
	const float yScale = (float)_gpuFramebufferHeight / 192.0f;
	size_t lineCount = (size_t)ceilf(yScale);
	
	if (gpu->tempScanlineBuffer != NULL) memset(gpu->tempScanlineBuffer, 0, _gpuFramebufferWidth * lineCount * sizeof(u16));
	if (gpu->bgPixels != NULL) memset(gpu->bgPixels, 0, _gpuFramebufferWidth * lineCount * 4 * sizeof(u8));
	if (gpu->h_win[0] != NULL) memset(gpu->h_win[0], 0, _gpuFramebufferWidth * sizeof(u8));
	if (gpu->h_win[1] != NULL) memset(gpu->h_win[1], 0, _gpuFramebufferWidth * sizeof(u8));
	
	//important for emulator stability for this to initialize, since we have to setup a table based on it
	gpu->BLDALPHA_EVA = 0;
	gpu->BLDALPHA_EVB = 0;
	//make sure we have our blend table setup even if the game blends without setting the blend variables
	gpu->updateBLDALPHA();

	gpu->setFinalColorBck_funcNum = 0;
	gpu->setFinalColor3d_funcNum = 0;
	gpu->setFinalColorSpr_funcNum = 0;
	gpu->BGSize[0][0] = gpu->BGSize[1][0] = gpu->BGSize[2][0] = gpu->BGSize[3][0] = 256;
	gpu->BGSize[0][1] = gpu->BGSize[1][1] = gpu->BGSize[2][1] = gpu->BGSize[3][1] = 256;

	gpu->spriteRenderMode = GPU::SPRITE_1D;

	gpu->bgPrio[4] = 0xFF;

	gpu->bg0HasHighestPrio = TRUE;

	if (gpu->core == GPUCOREID_SUB)
	{
		gpu->oam = (MMU.ARM9_OAM + ADDRESS_STEP_1KB);
		gpu->sprMem = MMU_BOBJ;
		// GPU core B
		gpu->dispx_st = (REG_DISPx*)(&MMU.ARM9_REG[REG_DISPB]);
	}
	else
	{
		gpu->oam = (MMU.ARM9_OAM);
		gpu->sprMem = MMU_AOBJ;
		// GPU core A
		gpu->dispx_st = (REG_DISPx*)(&MMU.ARM9_REG[0]);
	}
}

void GPU_DeInit(GPU *gpu)
{
	if (gpu == &GPU_main || gpu == &GPU_sub) return;
	
	free(gpu->tempScanlineBuffer);
	gpu->tempScanlineBuffer = NULL;
	free(gpu->bgPixels);
	gpu->bgPixels = NULL;
	
	free(gpu->h_win[0]);
	gpu->h_win[0] = NULL;
	free(gpu->h_win[1]);
	gpu->h_win[1] = NULL;
	
	free(gpu);
	gpu = NULL;
}

static void GPU_resortBGs(GPU *gpu)
{
	int i, prio;
	struct _DISPCNT * cnt = &gpu->dispx_st->dispx_DISPCNT.bits;
	itemsForPriority_t * item;

	// we don't need to check for windows here...
// if we tick boxes, invisible layers become invisible & vice versa
#define OP ^ !
// if we untick boxes, layers become invisible
//#define OP &&
	gpu->LayersEnable[0] = CommonSettings.dispLayers[gpu->core][0] OP(cnt->BG0_Enable/* && !(cnt->BG0_3D && (gpu->core==0))*/);
	gpu->LayersEnable[1] = CommonSettings.dispLayers[gpu->core][1] OP(cnt->BG1_Enable);
	gpu->LayersEnable[2] = CommonSettings.dispLayers[gpu->core][2] OP(cnt->BG2_Enable);
	gpu->LayersEnable[3] = CommonSettings.dispLayers[gpu->core][3] OP(cnt->BG3_Enable);
	gpu->LayersEnable[4] = CommonSettings.dispLayers[gpu->core][4] OP(cnt->OBJ_Enable);

	// KISS ! lower priority first, if same then lower num
	for (i=0;i<NB_PRIORITIES;i++) {
		item = &(gpu->itemsForPriority[i]);
		item->nbBGs=0;
		item->nbPixelsX=0;
	}
	for (i=NB_BG; i>0; ) {
		i--;
		if (!gpu->LayersEnable[i]) continue;
		prio = (gpu->dispx_st)->dispx_BGxCNT[i].bits.Priority;
		item = &(gpu->itemsForPriority[prio]);
		item->BGs[item->nbBGs]=i;
		item->nbBGs++;
	}

	int bg0Prio = gpu->dispx_st->dispx_BGxCNT[0].bits.Priority;
	gpu->bg0HasHighestPrio = TRUE;
	for(i = 1; i < 4; i++)
	{
		if(gpu->LayersEnable[i])
		{
			if(gpu->dispx_st->dispx_BGxCNT[i].bits.Priority < bg0Prio)
			{
				gpu->bg0HasHighestPrio = FALSE;
				break;
			}
		}
	}
	
#if 0
//debug
	for (i=0;i<NB_PRIORITIES;i++) {
		item = &(gpu->itemsForPriority[i]);
		printf("%d : ", i);
		for (j=0; j<NB_PRIORITIES; j++) {
			if (j<item->nbBGs) 
				printf("BG%d ", item->BGs[j]);
			else
				printf("... ", item->BGs[j]);
		}
	}
	printf("\n");
#endif
}

static FORCEINLINE u16 _blend(const u16 colA, const u16 colB, GPU::TBlendTable *blendTable)
{
	u8 r = (*blendTable)[colA&0x1F][colB&0x1F];
	u8 g = (*blendTable)[(colA>>5)&0x1F][(colB>>5)&0x1F];
	u8 b = (*blendTable)[(colA>>10)&0x1F][(colB>>10)&0x1F];

	return r|(g<<5)|(b<<10);
}

FORCEINLINE u16 GPU::blend(const u16 colA, const u16 colB)
{
	return _blend(colA, colB, blendTable);
}


void GPU_setMasterBrightness(GPU *gpu, const u16 val)
{
	if (!nds.isInVblank())
	{
		PROGINFO("Changing master brightness outside of vblank\n");
	}
	
 	gpu->MasterBrightFactor = (val & 0x1F);
	gpu->MasterBrightMode = (GPUMasterBrightMode)(val>>14);
	//printf("MASTER BRIGHTNESS %d to %d at %d\n",gpu->core,gpu->MasterBrightFactor,nds.VCount);

}

void SetupFinalPixelBlitter(GPU *gpu)
{
	u8 windowUsed = (gpu->WIN0_ENABLED | gpu->WIN1_ENABLED | gpu->WINOBJ_ENABLED);
	u8 blendMode = (gpu->BLDCNT >> 6)&3;

	gpu->setFinalColorSpr_funcNum = windowUsed*4 + blendMode;
	gpu->setFinalColorBck_funcNum = windowUsed*4 + blendMode;
	gpu->setFinalColor3d_funcNum = windowUsed*4 + blendMode;
	
}
    
//Sets up LCD control variables for Display Engines A and B for quick reading
void GPU_setVideoProp(GPU *gpu, u32 p)
{
	struct _DISPCNT *cnt;
	cnt = &(gpu->dispx_st)->dispx_DISPCNT.bits;

	gpu->dispx_st->dispx_DISPCNT.val = LE_TO_LOCAL_32(p);

	gpu->WIN0_ENABLED	= cnt->Win0_Enable;
	gpu->WIN1_ENABLED	= cnt->Win1_Enable;
	gpu->WINOBJ_ENABLED = cnt->WinOBJ_Enable;

	SetupFinalPixelBlitter(gpu);

    gpu->dispMode = (GPUDisplayMode)( cnt->DisplayMode & ((gpu->core)?1:3) );

	gpu->vramBlock = cnt->VRAM_Block;

	switch (gpu->dispMode)
	{
		case GPUDisplayMode_Off: // Display Off
			break;
		case GPUDisplayMode_Normal: // Display BG and OBJ layers
			break;
		case GPUDisplayMode_VRAM: // Display framebuffer
			gpu->VRAMaddr = (u8 *)MMU.ARM9_LCD + (gpu->vramBlock * 0x20000);
			break;
		case GPUDisplayMode_MainMemory: // Display from Main RAM
			// nothing to be done here
			// see GPU_RenderLine who gets data from FIFO.
			break;
	}

	if (cnt->OBJ_Tile_mapping)
	{
		//1-d sprite mapping boundaries:
		//32k, 64k, 128k, 256k
		gpu->sprBoundary = 5 + cnt->OBJ_Tile_1D_Bound ;
		
		//do not be deceived: even though a sprBoundary==8 (256KB region) is impossible to fully address
		//in GPU_SUB, it is still fully legal to address it with that granularity.
		//so don't do this: //if((gpu->core == GPU_SUB) && (cnt->OBJ_Tile_1D_Bound == 3)) gpu->sprBoundary = 7;

		gpu->spriteRenderMode = GPU::SPRITE_1D;
	}
	else
	{
		//2d sprite mapping
		//boundary : 32k
		gpu->sprBoundary = 5;
		gpu->spriteRenderMode = GPU::SPRITE_2D;
	}
     
	if (cnt->OBJ_BMP_1D_Bound && (gpu->core == GPUCOREID_MAIN))
		gpu->sprBMPBoundary = 8;
	else
		gpu->sprBMPBoundary = 7;

	gpu->sprEnable = cnt->OBJ_Enable;
	
	GPU_setBGProp(gpu, 3, T1ReadWord(MMU.ARM9_REG, gpu->core * ADDRESS_STEP_4KB + 14));
	GPU_setBGProp(gpu, 2, T1ReadWord(MMU.ARM9_REG, gpu->core * ADDRESS_STEP_4KB + 12));
	GPU_setBGProp(gpu, 1, T1ReadWord(MMU.ARM9_REG, gpu->core * ADDRESS_STEP_4KB + 10));
	GPU_setBGProp(gpu, 0, T1ReadWord(MMU.ARM9_REG, gpu->core * ADDRESS_STEP_4KB + 8));
	
	//GPU_resortBGs(gpu);
}

//this handles writing in BGxCNT
void GPU_setBGProp(GPU *gpu, u16 num, u16 p)
{
	struct _BGxCNT *cnt = &((gpu->dispx_st)->dispx_BGxCNT[num].bits);
	struct _DISPCNT *dispCnt = &(gpu->dispx_st)->dispx_DISPCNT.bits;
	
	gpu->dispx_st->dispx_BGxCNT[num].val = LE_TO_LOCAL_16(p);
	
	GPU_resortBGs(gpu);

	if (gpu->core == GPUCOREID_SUB)
	{
		gpu->BG_tile_ram[num] = MMU_BBG;
		gpu->BG_bmp_ram[num]  = MMU_BBG;
		gpu->BG_bmp_large_ram[num]  = MMU_BBG;
		gpu->BG_map_ram[num]  = MMU_BBG;
	} 
	else 
	{
		gpu->BG_tile_ram[num] = MMU_ABG +  dispCnt->CharacBase_Block * ADDRESS_STEP_64KB ;
		gpu->BG_bmp_ram[num]  = MMU_ABG;
		gpu->BG_bmp_large_ram[num] = MMU_ABG;
		gpu->BG_map_ram[num]  = MMU_ABG +  dispCnt->ScreenBase_Block * ADDRESS_STEP_64KB;
	}

	gpu->BG_tile_ram[num] += (cnt->CharacBase_Block * ADDRESS_STEP_16KB);
	gpu->BG_bmp_ram[num]  += (cnt->ScreenBase_Block * ADDRESS_STEP_16KB);
	gpu->BG_map_ram[num]  += (cnt->ScreenBase_Block * ADDRESS_STEP_2KB);

	switch(num)
	{
		case 0:
		case 1:
			gpu->BGExtPalSlot[num] = cnt->PaletteSet_Wrap * 2 + num ;
			break;
			
		default:
			gpu->BGExtPalSlot[num] = (u8)num;
			break;
	}

	BGType mode = GPU_mode2type[dispCnt->BG_Mode][num];

	//clarify affine ext modes 
	if(mode == BGType_AffineExt)
	{
		//see: http://nocash.emubase.de/gbatek.htm#dsvideobgmodescontrol
		u8 affineModeSelection = (cnt->Palette_256 << 1) | (cnt->CharacBase_Block & 1) ;
		switch(affineModeSelection)
		{
		case 0:
		case 1:
			mode = BGType_AffineExt_256x16;
			break;
		case 2:
			mode = BGType_AffineExt_256x1;
			break;
		case 3:
			mode = BGType_AffineExt_Direct;
			break;
		}
	}

	gpu->BGTypes[num] = mode;

	gpu->BGSize[num][0] = sizeTab[mode][cnt->ScreenSize][0];
	gpu->BGSize[num][1] = sizeTab[mode][cnt->ScreenSize][1];
	
	gpu->bgPrio[num] = (p & 0x3);
}

/*****************************************************************************/
//			ENABLING / DISABLING LAYERS
/*****************************************************************************/

void GPU_remove(GPU *gpu, const size_t num)
{
	CommonSettings.dispLayers[gpu->core][num] = false;
	GPU_resortBGs(gpu);
}

void GPU_addBack(GPU *gpu, const size_t num)
{
	CommonSettings.dispLayers[gpu->core][num] = true;
	GPU_resortBGs(gpu);
}


/*****************************************************************************/
//		ROUTINES FOR INSIDE / OUTSIDE WINDOW CHECKS
/*****************************************************************************/

template<int WIN_NUM>
FORCEINLINE u8 GPU::withinRect(const size_t x) const
{
	return curr_win[WIN_NUM][x];
}



//  Now assumes that *draw and *effect are different from 0 when called, so we can avoid
// setting some values twice
FORCEINLINE void GPU::renderline_checkWindows(const size_t x, bool &draw, bool &effect) const
{
	// Check if win0 if enabled, and only check if it is
	// howevever, this has already been taken care of by the window precalculation
	//if (WIN0_ENABLED)
	{
		// it is in win0, do we display ?
		// high priority	
		if (withinRect<0>(x))
		{
			//INFO("bg%i passed win0 : (%i %i) was within (%i %i)(%i %i)\n", bgnum, x, gpu->currLine, gpu->WIN0H0, gpu->WIN0V0, gpu->WIN0H1, gpu->WIN0V1);
			draw = (WININ0 >> currBgNum) & 1;
			effect = (WININ0_SPECIAL);
			return;
		}
	}

	// Check if win1 if enabled, and only check if it is
	//if (WIN1_ENABLED)
	// howevever, this has already been taken care of by the window precalculation
	{
		// it is in win1, do we display ?
		// mid priority
		if (withinRect<1>(x))
		{
			//INFO("bg%i passed win1 : (%i %i) was within (%i %i)(%i %i)\n", bgnum, x, gpu->currLine, gpu->WIN1H0, gpu->WIN1V0, gpu->WIN1H1, gpu->WIN1V1);
			draw = (WININ1 >> currBgNum) & 1;
			effect = (WININ1_SPECIAL);
			return;
		}
	}

	//if(true) //sprwin test hack
	if (WINOBJ_ENABLED)
	{
		// it is in winOBJ, do we display ?
		// low priority
		if (sprWin[x])
		{
			draw = (WINOBJ >> currBgNum) & 1;
			effect = (WINOBJ_SPECIAL);
			return;
		}
	}

	if (WINOBJ_ENABLED | WIN1_ENABLED | WIN0_ENABLED)
	{
		draw = (WINOUT >> currBgNum) & 1;
		effect = (WINOUT_SPECIAL);
	}
}

/*****************************************************************************/
//			PIXEL RENDERING
/*****************************************************************************/

template<BlendFunc FUNC, bool WINDOW>
FORCEINLINE FASTCALL void GPU::_master_setFinal3dColor(const size_t x, u16 &outDst, const FragmentColor src)
{
	u8 alpha = src.a;
	u16 final;

	bool windowEffect = blend1; //bomberman land touch dialogbox will fail without setting to blend1
	
	//TODO - should we do an alpha==0 -> bail out entirely check here?

	if (WINDOW)
	{
		bool windowDraw = false;
		renderline_checkWindows(x, windowDraw, windowEffect);

		//we never have anything more to do if the window rejected us
		if (!windowDraw) return;
	}

	const size_t bg_under = bgPixels[x];
	if (blend2[bg_under])
	{
		alpha++;
		if (alpha < 32)
		{
			//if the layer underneath is a blend bottom layer, then 3d always alpha blends with it
			COLOR c2, cfinal;

			c2.val = outDst;

			cfinal.bits.red = ((src.r * alpha) + ((c2.bits.red<<1) * (32 - alpha)))>>6;
			cfinal.bits.green = ((src.g * alpha) + ((c2.bits.green<<1) * (32 - alpha)))>>6;
			cfinal.bits.blue = ((src.b * alpha) + ((c2.bits.blue<<1) * (32 - alpha)))>>6;

			final = cfinal.val;
		}
		else final = R6G6B6TORGB15(src.r, src.g, src.b);
	}
	else
	{
		final = R6G6B6TORGB15(src.r, src.g, src.b);
		//perform the special effect
		if (windowEffect)
		{
			switch (FUNC)
			{
				case Increase: final = currentFadeInColors[final&0x7FFF]; break;
				case Decrease: final = currentFadeOutColors[final&0x7FFF]; break;
				case NoBlend:
				case Blend:
					break;
			}
		}
	}
	
	outDst = final | 0x8000;
	bgPixels[x] = 0;
}

template<bool BACKDROP, BlendFunc FUNC, bool WINDOW>
FORCEINLINE FASTCALL bool GPU::_master_setFinalBGColor(u16 &outColor, const size_t x)
{
	//no further analysis for no special effects. on backdrops. just draw it.
	if ((FUNC == NoBlend) && BACKDROP) return true;

	//blend backdrop with what?? this doesn't make sense
	if ((FUNC == Blend) && BACKDROP) return true;

	bool windowEffect = true;

	if (WINDOW)
	{
		bool windowDraw = false;
		renderline_checkWindows(x, windowDraw, windowEffect);

		//backdrop must always be drawn
		if (BACKDROP) windowDraw = true;

		//we never have anything more to do if the window rejected us
		if (!windowDraw) return false;
	}

	//special effects rejected. just draw it.
	if (!(blend1 && windowEffect))
		return true;

	const size_t bg_under = bgPixels[x];

	//perform the special effect
	switch (FUNC)
	{
		case Blend: if(blend2[bg_under]) outColor = blend(outColor, currDst[x]); break;
		case Increase: outColor = currentFadeInColors[outColor]; break;
		case Decrease: outColor = currentFadeOutColors[outColor]; break;
		case NoBlend: break;
	}
	return true;
}

template<BlendFunc FUNC, bool WINDOW>
static FORCEINLINE void _master_setFinalOBJColor(GPU *gpu, u16 *dstLine, u16 color, u8 alpha, u8 type, const size_t x)
{
	const bool isObjTranslucentType = type == GPU_OBJ_MODE_Transparent || type == GPU_OBJ_MODE_Bitmap;
	
	bool windowDraw = true;
	bool windowEffectSatisfied = true;

	if(WINDOW)
	{
		gpu->renderline_checkWindows(x, windowDraw, windowEffectSatisfied);
		if(!windowDraw)
			return;
	}

	//if the window effect is satisfied, then we can do color effects to modify the color
	if(windowEffectSatisfied)
	{
		const bool firstTargetSatisfied = gpu->blend1;
		const size_t bg_under = gpu->bgPixels[x];
		const bool secondTargetSatisfied = (bg_under != 4) && gpu->blend2[bg_under];
		BlendFunc selectedFunc = NoBlend;

		int eva = gpu->BLDALPHA_EVA, evb = gpu->BLDALPHA_EVB;

		//if normal BLDCNT layer target conditions are met, then we can use the BLDCNT-specified color effect
		if(FUNC == Blend)
		{
			//blending requires first and second target screens to be satisfied
			if(firstTargetSatisfied && secondTargetSatisfied) selectedFunc = FUNC;
		}
		else 
		{
			//brightness up and down requires only the first target screen to be satisfied
			if(firstTargetSatisfied) selectedFunc = FUNC;
		}

		//translucent-capable OBJ are forcing the function to blend when the second target is satisfied
		if(isObjTranslucentType && secondTargetSatisfied)
		{
			selectedFunc = Blend;
		
			//obj without fine-grained alpha are using EVA/EVB for blending. this is signified by receiving 255 in the alpha
			//it's tested by the spriteblend demo and the glory of heracles title screen
			if(alpha != 255)
			{
				eva = alpha;
				evb = 16 - alpha;
			}
		}
		
		switch (selectedFunc)
		{
			case NoBlend: break;
			case Increase: color = gpu->currentFadeInColors[color&0x7FFF]; break;
			case Decrease: color = gpu->currentFadeOutColors[color&0x7FFF]; break;
			case Blend:
				color = _blend(color, dstLine[x], &gpuBlendTable555[eva][evb]);
				break;
		}
	}
	
	dstLine[x] = color | 0x8000;
	gpu->bgPixels[x] = 4;
}

//FUNCNUM is only set for backdrop, for an optimization of looking it up early
template<bool BACKDROP, int FUNCNUM> 
FORCEINLINE void GPU::setFinalColorBG(u16 color, const size_t x)
{
	//It is not safe to assert this here.
	//This is probably the best place to enforce it, since almost every single color that comes in here
	//will be pulled from a palette that needs the top bit stripped off anyway.
	//assert((color&0x8000)==0);
	if (!BACKDROP) color &= 0x7FFF; //but for the backdrop we can easily guarantee earlier that theres no bit here

	bool draw = false;

	const int test = (BACKDROP) ? FUNCNUM : setFinalColorBck_funcNum;
	switch (test)
	{
		case 0: draw = _master_setFinalBGColor<BACKDROP,NoBlend,false>(color,x); break;
		case 1: draw = _master_setFinalBGColor<BACKDROP,Blend,false>(color,x); break;
		case 2: draw = _master_setFinalBGColor<BACKDROP,Increase,false>(color,x); break;
		case 3: draw = _master_setFinalBGColor<BACKDROP,Decrease,false>(color,x); break;
		case 4: draw = _master_setFinalBGColor<BACKDROP,NoBlend,true>(color,x); break;
		case 5: draw = _master_setFinalBGColor<BACKDROP,Blend,true>(color,x); break;
		case 6: draw = _master_setFinalBGColor<BACKDROP,Increase,true>(color,x); break;
		case 7: draw = _master_setFinalBGColor<BACKDROP,Decrease,true>(color,x); break;
		default: break;
	};

	if (BACKDROP || draw) //backdrop must always be drawn
	{
		currDst[x] = color | 0x8000;
		if (!BACKDROP) bgPixels[x] = currBgNum; //lets do this in the backdrop drawing loop, should be faster
	}
}

FORCEINLINE void GPU::setFinalColor3d(const size_t x, u16 &outDst, const FragmentColor src)
{
	switch (setFinalColor3d_funcNum)
	{
		case 0x0: _master_setFinal3dColor<NoBlend,false>(x, outDst, src); break;
		case 0x1: _master_setFinal3dColor<Blend,false>(x, outDst, src); break;
		case 0x2: _master_setFinal3dColor<Increase,false>(x, outDst, src); break;
		case 0x3: _master_setFinal3dColor<Decrease,false>(x, outDst, src); break;
		case 0x4: _master_setFinal3dColor<NoBlend,true>(x, outDst, src); break;
		case 0x5: _master_setFinal3dColor<Blend,true>(x, outDst, src); break;
		case 0x6: _master_setFinal3dColor<Increase,true>(x, outDst, src); break;
		case 0x7: _master_setFinal3dColor<Decrease,true>(x, outDst, src); break;
	};
}

FORCEINLINE void setFinalColorSpr(GPU *gpu, u16 *dst, u16 color, u8 alpha, u8 type, u16 x)
{
	switch(gpu->setFinalColorSpr_funcNum)
	{
	case 0x0: _master_setFinalOBJColor<NoBlend,false>(gpu, dst, color, alpha, type, x); break;
	case 0x1: _master_setFinalOBJColor<Blend,false>(gpu, dst, color, alpha, type, x); break;
	case 0x2: _master_setFinalOBJColor<Increase,false>(gpu, dst, color, alpha, type, x); break;
	case 0x3: _master_setFinalOBJColor<Decrease,false>(gpu, dst, color, alpha, type, x); break;
	case 0x4: _master_setFinalOBJColor<NoBlend,true>(gpu, dst, color, alpha, type, x); break;
	case 0x5: _master_setFinalOBJColor<Blend,true>(gpu, dst, color, alpha, type, x); break;
	case 0x6: _master_setFinalOBJColor<Increase,true>(gpu, dst, color, alpha, type, x); break;
	case 0x7: _master_setFinalOBJColor<Decrease,true>(gpu, dst, color, alpha, type, x); break;
	};
}

template<bool MOSAIC, bool BACKDROP>
FORCEINLINE void GPU::__setFinalColorBck(u16 color, const size_t x, const bool opaque)
{
	return ___setFinalColorBck<MOSAIC, BACKDROP, 0>(color, x, opaque);
}

//this was forced inline because most of the time it just falls through to setFinalColorBck() and the function call
//overhead was ridiculous and terrible
template<bool MOSAIC, bool BACKDROP, int FUNCNUM>
FORCEINLINE void GPU::___setFinalColorBck(u16 color, const size_t x, const bool opaque)
{
	// TODO: Temporarily clamp x here until mosaics-related memory blocks can be reworked.
	const size_t clamped_x = (x > 255) ? 255 : x;
	
	//due to this early out, we will get incorrect behavior in cases where 
	//we enable mosaic in the middle of a frame. this is deemed unlikely.
	if (!MOSAIC)
	{
		if (opaque)
		{
			setFinalColorBG<BACKDROP,FUNCNUM>(color, x);
		}
		
		return;
	}
	
	if (!opaque) color = 0xFFFF;
	else color &= 0x7FFF;
	
	if (GPU::mosaicLookup.width[clamped_x].begin && GPU::mosaicLookup.height[currLine].begin)
	{
		// Do nothing.
	}
	else
	{
		//due to the early out, enabled must always be true
		//x_int = enabled ? GPU::mosaicLookup.width[x].trunc : x;
		const size_t x_int = GPU::mosaicLookup.width[clamped_x].trunc;
		color = mosaicColors.bg[currBgNum][x_int];
	}
	
	mosaicColors.bg[currBgNum][clamped_x] = color;
	
	if (color != 0xFFFF)
	{
		setFinalColorBG<BACKDROP,FUNCNUM>(color, x);
	}
}

//unpacks an _OAM_ structure from the provided oam buffer (should point at OAM 0) and provided OAM index.
//is endian-safe
void SlurpOAM(_OAM_* oam_output, void* oam_buffer, int oam_index)
{
	u16* u16_oam_buffer = (u16*)oam_buffer;
	int u16_offset = oam_index<<2;
	u16 attr[4];
	for(int i=0;i<4;i++)
		attr[i] = LE_TO_LOCAL_16(u16_oam_buffer[u16_offset + i]);
	
	oam_output->Y = (attr[0]>>0) & 0xFF;
	oam_output->RotScale = (attr[0]>>8)&3;
	oam_output->Mode = (attr[0]>>10)&3;
	oam_output->Mosaic = (attr[0]>>12)&1;
	oam_output->Depth = (attr[0]>>13)&1;
	oam_output->Shape = (attr[0]>>14)&3;
	
	oam_output->X = (((s32)((attr[1]>>0)&0x1FF))<<23)>>23;
	oam_output->RotScalIndex = (attr[1]>>9)&7;
	oam_output->HFlip = (attr[1]>>12)&1;
	oam_output->VFlip = (attr[1]>>13)&1;
	oam_output->Size = (attr[1]>>14)&3;

	oam_output->TileIndex = (attr[2]>>0)&0x3FF;
	oam_output->Priority = (attr[2]>>10)&3;
	oam_output->PaletteIndex = (attr[2]>>12)&0xF;

	oam_output->attr3 = attr[3];
}

//gets the affine parameter associated with the specified oam index.
u16 SlurpOAMAffineParam(void* oam_buffer, int oam_index)
{
	u16* u16_oam_buffer = (u16*)oam_buffer;
	int u16_offset = oam_index<<2;
	return LE_TO_LOCAL_16(u16_oam_buffer[u16_offset + 3]);
}


//this is fantastically inaccurate.
//we do the early return even though it reduces the resulting accuracy
//because we need the speed, and because it is inaccurate anyway
static void mosaicSpriteLinePixel(GPU *gpu, const size_t x, u16 l, u16 *dst, u8 *dst_alpha, u8 *typeTab, u8 *prioTab)
{
	_OAM_ spriteInfo;
	SlurpOAM(&spriteInfo,gpu->oam,gpu->sprNum[x]);
	const bool enabled = spriteInfo.Mosaic!=0;
	if (!enabled)
		return;

	const bool opaque = prioTab[x] <= 4;

	GPU::MosaicColor::Obj objColor;
	objColor.color = LE_TO_LOCAL_16(dst[x]);
	objColor.alpha = dst_alpha[x];
	objColor.opaque = opaque;

	const size_t x_int = enabled ? GPU::mosaicLookup.width[x].trunc : x;

	if (enabled)
	{
		const size_t y = l;
		
		if (GPU::mosaicLookup.width[x].begin && GPU::mosaicLookup.height[y].begin)
		{
			// Do nothing.
		}
		else
		{
			objColor = gpu->mosaicColors.obj[x_int];
		}
	}
	gpu->mosaicColors.obj[x] = objColor;
	
	dst[x] = LE_TO_LOCAL_16(objColor.color);
	dst_alpha[x] = objColor.alpha;
	if(!objColor.opaque) prioTab[x] = 0xFF;
}

FORCEINLINE static void mosaicSpriteLine(GPU *gpu, u16 l, u16 *dst, u8 *dst_alpha, u8 *typeTab, u8 *prioTab)
{
	//don't even try this unless the mosaic is effective
	if (gpu->mosaicLookup.widthValue != 0 || gpu->mosaicLookup.heightValue != 0)
		for (size_t i = 0; i < 256; i++)
			mosaicSpriteLinePixel(gpu, i, l, dst, dst_alpha, typeTab, prioTab);
}

template<bool MOSAIC> void lineLarge8bpp(GPU *gpu)
{
	if (gpu->core == GPUCOREID_SUB)
	{
		PROGINFO("Cannot use large 8bpp screen on sub core\n");
		return;
	}

	u8 num = gpu->currBgNum;
	u16 XBG = gpu->getHOFS(gpu->currBgNum);
	u16 YBG = gpu->currLine + gpu->getVOFS(gpu->currBgNum);
	u16 lg     = gpu->BGSize[num][0];
	u16 ht     = gpu->BGSize[num][1];
	u16 wmask  = (lg-1);
	u16 hmask  = (ht-1);
	YBG &= hmask;

	//TODO - handle wrapping / out of bounds correctly from rot_scale_op?

	u32 tmp_map = gpu->BG_bmp_large_ram[num] + lg * YBG;
	u8 *map = (u8 *)MMU_gpu_map(tmp_map);

	u8 *pal = MMU.ARM9_VMEM + gpu->core * ADDRESS_STEP_1KB;

	for (size_t x = 0; x < lg; ++x, ++XBG)
	{
		XBG &= wmask;
		u8 pixel = map[XBG];
		u16 color = T1ReadWord(pal, pixel<<1);
		gpu->__setFinalColorBck<MOSAIC,false>(color,x,(color!=0));
	}
	
}

/*****************************************************************************/
//			BACKGROUND RENDERING -TEXT-
/*****************************************************************************/
// render a text background to the combined pixelbuffer
template<bool MOSAIC> INLINE void renderline_textBG(GPU * gpu, u16 XBG, u16 YBG, u16 LG)
{
	u8 num = gpu->currBgNum;
	struct _BGxCNT *bgCnt = &(gpu->dispx_st)->dispx_BGxCNT[num].bits;
	struct _DISPCNT *dispCnt = &(gpu->dispx_st)->dispx_DISPCNT.bits;
	u16 lg     = gpu->BGSize[num][0];
	u16 ht     = gpu->BGSize[num][1];
	u16 wmask  = (lg-1);
	u16 hmask  = (ht-1);
	u16 tmp    = ((YBG & hmask) >> 3);
	u32 map;
	u8 *pal, *line;
	u32 tile;
	u16 color;
	u16 xoff;
	u16 yoff;
	u32 x      = 0;
	u32 xfin;

	s8 line_dir = 1;
	u32 mapinfo;
	TILEENTRY tileentry;

	u32 tmp_map = gpu->BG_map_ram[num] + (tmp&31) * 64;
	if(tmp>31) 
		tmp_map+= ADDRESS_STEP_512B << bgCnt->ScreenSize ;

	map = tmp_map;
	tile = gpu->BG_tile_ram[num];

	xoff = XBG;
	pal = MMU.ARM9_VMEM + gpu->core * ADDRESS_STEP_1KB;

	if(!bgCnt->Palette_256)    // color: 16 palette entries
	{
		yoff = ((YBG&7)<<2);
		xfin = 8 - (xoff&7);
		for(x = 0; x < LG; xfin = std::min<u16>(x+8, LG))
		{
			u16 tilePalette = 0;
			tmp = ((xoff&wmask)>>3);
			mapinfo = map + (tmp&0x1F) * 2;
			if(tmp>31) mapinfo += 32*32*2;
			tileentry.val = T1ReadWord(MMU_gpu_map(mapinfo), 0);

			tilePalette = (tileentry.bits.Palette*16);

			line = (u8*)MMU_gpu_map(tile + (tileentry.bits.TileNum * 0x20) + ((tileentry.bits.VFlip) ? (7*4)-yoff : yoff));
			
			if(tileentry.bits.HFlip)
			{
				line += (3 - ((xoff&7)>>1));
				for(; x < xfin; line --) 
				{	
					u8 currLine = *line;

					if(!(xoff&1))
					{
						color = T1ReadWord(pal, ((currLine>>4) + tilePalette) << 1);
						gpu->__setFinalColorBck<MOSAIC,false>(color, x, (currLine>>4 != 0));
						x++; xoff++;
					}
					
					if(x<xfin)
					{
						color = T1ReadWord(pal, ((currLine&0xF) + tilePalette) << 1);
						gpu->__setFinalColorBck<MOSAIC,false>(color, x, ((currLine&0xF) != 0));
						x++; xoff++;
					}
				}
			} else {
				line += ((xoff&7)>>1);
				for(; x < xfin; line ++) 
				{
					u8 currLine = *line;

					if(!(xoff&1))
					{
						color = T1ReadWord(pal, ((currLine&0xF) + tilePalette) << 1);
						gpu->__setFinalColorBck<MOSAIC,false>(color, x, ((currLine&0xF) != 0));
						x++; xoff++;
					}

					if(x<xfin)
					{
						color = T1ReadWord(pal, ((currLine>>4) + tilePalette) << 1);
						gpu->__setFinalColorBck<MOSAIC,false>(color, x, (currLine>>4 != 0));
						x++; xoff++;
					}
				}
			}
		}
		return;
	}

	//256-color BG

	if(dispCnt->ExBGxPalette_Enable)  // color: extended palette
	{
		pal = MMU.ExtPal[gpu->core][gpu->BGExtPalSlot[num]];
		if(!pal) return;
	}

	yoff = ((YBG&7)<<3);
	u8* tilePal;
	xfin = 8 - (xoff&7);
	u32 extPalMask = -dispCnt->ExBGxPalette_Enable;
	for(x = 0; x < LG; xfin = std::min<u16>(x+8, LG))
	{
		tmp = (xoff & (lg-1))>>3;
		mapinfo = map + (tmp & 31) * 2;
		if(tmp > 31) mapinfo += 32*32*2;
		tileentry.val = T1ReadWord(MMU_gpu_map(mapinfo), 0);
		tilePal = pal + ((tileentry.bits.Palette<<9)&extPalMask);
		line = (u8*)MMU_gpu_map(tile + (tileentry.bits.TileNum*0x40) + ((tileentry.bits.VFlip) ? (7*8)-yoff : yoff));

		if(tileentry.bits.HFlip)
		{
			line += (7 - (xoff&7));
			line_dir = -1;
		} else {
			line += (xoff&7);
			line_dir = 1;
		}
		for(; x < xfin; )
		{
			color = T1ReadWord(tilePal, (*line) << 1);

			gpu->__setFinalColorBck<MOSAIC,false>(color, x, (*line != 0));
			
			x++; xoff++;

			line += line_dir;
		}
	}
}

/*****************************************************************************/
//			BACKGROUND RENDERING -ROTOSCALE-
/*****************************************************************************/

template<bool MOSAIC> FORCEINLINE void rot_tiled_8bit_entry(GPU * gpu, s32 auxX, s32 auxY, int lg, u32 map, u32 tile, u8 * pal, int i) {
	u8 palette_entry;
	u16 tileindex, x, y, color;

	tileindex = *(u8*)MMU_gpu_map(map + ((auxX>>3) + (auxY>>3) * (lg>>3)));

	x = (auxX&7); 
	y = (auxY&7);

	palette_entry = *(u8*)MMU_gpu_map(tile + ((tileindex<<6)+(y<<3)+x));
	color = T1ReadWord(pal, palette_entry << 1);
	gpu->__setFinalColorBck<MOSAIC,false>(color, i, (palette_entry != 0));
}

template<bool MOSAIC, bool extPal> FORCEINLINE void rot_tiled_16bit_entry(GPU * gpu, s32 auxX, s32 auxY, int lg, u32 map, u32 tile, u8 * pal, int i) {
	void* const map_addr = MMU_gpu_map(map + (((auxX>>3) + (auxY>>3) * (lg>>3))<<1));
	
	TILEENTRY tileentry;
	tileentry.val = T1ReadWord(map_addr, 0);

	const u16 x = ((tileentry.bits.HFlip) ? 7 - (auxX) : (auxX))&7;
	const u16 y = ((tileentry.bits.VFlip) ? 7 - (auxY) : (auxY))&7;

	const u8 palette_entry = *(u8*)MMU_gpu_map(tile + ((tileentry.bits.TileNum<<6)+(y<<3)+x));
	const u16 color = T1ReadWord(pal, (palette_entry + (extPal ? (tileentry.bits.Palette<<8) : 0)) << 1);
	gpu->__setFinalColorBck<MOSAIC,false>(color, i, (palette_entry != 0));
}

template<bool MOSAIC> FORCEINLINE void rot_256_map(GPU * gpu, s32 auxX, s32 auxY, int lg, u32 map, u32 tile, u8 * pal, int i) {
	u8 palette_entry;
	u16 color;

	u8* adr = (u8*)MMU_gpu_map((map) + ((auxX + auxY * lg)));

	palette_entry = *adr;
	color = T1ReadWord(pal, palette_entry << 1);
	gpu->__setFinalColorBck<MOSAIC,false>(color, i, (palette_entry != 0));
}

template<bool MOSAIC> FORCEINLINE void rot_BMP_map(GPU * gpu, s32 auxX, s32 auxY, int lg, u32 map, u32 tile, u8 * pal, int i) {
	u16 color;
	void* adr = MMU_gpu_map((map) + ((auxX + auxY * lg) << 1));
	color = T1ReadWord(adr, 0);
	gpu->__setFinalColorBck<MOSAIC,false>(color, i, ((color&0x8000) != 0));
}

typedef void (*rot_fun)(GPU * gpu, s32 auxX, s32 auxY, int lg, u32 map, u32 tile, u8 * pal, int i);

template<rot_fun fun, bool WRAP>
FORCEINLINE void rot_scale_op(GPU * gpu, s32 X, s32 Y, s16 PA, s16 PB, s16 PC, s16 PD, u16 LG, s32 wh, s32 ht, u32 map, u32 tile, u8 * pal)
{
	ROTOCOORD x, y;
	x.val = X;
	y.val = Y;

	const s32 dx = (s32)PA;
	const s32 dy = (s32)PC;

	// as an optimization, specially handle the fairly common case of
	// "unrotated + unscaled + no boundary checking required"
	if(dx==0x100 && dy==0)
	{
		s32 auxX = x.bits.Integer;
		s32 auxY = y.bits.Integer;
		if(WRAP || (auxX + LG < wh && auxX >= 0 && auxY < ht && auxY >= 0))
		{
			if(WRAP)
			{
				auxY = auxY & (ht-1);
				auxX = auxX & (wh-1);
			}
			for(int i = 0; i < LG; ++i)
			{
				fun(gpu, auxX, auxY, wh, map, tile, pal, i);
				auxX++;
				if(WRAP)
					auxX = auxX & (wh-1);
			}
			return;
		}
	}
	
	for(int i = 0; i < LG; ++i)
	{
		s32 auxX, auxY;
		auxX = x.bits.Integer;
		auxY = y.bits.Integer;
	
		if(WRAP)
		{
			auxX = auxX & (wh-1);
			auxY = auxY & (ht-1);
		}
		
		if(WRAP || ((auxX >= 0) && (auxX < wh) && (auxY >= 0) && (auxY < ht)))
			fun(gpu, auxX, auxY, wh, map, tile, pal, i);

		x.val += dx;
		y.val += dy;
	}
}

template<rot_fun fun>
FORCEINLINE void apply_rot_fun(GPU * gpu, s32 X, s32 Y, s16 PA, s16 PB, s16 PC, s16 PD, u16 LG, u32 map, u32 tile, u8 * pal)
{
	struct _BGxCNT * bgCnt = &(gpu->dispx_st)->dispx_BGxCNT[gpu->currBgNum].bits;
	s32 wh = gpu->BGSize[gpu->currBgNum][0];
	s32 ht = gpu->BGSize[gpu->currBgNum][1];
	if(bgCnt->PaletteSet_Wrap)
		rot_scale_op<fun,true>(gpu, X, Y, PA, PB, PC, PD, LG, wh, ht, map, tile, pal);	
	else rot_scale_op<fun,false>(gpu, X, Y, PA, PB, PC, PD, LG, wh, ht, map, tile, pal);	
}


template<bool MOSAIC> FORCEINLINE void rotBG2(GPU * gpu, s32 X, s32 Y, s16 PA, s16 PB, s16 PC, s16 PD, u16 LG)
{
	u8 num = gpu->currBgNum;
	u8 * pal = MMU.ARM9_VMEM + gpu->core * 0x400;
//	printf("rot mode\n");
	apply_rot_fun<rot_tiled_8bit_entry<MOSAIC> >(gpu,X,Y,PA,PB,PC,PD,LG, gpu->BG_map_ram[num], gpu->BG_tile_ram[num], pal);
}

template<bool MOSAIC> FORCEINLINE void extRotBG2(GPU * gpu, s32 X, s32 Y, s16 PA, s16 PB, s16 PC, s16 PD, s16 LG)
{
	u8 num = gpu->currBgNum;
	struct _DISPCNT * dispCnt = &(gpu->dispx_st)->dispx_DISPCNT.bits;
	
	u8 *pal;

	switch(gpu->BGTypes[num])
	{
	case BGType_AffineExt_256x16:
		if(dispCnt->ExBGxPalette_Enable)
			pal = MMU.ExtPal[gpu->core][gpu->BGExtPalSlot[num]];
		else
			pal = MMU.ARM9_VMEM + gpu->core * 0x400;
		if (!pal) return;
		// 16  bit bgmap entries
		if(dispCnt->ExBGxPalette_Enable)
			apply_rot_fun<rot_tiled_16bit_entry<MOSAIC, true> >(gpu,X,Y,PA,PB,PC,PD,LG, gpu->BG_map_ram[num], gpu->BG_tile_ram[num], pal);
		else apply_rot_fun<rot_tiled_16bit_entry<MOSAIC, false> >(gpu,X,Y,PA,PB,PC,PD,LG, gpu->BG_map_ram[num], gpu->BG_tile_ram[num], pal);
		return;
	case BGType_AffineExt_256x1:
		// 256 colors 
		pal = MMU.ARM9_VMEM + gpu->core * 0x400;
		apply_rot_fun<rot_256_map<MOSAIC> >(gpu,X,Y,PA,PB,PC,PD,LG, gpu->BG_bmp_ram[num], 0, pal);
		return;
	case BGType_AffineExt_Direct:
		// direct colors / BMP
		apply_rot_fun<rot_BMP_map<MOSAIC> >(gpu,X,Y,PA,PB,PC,PD,LG, gpu->BG_bmp_ram[num], 0, NULL);
		return;
	case BGType_Large8bpp:
		// large screen 256 colors
		pal = MMU.ARM9_VMEM + gpu->core * 0x400;
		apply_rot_fun<rot_256_map<MOSAIC> >(gpu,X,Y,PA,PB,PC,PD,LG, gpu->BG_bmp_large_ram[num], 0, pal);
		return;
	default: break;
	}
}

/*****************************************************************************/
//			BACKGROUND RENDERING -HELPER FUNCTIONS-
/*****************************************************************************/

#if 0
static void lineNull(GPU *gpu)
{
}
#endif

template<bool MOSAIC> void lineText(GPU *gpu)
{
	if(gpu->debug)
	{
		const s32 wh = gpu->BGSize[gpu->currBgNum][0];
		renderline_textBG<MOSAIC>(gpu, 0, gpu->currLine, wh);
	}
	else
	{
		const u16 vofs = gpu->getVOFS(gpu->currBgNum);
		const u16 hofs = gpu->getHOFS(gpu->currBgNum);
		renderline_textBG<MOSAIC>(gpu, hofs, gpu->currLine + vofs, 256);
	}
}

template<bool MOSAIC> void lineRot(GPU *gpu)
{	
	BGxPARMS *parms;
	if (gpu->currBgNum == 2)
	{
		parms = &(gpu->dispx_st)->dispx_BG2PARMS;
	}
	else
	{
		parms = &(gpu->dispx_st)->dispx_BG3PARMS;		
	}

	if(gpu->debug)
	{
		s32 wh = gpu->BGSize[gpu->currBgNum][0];
		rotBG2<MOSAIC>(gpu, 0, (s16)gpu->currLine*256, 256,0, 0,-77, wh);
	}
	else
	{
		 rotBG2<MOSAIC>(gpu, 
				  parms->BGxX,
				  parms->BGxY,
				  parms->BGxPA,
				  parms->BGxPB,
				  parms->BGxPC,
				  parms->BGxPD,
				  256);
		 parms->BGxX += parms->BGxPB;
		 parms->BGxY += parms->BGxPD;
	}
}

template<bool MOSAIC> void lineExtRot(GPU *gpu)
{
	BGxPARMS *parms;
	if (gpu->currBgNum == 2)
	{
		parms = &(gpu->dispx_st)->dispx_BG2PARMS;
	}
	else
	{
		parms = &(gpu->dispx_st)->dispx_BG3PARMS;		
	}

	if (gpu->debug)
	{
		s32 wh = gpu->BGSize[gpu->currBgNum][0];
		extRotBG2<MOSAIC>(gpu, 0, (s16)gpu->currLine*256, 256,0, 0,-77, wh);
	}
	else
	{
		extRotBG2<MOSAIC>(gpu,
              parms->BGxX,
              parms->BGxY,
              parms->BGxPA,
              parms->BGxPB,
              parms->BGxPC,
              parms->BGxPD,
              256);
		parms->BGxX += parms->BGxPB;
		parms->BGxY += parms->BGxPD;
	}
}

/*****************************************************************************/
//			SPRITE RENDERING -HELPER FUNCTIONS-
/*****************************************************************************/

/* if i understand it correct, and it fixes some sprite problems in chameleon shot */
/* we have a 15 bit color, and should use the pal entry bits as alpha ?*/
/* http://nocash.emubase.de/gbatek.htm#dsvideoobjs */
INLINE void render_sprite_BMP(GPU *gpu, const u8 spriteNum, const u16 l, u16 *dst, const u32 srcadr, u8 *dst_alpha, u8 *typeTab, u8 *prioTab, const u8 prio, const size_t lg, size_t sprX, size_t x, const size_t xdir, const u8 alpha)
{
	for (size_t i = 0; i < lg; i++, ++sprX, x += xdir)
	{
		const u16 *src = (u16 *)MMU_gpu_map(srcadr + (x << 1));
		const u16 color = LE_TO_LOCAL_16(*src);

		//a cleared alpha bit suppresses the pixel from processing entirely; it doesnt exist
		if ((color & 0x8000) && (prio < prioTab[sprX]))
		{
			dst[sprX] = color;
			dst_alpha[sprX] = alpha+1;
			typeTab[sprX] = 3;
			prioTab[sprX] = prio;
			gpu->sprNum[sprX] = spriteNum;
		}
	}
}

INLINE void render_sprite_256(GPU *gpu, const u8 spriteNum, const u16 l, u16 *dst, const u32 srcadr, const u16 *pal, u8 *dst_alpha, u8 *typeTab, u8 *prioTab, const u8 prio, const size_t lg, size_t sprX, size_t x, const size_t xdir, const u8 alpha)
{
	for (size_t i = 0; i < lg; i++, ++sprX, x += xdir)
	{
		const u32 adr = srcadr + (x & 0x7) + ((x & 0xFFF8) << 3);
		const u8 *src = (u8 *)MMU_gpu_map(adr);
		const u8 palette_entry = *src;

		//a zero value suppresses the pixel from processing entirely; it doesnt exist
		if ((palette_entry > 0) && (prio < prioTab[sprX]))
		{
			const u16 color = LE_TO_LOCAL_16(pal[palette_entry]);
			dst[sprX] = color;
			dst_alpha[sprX] = -1;
			typeTab[sprX] = (alpha ? 1 : 0);
			prioTab[sprX] = prio;
			gpu->sprNum[sprX] = spriteNum;
		}
	}
}

INLINE void render_sprite_16(GPU *gpu, const u16 l, u16 *dst, const u32 srcadr, const u16 *pal, u8 *dst_alpha, u8 *typeTab, u8 *prioTab, const u8 prio, const size_t lg, size_t sprX, size_t x, const size_t xdir, const u8 alpha)
{
	for (size_t i = 0; i < lg; i++, ++sprX, x += xdir)
	{
		const u16 x1 = x >> 1;
		const u32 adr = srcadr + (x1 & 0x3) + ((x1 & 0xFFFC) << 3);
		const u8 *src = (u8 *)MMU_gpu_map(adr);//
		const u8 palette = *src;
		const u8 palette_entry = (x & 1) ? palette >> 4 : palette & 0xF;
		
		//a zero value suppresses the pixel from processing entirely; it doesnt exist
		if ((palette_entry > 0) && (prio < prioTab[sprX]))
		{
			const u16 color = LE_TO_LOCAL_16(pal[palette_entry]);
			dst[sprX] = color;
			dst_alpha[sprX] = -1;
			typeTab[sprX] = (alpha ? 1 : 0);
			prioTab[sprX] = prio;
		}
	}
}

INLINE void render_sprite_Win (GPU * gpu, u16 l, u8 * src,
	int col256, int lg, int sprX, int x, int xdir) {
	int i; u8 palette, palette_entry;
	u16 x1;
	if (col256) {
		for(i = 0; i < lg; i++, sprX++,x+=xdir)
			//sprWin[sprX] = (src[x])?1:0;
			if(src[(x&7) + ((x&0xFFF8)<<3)]) 
				sprWin[sprX] = 1;
	} else {
		for(i = 0; i < lg; i++, ++sprX, x+=xdir)
		{
			x1 = x>>1;
			palette = src[(x1&0x3) + ((x1&0xFFFC)<<3)];
			if (x & 1) palette_entry = palette >> 4;
			else       palette_entry = palette & 0xF;
			//sprWin[sprX] = (palette_entry)?1:0;
			if(palette_entry)
				sprWin[sprX] = 1;
		}
	}
}

// return val means if the sprite is to be drawn or not
FORCEINLINE BOOL compute_sprite_vars(_OAM_ * spriteInfo, u16 l, 
	size &sprSize, s32 &sprX, s32 &sprY, s32 &x, s32 &y, s32 &lg, int &xdir) {

	x = 0;
	// get sprite location and size
	sprX = (spriteInfo->X/*<<23*/)/*>>23*/;
	sprY = spriteInfo->Y;
	sprSize = sprSizeTab[spriteInfo->Size][spriteInfo->Shape];

	lg = sprSize.x;
	
	if (sprY>=192)
		sprY = (s32)((s8)(spriteInfo->Y));
	
// FIXME: for rot/scale, a list of entries into the sprite should be maintained,
// that tells us where the first pixel of a screenline starts in the sprite,
// and how a step to the right in a screenline translates within the sprite

	//this wasn't really tested by anything. very unlikely to get triggered
	y = (l - sprY)&255;                        /* get the y line within sprite coords */
	if(y >= sprSize.y)
		return FALSE;

	if((sprX==256)||(sprX+sprSize.x<=0))	/* sprite pixels outside of line */
		return FALSE;				/* not to be drawn */

	// sprite portion out of the screen (LEFT)
	if(sprX<0)
	{
		lg += sprX;	
		x = -(sprX);
		sprX = 0;
	}
	// sprite portion out of the screen (RIGHT)
	if (sprX+sprSize.x >= 256)
		lg = 256 - sprX;

	// switch TOP<-->BOTTOM
	if (spriteInfo->VFlip)
		y = sprSize.y - y -1;
	
	// switch LEFT<-->RIGHT
	if (spriteInfo->HFlip) {
		x = sprSize.x - x -1;
		xdir  = -1;
	} else {
		xdir  = 1;
	}
	return TRUE;
}

/*****************************************************************************/
//			SPRITE RENDERING
/*****************************************************************************/


//TODO - refactor this so there isnt as much duped code between rotozoomed and non-rotozoomed versions

static u32 bmp_sprite_address(GPU* gpu, _OAM_ * spriteInfo, size sprSize, s32 y)
{
	if (gpu->dispCnt().OBJ_BMP_mapping)
	{
		//tested by buffy sacrifice damage blood splatters in corner
		return gpu->sprMem + (spriteInfo->TileIndex<<gpu->sprBMPBoundary) + (y*sprSize.x*2);
	}
	else
	{
		//2d mapping:
		//verified in rotozoomed mode by knights in the nightmare intro

		if (gpu->dispCnt().OBJ_BMP_2D_dim)
			//256*256, verified by heroes of mana FMV intro
			return gpu->sprMem + (((spriteInfo->TileIndex&0x3E0) * 64  + (spriteInfo->TileIndex&0x1F) *8 + ( y << 8)) << 1);
		else 
			//128*512, verified by harry potter and the order of the phoenix conversation portraits
			return gpu->sprMem + (((spriteInfo->TileIndex&0x3F0) * 64  + (spriteInfo->TileIndex&0x0F) *8 + ( y << 7)) << 1);
	}
}


template<GPU::SpriteRenderMode MODE>
void GPU::_spriteRender(u16 *dst, u8 *dst_alpha, u8 *typeTab, u8 *prioTab)
{
	u16 l = currLine;
	GPU *gpu = this;

	int cost = 0;

	struct _DISPCNT * dispCnt = &(gpu->dispx_st)->dispx_DISPCNT.bits;
	u8 block = gpu->sprBoundary;

	for (size_t i = 0; i < 128; i++)
	{
		_OAM_ theOAM;
		_OAM_*spriteInfo = &theOAM;
		SlurpOAM(spriteInfo, gpu->oam, i);

		//for each sprite:
		if (cost >= 2130)
		{
			//out of sprite rendering time
			//printf("sprite overflow!\n");
			//return;		
		}

		//do we incur a cost if a sprite is disabled?? we guess so.
		cost += 2;

		size sprSize;
		s32 sprX, sprY, x, y, lg;
		int xdir;
		u8 prio, *src;
		u32 srcadr;
		u16 j;

		// Check if sprite is disabled before everything
		if (spriteInfo->RotScale == 2)
			continue;

		prio = spriteInfo->Priority;


		if (spriteInfo->RotScale & 1) 
		{
			s32		fieldX, fieldY, auxX, auxY, realX, realY, offset;
			u8		blockparameter, *pal;
			s16		dx, dmx, dy, dmy;
			u16		colour;

			// Get sprite positions and size
			sprX = (spriteInfo->X<<23)>>23;
			sprY = spriteInfo->Y;
			sprSize = sprSizeTab[spriteInfo->Size][spriteInfo->Shape];

			lg = sprSize.x;
			
			if (sprY>=192)
				sprY = (s32)((s8)(spriteInfo->Y));

			// Copy sprite size, to check change it if needed
			fieldX = sprSize.x;
			fieldY = sprSize.y;

			// If we are using double size mode, double our control vars
			if (spriteInfo->RotScale & 2)
			{
				fieldX <<= 1;
				fieldY <<= 1;
				lg <<= 1;
			}

			//check if the sprite is visible y-wise. unfortunately our logic for x and y is different due to our scanline based rendering
			//tested thoroughly by many large sprites in Super Robot Wars K which wrap around the screen
			y = (l - sprY) & 255;
			if (y >= fieldY)
				continue;

			//check if sprite is visible x-wise.
			if ((sprX == 256) || (sprX + fieldX <= 0))
				continue;

			cost += sprSize.x*2 + 10;

			// Get which four parameter block is assigned to this sprite
			blockparameter = (spriteInfo->RotScalIndex + (spriteInfo->HFlip<< 3) + (spriteInfo->VFlip << 4))*4;

			// Get rotation/scale parameters
			dx = SlurpOAMAffineParam(gpu->oam,blockparameter+0);
			dmx = SlurpOAMAffineParam(gpu->oam,blockparameter+1);
			dy = SlurpOAMAffineParam(gpu->oam,blockparameter+2);
			dmy = SlurpOAMAffineParam(gpu->oam,blockparameter+3);


			// Calculate fixed poitn 8.8 start offsets
			realX = ((sprSize.x) << 7) - (fieldX >> 1)*dx - (fieldY>>1)*dmx + y * dmx;
			realY = ((sprSize.y) << 7) - (fieldX >> 1)*dy - (fieldY>>1)*dmy + y * dmy;

			if (sprX < 0)
			{
				// If sprite is not in the window
				if (sprX + fieldX <= 0)
					continue;

				// Otherwise, is partially visible
				lg += sprX;
				realX -= sprX*dx;
				realY -= sprX*dy;
				sprX = 0;
			}
			else
			{
				if (sprX + fieldX > 256)
					lg = 256 - sprX;
			}

			// If we are using 1 palette of 256 colours
			if (spriteInfo->Depth)
			{
				src = (u8 *)MMU_gpu_map(gpu->sprMem + (spriteInfo->TileIndex << block));

				// If extended palettes are set, use them
				if (dispCnt->ExOBJPalette_Enable)
					pal = (MMU.ObjExtPal[gpu->core][0]+(spriteInfo->PaletteIndex*0x200));
				else
					pal = (MMU.ARM9_VMEM + 0x200 + gpu->core *0x400);

				for (j = 0; j < lg; ++j, ++sprX)
				{
					// Get the integer part of the fixed point 8.8, and check if it lies inside the sprite data
					auxX = (realX >> 8);
					auxY = (realY >> 8);

					if (auxX >= 0 && auxY >= 0 && auxX < sprSize.x && auxY < sprSize.y)
					{
						if(MODE == SPRITE_2D)
							offset = (auxX&0x7) + ((auxX&0xFFF8)<<3) + ((auxY>>3)<<10) + ((auxY&0x7)*8);
						else
							offset = (auxX&0x7) + ((auxX&0xFFF8)<<3) + ((auxY>>3)*sprSize.x*8) + ((auxY&0x7)*8);

						colour = src[offset];

						if (colour && (prio<prioTab[sprX]))
						{ 
							dst[sprX] = HostReadWord(pal, (colour<<1));
							dst_alpha[sprX] = -1;
							typeTab[sprX] = spriteInfo->Mode;
							prioTab[sprX] = prio;
						}
					}

					//  Add the rotation/scale coeficients, here the rotation/scaling
					// is performed
					realX += dx;
					realY += dy;
				}

				continue;
			}
			// Rotozoomed direct color
			else if (spriteInfo->Mode == 3)
			{
				//transparent (i think, dont bother to render?) if alpha is 0
				if (spriteInfo->PaletteIndex == 0)
					continue;

				srcadr = bmp_sprite_address(this,spriteInfo,sprSize,0);

				for (j = 0; j < lg; ++j, ++sprX)
				{
					// Get the integer part of the fixed point 8.8, and check if it lies inside the sprite data
					auxX = (realX>>8);
					auxY = (realY>>8);

					//this is all very slow, and so much dup code with other rotozoomed modes.
					//dont bother fixing speed until this whole thing gets reworked

					if (auxX >= 0 && auxY >= 0 && auxX < sprSize.x && auxY < sprSize.y)
					{
						if(dispCnt->OBJ_BMP_2D_dim)
							//tested by knights in the nightmare
							offset = (bmp_sprite_address(this,spriteInfo,sprSize,auxY)-srcadr)/2+auxX;
						else //tested by lego indiana jones (somehow?)
							//tested by buffy sacrifice damage blood splatters in corner
							offset = auxX + (auxY*sprSize.x);


						u16* mem = (u16*)MMU_gpu_map(srcadr + (offset<<1));
						colour = LE_TO_LOCAL_16(*mem);
						
						if((colour&0x8000) && (prio<prioTab[sprX]))
						{
							dst[sprX] = colour;
							dst_alpha[sprX] = spriteInfo->PaletteIndex;
							typeTab[sprX] = spriteInfo->Mode;
							prioTab[sprX] = prio;
						}
					}

					//  Add the rotation/scale coeficients, here the rotation/scaling
					// is performed
					realX += dx;
					realY += dy;
				}

				continue;
			}
			// Rotozoomed 16/16 palette
			else
			{
				if (MODE == SPRITE_2D)
				{
					src = (u8 *)MMU_gpu_map(gpu->sprMem + (spriteInfo->TileIndex<<5));
					pal = MMU.ARM9_VMEM + 0x200 + (gpu->core*0x400 + (spriteInfo->PaletteIndex*32));
				}
				else
				{
					src = (u8 *)MMU_gpu_map(gpu->sprMem + (spriteInfo->TileIndex<<gpu->sprBoundary));
					pal = MMU.ARM9_VMEM + 0x200 + gpu->core*0x400 + (spriteInfo->PaletteIndex*32);
				}

				for (j = 0; j < lg; ++j, ++sprX)
				{
					// Get the integer part of the fixed point 8.8, and check if it lies inside the sprite data
					auxX = (realX>>8);
					auxY = (realY>>8);

					if (auxX >= 0 && auxY >= 0 && auxX < sprSize.x && auxY < sprSize.y)
					{
						if(MODE == SPRITE_2D)
							offset = ((auxX>>1)&0x3) + (((auxX>>1)&0xFFFC)<<3) + ((auxY>>3)<<10) + ((auxY&0x7)*4);
						else
							offset = ((auxX>>1)&0x3) + (((auxX>>1)&0xFFFC)<<3) + ((auxY>>3)*sprSize.x)*4 + ((auxY&0x7)*4);
						
						colour = src[offset];

						// Get 4bits value from the readed 8bits
						if (auxX&1)	colour >>= 4;
						else		colour &= 0xF;

						if (colour && (prio<prioTab[sprX]))
						{
							if (spriteInfo->Mode==2)
								sprWin[sprX] = 1;
							else
							{
								dst[sprX] = LE_TO_LOCAL_16(HostReadWord(pal, colour << 1));
								dst_alpha[sprX] = -1;
								typeTab[sprX] = spriteInfo->Mode;
								prioTab[sprX] = prio;
							}
						}
					}

					//  Add the rotation/scale coeficients, here the rotation/scaling
					// is performed
					realX += dx;
					realY += dy;
				}

				continue;
			}
		}
		else //NOT rotozoomed
		{
			u16 *pal;
	
			if (!compute_sprite_vars(spriteInfo, l, sprSize, sprX, sprY, x, y, lg, xdir))
				continue;

			cost += sprSize.x;

			if (spriteInfo->Mode == 2)
			{
				if(MODE == SPRITE_2D)
				{
					if (spriteInfo->Depth)
						src = (u8 *)MMU_gpu_map(gpu->sprMem + ((spriteInfo->TileIndex)<<5) + ((y>>3)<<10) + ((y&0x7)*8));
					else
						src = (u8 *)MMU_gpu_map(gpu->sprMem + ((spriteInfo->TileIndex)<<5) + ((y>>3)<<10) + ((y&0x7)*4));
				}
				else
				{
					if (spriteInfo->Depth)
						src = (u8 *)MMU_gpu_map(gpu->sprMem + (spriteInfo->TileIndex<<block) + ((y>>3)*sprSize.x*8) + ((y&0x7)*8));
					else
						src = (u8 *)MMU_gpu_map(gpu->sprMem + (spriteInfo->TileIndex<<block) + ((y>>3)*sprSize.x*4) + ((y&0x7)*4));
				}

				render_sprite_Win(gpu, l, src, spriteInfo->Depth, lg, sprX, x, xdir);
				continue;
			}

			if (spriteInfo->Mode == 3)              //sprite is in BMP format
			{
				srcadr = bmp_sprite_address(this, spriteInfo, sprSize, y);

				//transparent (i think, dont bother to render?) if alpha is 0
				if(spriteInfo->PaletteIndex == 0)
					continue;
				
				render_sprite_BMP(gpu, i, l, dst, srcadr, dst_alpha, typeTab, prioTab, prio, lg, sprX, x, xdir, spriteInfo->PaletteIndex);
				continue;
			}
				
			if(spriteInfo->Depth) //256 colors
			{
				if(MODE == SPRITE_2D)
					srcadr = gpu->sprMem + ((spriteInfo->TileIndex)<<5) + ((y>>3)<<10) + ((y&0x7)*8);
				else
					srcadr = gpu->sprMem + (spriteInfo->TileIndex<<block) + ((y>>3)*sprSize.x*8) + ((y&0x7)*8);
				
				if (dispCnt->ExOBJPalette_Enable)
					pal = (u16*)(MMU.ObjExtPal[gpu->core][0]+(spriteInfo->PaletteIndex*0x200));
				else
					pal = (u16*)(MMU.ARM9_VMEM + 0x200 + gpu->core *0x400);
		
				render_sprite_256(gpu, i, l, dst, srcadr, pal, dst_alpha, typeTab, prioTab, prio, lg, sprX, x, xdir, spriteInfo->Mode == 1);

				continue;
			}

			// 16 colors 
			if (MODE == SPRITE_2D)
			{
				srcadr = gpu->sprMem + ((spriteInfo->TileIndex)<<5) + ((y>>3)<<10) + ((y&0x7)*4);
			}
			else
			{
				srcadr = gpu->sprMem + (spriteInfo->TileIndex<<block) + ((y>>3)*sprSize.x*4) + ((y&0x7)*4);
			}
				
			pal = (u16*)(MMU.ARM9_VMEM + 0x200 + gpu->core * 0x400);
			
			pal += (spriteInfo->PaletteIndex<<4);
			
			render_sprite_16(gpu, l, dst, srcadr, pal, dst_alpha, typeTab, prioTab, prio, lg, sprX, x, xdir, spriteInfo->Mode == 1);
		}
	}

}


/*****************************************************************************/
//			SCREEN FUNCTIONS
/*****************************************************************************/

int Screen_Init()
{
	MainScreen.gpu = GPU_Init(GPUCOREID_MAIN);
	SubScreen.gpu = GPU_Init(GPUCOREID_SUB);
	
	disp_fifo.head = disp_fifo.tail = 0;
	
	OSDCLASS *previousOSD = osd;
	osd = new OSDCLASS(-1);
	delete previousOSD;
	
	gfx3d_init();
	GPU_SetFramebufferSize(256, 192);
	
	return 0;
}

void Screen_Reset(void)
{
	GPU_Reset(MainScreen.gpu);
	GPU_Reset(SubScreen.gpu);
	MainScreen.offset = 0;
	SubScreen.offset = _gpuFramebufferHeight;

	for (size_t i = 0; i < (_gpuFramebufferWidth * _gpuFramebufferHeight) * 2; i++)
	{
		GPU_screen[i] = 0x7FFF;
	}

	disp_fifo.head = disp_fifo.tail = 0;
	osd->clear();
}

void Screen_DeInit(void)
{
	gfx3d_deinit();
	
	GPU_DeInit(MainScreen.gpu);
	MainScreen.gpu = NULL;
	GPU_DeInit(SubScreen.gpu);
	SubScreen.gpu = NULL;

	delete osd;
	osd = NULL;
	
	free(GPU_screen);
	GPU_screen = NULL;
	
	free(win_empty);
	win_empty = NULL;
}

size_t GPU_GetFramebufferWidth()
{
	return _gpuFramebufferWidth;
}

size_t GPU_GetFramebufferHeight()
{
	return _gpuFramebufferHeight;
}

void GPU_SetFramebufferSize(size_t w, size_t h)
{
	if (w < 256 || h < 192)
	{
		return;
	}
	
	const float yScale = (float)h / 192.0f;
	size_t lineCount = (size_t)ceilf(yScale);
	
	_gpuFramebufferWidth = w;
	_gpuFramebufferHeight = h;
	GPU_screen = (u16 *)realloc(GPU_screen, w * h * sizeof(u16) * 2);
	MainScreen.offset = 0;
	SubScreen.offset = _gpuFramebufferHeight;
	
	MainScreen.gpu->tempScanlineBuffer = (u16 *)realloc(MainScreen.gpu->tempScanlineBuffer, w * lineCount * sizeof(u16));
	SubScreen.gpu->tempScanlineBuffer = (u16 *)realloc(SubScreen.gpu->tempScanlineBuffer, w * lineCount * sizeof(u16));
	MainScreen.gpu->bgPixels = (u8 *)realloc(MainScreen.gpu->bgPixels, w * lineCount * 4 * sizeof(u8)); // yes indeed, this is oversized. map debug tools try to write to it
	SubScreen.gpu->bgPixels = (u8 *)realloc(SubScreen.gpu->bgPixels, w * lineCount * 4 * sizeof(u8)); // yes indeed, this is oversized. map debug tools try to write to it
	
	const size_t windowBufferSize = w * sizeof(u8);
	const u8 *oldWinEmptyPtr = win_empty;
	win_empty = (u8 *)realloc(win_empty, windowBufferSize);
	memset(win_empty, 0, windowBufferSize);
	
	MainScreen.gpu->h_win[0] = (u8 *)realloc(MainScreen.gpu->h_win[0], windowBufferSize);
	MainScreen.gpu->h_win[1] = (u8 *)realloc(MainScreen.gpu->h_win[1], windowBufferSize);
	SubScreen.gpu->h_win[0] = (u8 *)realloc(SubScreen.gpu->h_win[0], windowBufferSize);
	SubScreen.gpu->h_win[1] = (u8 *)realloc(SubScreen.gpu->h_win[1], windowBufferSize);
	
	MainScreen.gpu->curr_win[0] = (MainScreen.gpu->curr_win[0] == NULL || MainScreen.gpu->curr_win[0] == oldWinEmptyPtr) ? win_empty : MainScreen.gpu->h_win[0];
	MainScreen.gpu->curr_win[1] = (MainScreen.gpu->curr_win[1] == NULL || MainScreen.gpu->curr_win[1] == oldWinEmptyPtr) ? win_empty : MainScreen.gpu->h_win[1];
	SubScreen.gpu->curr_win[0] = (SubScreen.gpu->curr_win[0] == NULL || SubScreen.gpu->curr_win[0] == oldWinEmptyPtr) ? win_empty : SubScreen.gpu->h_win[0];
	SubScreen.gpu->curr_win[1] = (SubScreen.gpu->curr_win[1] == NULL || SubScreen.gpu->curr_win[1] == oldWinEmptyPtr) ? win_empty : SubScreen.gpu->h_win[1];
	
	for (size_t i = 0; i < (w * h) * 2; i++)
	{
		GPU_screen[i] = 0x7FFF;
	}
	
	gfx3d_setFramebufferSize(_gpuFramebufferWidth, _gpuFramebufferHeight);
}

/*****************************************************************************/
//			GPU_RenderLine
/*****************************************************************************/

void GPU_set_DISPCAPCNT(u32 val)
{
	GPU * gpu = MainScreen.gpu;	
	struct _DISPCNT * dispCnt = &(gpu->dispx_st)->dispx_DISPCNT.bits;

	gpu->dispCapCnt.val = val;
	gpu->dispCapCnt.EVA = std::min((u32)16, (val & 0x1F));
	gpu->dispCapCnt.EVB = std::min((u32)16, ((val >> 8) & 0x1F));
	gpu->dispCapCnt.writeBlock =  (val >> 16) & 0x03;
	gpu->dispCapCnt.writeOffset = (val >> 18) & 0x03;
	gpu->dispCapCnt.readBlock = dispCnt->VRAM_Block;

	if (dispCnt->DisplayMode == 2)
		gpu->dispCapCnt.readOffset = 0;
	else
		gpu->dispCapCnt.readOffset = (val >> 26) & 0x03;
	
	gpu->dispCapCnt.srcA = (val >> 24) & 0x01;
	gpu->dispCapCnt.srcB = (val >> 25) & 0x01;
	gpu->dispCapCnt.capSrc = (val >> 29) & 0x03;

	switch((val >> 20) & 0x03)
	{
		case 0:
			gpu->dispCapCnt.capx = DISPCAPCNT::_128;
			gpu->dispCapCnt.capy = 128;
			break;
		case 1:
			gpu->dispCapCnt.capx = DISPCAPCNT::_256;
			gpu->dispCapCnt.capy = 64;
			break;
		case 2:
			gpu->dispCapCnt.capx = DISPCAPCNT::_256;
			gpu->dispCapCnt.capy = 128;
			break;
		case 3:
			gpu->dispCapCnt.capx = DISPCAPCNT::_256;
			gpu->dispCapCnt.capy = 192;
			break;
	}

	/*INFO("Capture 0x%X:\n EVA=%i, EVB=%i, wBlock=%i, wOffset=%i, capX=%i, capY=%i\n rBlock=%i, rOffset=%i, srcCap=%i, dst=0x%X, src=0x%X\n srcA=%i, srcB=%i\n\n",
			val, gpu->dispCapCnt.EVA, gpu->dispCapCnt.EVB, gpu->dispCapCnt.writeBlock, gpu->dispCapCnt.writeOffset,
			gpu->dispCapCnt.capx, gpu->dispCapCnt.capy, gpu->dispCapCnt.readBlock, gpu->dispCapCnt.readOffset, 
			gpu->dispCapCnt.capSrc, gpu->dispCapCnt.dst - MMU.ARM9_LCD, gpu->dispCapCnt.src - MMU.ARM9_LCD,
			gpu->dispCapCnt.srcA, gpu->dispCapCnt.srcB);*/
}

static void GPU_RenderLine_layer(GPU *gpu, const u16 l)
{
	CACHE_ALIGN u16 spr[256];
	CACHE_ALIGN u8 sprAlpha[256];
	CACHE_ALIGN u8 sprType[256];
	CACHE_ALIGN u8 sprPrio[256];
	
	const float xScale = _gpuFramebufferWidth / 256.0f;
	const float yScale = _gpuFramebufferHeight / 192.0f;
	const size_t srcLine = (size_t)((float)l * yScale);
	const size_t lineCount = yScale;

	u16 *dstLine = gpu->currDst;
	struct _DISPCNT * dispCnt = &(gpu->dispx_st)->dispx_DISPCNT.bits;
	itemsForPriority_t * item;
	u16 i16;
	bool BG_enabled = true;

	gpu->currentFadeInColors = &fadeInColors[gpu->BLDY_EVY][0];
	gpu->currentFadeOutColors = &fadeOutColors[gpu->BLDY_EVY][0];

	const u16 backdrop_color = T1ReadWord(MMU.ARM9_VMEM, gpu->core * 0x400) & 0x7FFF;

	//we need to write backdrop colors in the same way as we do BG pixels in order to do correct window processing
	//this is currently eating up 2fps or so. it is a reasonable candidate for optimization. 
	gpu->currBgNum = 5;
	switch (gpu->setFinalColorBck_funcNum)
	{
		//for backdrops, blend isnt applied (it's illogical, isnt it?)
		case 0:
		case 1:
PLAIN_CLEAR:
			memset_u16_le(dstLine, _gpuFramebufferWidth * lineCount, backdrop_color);
			break;

		//for backdrops, fade in and fade out can be applied if it's a 1st target screen
		case 2:
			if(gpu->BLDCNT & 0x20) //backdrop is selected for color effect
				memset_u16_le(dstLine, _gpuFramebufferWidth * lineCount, gpu->currentFadeInColors[backdrop_color]);
			else goto PLAIN_CLEAR;
			break;
		case 3:
			if(gpu->BLDCNT & 0x20) //backdrop is selected for color effect
				memset_u16_le(dstLine, _gpuFramebufferWidth * lineCount, gpu->currentFadeOutColors[backdrop_color]);
			else goto PLAIN_CLEAR;
			break;

		//windowed cases apparently need special treatment? why? can we not render the backdrop? how would that even work?
		case 4: for(size_t x=0;x<_gpuFramebufferWidth*lineCount;x++) gpu->___setFinalColorBck<false,true,4>(backdrop_color,x,true); break;
		case 5: for(size_t x=0;x<_gpuFramebufferWidth*lineCount;x++) gpu->___setFinalColorBck<false,true,5>(backdrop_color,x,true); break;
		case 6: for(size_t x=0;x<_gpuFramebufferWidth*lineCount;x++) gpu->___setFinalColorBck<false,true,6>(backdrop_color,x,true); break;
		case 7: for(size_t x=0;x<_gpuFramebufferWidth*lineCount;x++) gpu->___setFinalColorBck<false,true,7>(backdrop_color,x,true); break;
	}
	
	memset(gpu->bgPixels, 5, _gpuFramebufferWidth * lineCount);

	// init background color & priorities
	memset(sprAlpha, 0, 256);
	memset(sprType, 0, 256);
	memset(sprPrio, 0xFF, 256);
	memset(sprWin, 0, 256);
	
	// init pixels priorities
	assert(NB_PRIORITIES == 4);
	gpu->itemsForPriority[0].nbPixelsX = 0;
	gpu->itemsForPriority[1].nbPixelsX = 0;
	gpu->itemsForPriority[2].nbPixelsX = 0;
	gpu->itemsForPriority[3].nbPixelsX = 0;

	// for all the pixels in the line
	if (gpu->LayersEnable[4]) 
	{
		//n.b. - this is clearing the sprite line buffer to the background color,
		//but it has been changed to write u32 instead of u16 for a little speedup
		for (size_t i = 0; i < 128; ++i) HostWriteTwoWords((u8 *)spr, i << 2, backdrop_color | (backdrop_color<<16));
		//zero 06-may-09: I properly supported window color effects for backdrop, but I am not sure
		//how it interacts with this. I wish we knew why we needed this
		
		gpu->spriteRender(spr, sprAlpha, sprType, sprPrio);
		mosaicSpriteLine(gpu, l, spr, sprAlpha, sprType, sprPrio);

		for (size_t i = 0; i < 256; i++)
		{
			// assign them to the good priority item
			int prio = sprPrio[i];
			if (prio >= 4) continue;
			
			item = &(gpu->itemsForPriority[prio]);
			item->PixelsX[item->nbPixelsX]=i;
			item->nbPixelsX++;
		}
	}

	
	if (!gpu->LayersEnable[0] && !gpu->LayersEnable[1] && !gpu->LayersEnable[2] && !gpu->LayersEnable[3])
		BG_enabled = false;

	for (size_t j = 0; j < 8; j++)
		gpu->blend2[j] = (gpu->BLDCNT & (0x100 << j)) != 0;

	// paint lower priorities first
	// then higher priorities on top
	for (size_t prio = NB_PRIORITIES; prio > 0; )
	{
		prio--;
		item = &(gpu->itemsForPriority[prio]);
		// render BGs
		if (BG_enabled)
		{
			for (size_t i = 0; i < item->nbBGs; i++)
			{
				i16 = item->BGs[i];
				if (gpu->LayersEnable[i16])
				{
					gpu->currBgNum = (u8)i16;
					gpu->blend1 = (gpu->BLDCNT & (1 << gpu->currBgNum))!=0;

					struct _BGxCNT *bgCnt = &(gpu->dispx_st)->dispx_BGxCNT[i16].bits;
					gpu->curr_mosaic_enabled = bgCnt->Mosaic_Enable;

					if (gpu->core == GPUCOREID_MAIN)
					{
						if (i16 == 0 && dispCnt->BG0_3D)
						{
							gpu->currBgNum = 0;
							
							const u16 hofs = (u16)((float)gpu->getHOFS(i16) * xScale);
							u16 *oldDstLine = dstLine;
							
							for (size_t lineIndex = 0; lineIndex < lineCount; lineIndex++, dstLine += _gpuFramebufferWidth)
							{
								const FragmentColor *colorLine = gfx3d_GetLineDataRGBA6665(srcLine + lineIndex);
								
								for (size_t k = 0; k < _gpuFramebufferWidth; k++)
								{
									size_t q = k + hofs;
									if (q >= _gpuFramebufferWidth * 2)
									{
										q -= _gpuFramebufferWidth * 2;
									}
									
									if (q >= _gpuFramebufferWidth || colorLine[q].a == 0)
										continue;
									
									gpu->setFinalColor3d(k, dstLine[k], colorLine[q]);
								}
							}
							
							dstLine = oldDstLine;
							
							continue;
						}
					}

					//useful for debugging individual layers
					//if(gpu->core == 1 || i16 != 2) continue;




#ifndef DISABLE_MOSAIC
					if(gpu->curr_mosaic_enabled)
						gpu->modeRender<true>(i16);
					else 
#endif
						gpu->modeRender<false>(i16);
				} //layer enabled
			}
		}

		// render sprite Pixels
		if (gpu->LayersEnable[4])
		{
			gpu->currBgNum = 4;
			gpu->blend1 = (gpu->BLDCNT & (1 << gpu->currBgNum))!=0;
			
			for (size_t i = 0; i < item->nbPixelsX; i++)
			{
				i16 = item->PixelsX[i];
				setFinalColorSpr(gpu, gpu->currDst, spr[i16], sprAlpha[i16], sprType[i16], i16);
			}
		}
	}
}

template<bool SKIP> static void GPU_RenderLine_DispCapture(u16 l)
{
	//this macro takes advantage of the fact that there are only two possible values for capx
	#define CAPCOPY(SRC,DST,SETALPHABIT) \
	switch(gpu->dispCapCnt.capx) { \
		case DISPCAPCNT::_128: \
			for (int i = 0; i < 128; i++)  \
				HostWriteWord(DST, i << 1, HostReadWord(SRC, i << 1) | (SETALPHABIT ? 0x8000 : 0x0000)); \
			break; \
		case DISPCAPCNT::_256: \
			for (int i = 0; i < 256; i++)  \
				HostWriteWord(DST, i << 1, HostReadWord(SRC, i << 1) | (SETALPHABIT ? 0x8000 : 0x0000)); \
			break; \
			default: assert(false); \
		}
	
	GPU *gpu = MainScreen.gpu;

	if (l == 0)
	{
		if (gpu->dispCapCnt.val & 0x80000000)
		{
			gpu->dispCapCnt.enabled = TRUE;
			T1WriteLong(MMU.ARM9_REG, 0x64, gpu->dispCapCnt.val);
		}
	}

	bool skip = SKIP;

	if (gpu->dispCapCnt.enabled)
	{
		//128-wide captures should write linearly into memory, with no gaps
		//this is tested by hotel dusk
		u32 ofsmul = gpu->dispCapCnt.capx==DISPCAPCNT::_128?256:512;
		u32 cap_src_adr = gpu->dispCapCnt.readOffset * 0x8000 + (l * 512);
		u32 cap_dst_adr = gpu->dispCapCnt.writeOffset * 0x8000 + (l * ofsmul);

		//Read/Write block wrap to 00000h when exceeding 1FFFFh (128k)
		//this has not been tested yet (I thought I needed it for hotel dusk, but it was fixed by the above)
		cap_src_adr &= 0x1FFFF;
		cap_dst_adr &= 0x1FFFF;

		cap_src_adr += gpu->dispCapCnt.readBlock * 0x20000;
		cap_dst_adr += gpu->dispCapCnt.writeBlock * 0x20000;

		u8* cap_src = MMU.ARM9_LCD + cap_src_adr;
		u8* cap_dst = MMU.ARM9_LCD + cap_dst_adr;

		//we must block captures when the capture dest is not mapped to LCDC
		if(vramConfiguration.banks[gpu->dispCapCnt.writeBlock].purpose != VramConfiguration::LCDC)
			skip = true;

		//we must return zero from reads from memory not mapped to lcdc
		if(vramConfiguration.banks[gpu->dispCapCnt.readBlock].purpose != VramConfiguration::LCDC)
			cap_src = MMU.blank_memory;

		if(!skip)
		if (l < gpu->dispCapCnt.capy)
		{
			switch (gpu->dispCapCnt.capSrc)
			{
				case 0:		// Capture source is SourceA
					{
						//INFO("Capture source is SourceA\n");
						switch (gpu->dispCapCnt.srcA)
						{
							case 0:			// Capture screen (BG + OBJ + 3D)
								{
									//INFO("Capture screen (BG + OBJ + 3D)\n");
									u16 *src = gpu->tempScanline;
#ifdef LOCAL_BE
									static u16 swapSrc[256];
									const size_t swapSrcSize = (gpu->dispCapCnt.capx == DISPCAPCNT::_128) ? 128 : 256;
									
									for(size_t i = 0; i < swapSrcSize; i++)
									{
										swapSrc[i] = LE_TO_LOCAL_16(src[i]);
									}
									
									CAPCOPY((u8 *)swapSrc, cap_dst, true);
#else
									CAPCOPY((u8 *)src, cap_dst, true);
#endif
								}
							break;
							case 1:			// Capture 3D
								{
									//INFO("Capture 3D\n");
									const u16 *colorLine = gfx3d_GetLineDataRGBA5551(l);
									CAPCOPY(((u8*)colorLine),cap_dst,false);
								}
							break;
						}
					}
				break;
				case 1:		// Capture source is SourceB
					{
						//INFO("Capture source is SourceB\n");
						switch (gpu->dispCapCnt.srcB)
						{
							case 0:	
								//Capture VRAM
								CAPCOPY(cap_src,cap_dst,true);
								break;
							case 1:
								//capture dispfifo
								//(not yet tested)
								for(int i=0; i < 128; i++)
									T1WriteLong(cap_dst, i << 2, DISP_FIFOrecv());
								break;
						}
					}
				break;
					
				default:	// Capture source is SourceA+B blended
					{
						//INFO("Capture source is SourceA+B blended\n");
						const u16 *srcA = NULL;
						const u16 *srcB = NULL;

						if (gpu->dispCapCnt.srcA == 0)
						{
							// Capture screen (BG + OBJ + 3D)
							srcA = gpu->tempScanline;
						}
						else
						{
							srcA = gfx3d_GetLineDataRGBA5551(l);
						}

						static u16 fifoLine[256];

						if (gpu->dispCapCnt.srcB == 0)			// VRAM screen
							srcB = (u16 *)cap_src;
						else
						{
							//fifo - tested by splinter cell chaos theory thermal view
							srcB = fifoLine;
							for (int i=0; i < 128; i++)
								T1WriteLong((u8*)srcB, i << 2, DISP_FIFOrecv());
						}


						const size_t todo = (gpu->dispCapCnt.capx==DISPCAPCNT::_128 ? 128 : 256);

						for (size_t i = 0; i < todo; i++)
						{
							u16 a,r,g,b;
							u16 a_alpha = srcA[i] & 0x8000;
							u16 b_alpha = srcB[i] & 0x8000;
							
							if (a_alpha)
							{
								a = 0x8000;
								r =  ((srcA[i]        & 0x1F) * gpu->dispCapCnt.EVA);
								g = (((srcA[i] >>  5) & 0x1F) * gpu->dispCapCnt.EVA);
								b = (((srcA[i] >> 10) & 0x1F) * gpu->dispCapCnt.EVA);
							} 
							else
							{
								a = r = g = b = 0;
							}
							
							if (b_alpha)
							{
								a = 0x8000;
								r +=  ((srcB[i]        & 0x1F) * gpu->dispCapCnt.EVB);
								g += (((srcB[i] >>  5) & 0x1F) * gpu->dispCapCnt.EVB);
								b += (((srcB[i] >> 10) & 0x1F) * gpu->dispCapCnt.EVB);
							}
							
							r >>= 4;
							g >>= 4;
							b >>= 4;
							
							//freedom wings sky will overflow while doing some fsaa/motionblur effect without this
							r = std::min((u16)31,r);
							g = std::min((u16)31,g);
							b = std::min((u16)31,b);
							
							HostWriteWord(cap_dst, i << 1, a | (b << 10) | (g << 5) | r);
						}
					}
				break;
			}
		}

		if (l >= 191)
		{
			gpu->dispCapCnt.enabled = FALSE;
			gpu->dispCapCnt.val &= 0x7FFFFFFF;
			T1WriteLong(MMU.ARM9_REG, 0x64, gpu->dispCapCnt.val);
			return;
		}
	}
}

static INLINE void GPU_RenderLine_MasterBrightness(const GPUMasterBrightMode mode, const u32 factor, u16 *dst, const size_t dstLineCount)
{
	//isn't it odd that we can set uselessly high factors here?
	//factors above 16 change nothing. curious.
	if (factor == 0) return;
	
	//Apply final brightness adjust (MASTER_BRIGHT)
	//http://nocash.emubase.de/gbatek.htm#dsvideo (Under MASTER_BRIGHTNESS)
	
	switch (mode)
	{
		case GPUMasterBrightMode_Disable:
			break;
			
		case GPUMasterBrightMode_Up:
		{
			if (factor < 16)
			{
				for (size_t i = 0; i < _gpuFramebufferWidth * dstLineCount; ++i)
				{
					dst[i] = fadeInColors[factor][dst[i]&0x7FFF];
				}
			}
			else
			{
				// all white (optimization)
				for (size_t i = 0; i < _gpuFramebufferWidth * dstLineCount; ++i)
				{
					dst[i] = 0x7FFF;
				}
			}
			break;
		}
			
		case GPUMasterBrightMode_Down:
		{
			if (factor < 16)
			{
				for (size_t i = 0; i < _gpuFramebufferWidth * dstLineCount; ++i)
				{
					dst[i] = fadeOutColors[factor][dst[i]&0x7FFF];
				}
			}
			else
			{
				// all black (optimization)
				memset(dst, 0, _gpuFramebufferWidth * dstLineCount * sizeof(u16));
			}
			break;
		}
			
		case GPUMasterBrightMode_Reserved:
			break;
	}
	
}

template<int WIN_NUM>
FORCEINLINE void GPU::setup_windows()
{
	u8 y = currLine;
	u16 startY,endY;

	if(WIN_NUM==0)
	{
		startY = WIN0V0;
		endY = WIN0V1;
	}
	else
	{
		startY = WIN1V0;
		endY = WIN1V1;
	}

	if(WIN_NUM == 0 && !WIN0_ENABLED) goto allout;
	if(WIN_NUM == 1 && !WIN1_ENABLED) goto allout;

	if(startY > endY)
	{
		if((y < startY) && (y > endY)) goto allout;
	}
	else
	{
		if((y < startY) || (y >= endY)) goto allout;
	}

	//the x windows will apply for this scanline
	curr_win[WIN_NUM] = h_win[WIN_NUM];
	return;
	
allout:
	curr_win[WIN_NUM] = win_empty;
}

void GPU::update_winh(int WIN_NUM)
{
	//dont even waste any time in here if the window isnt enabled
	if(WIN_NUM==0 && !WIN0_ENABLED) return;
	if(WIN_NUM==1 && !WIN1_ENABLED) return;

	need_update_winh[WIN_NUM] = false;
	u16 startX,endX;

	if(WIN_NUM==0)
	{
		startX = WIN0H0;
		endX = WIN0H1;
	}
	else
	{
		startX = WIN1H0;
		endX = WIN1H1;
	}

	//the original logic: if you doubt the window code, please check it against the newer implementation below
	//if(startX > endX)
	//{
	//	if((x < startX) && (x > endX)) return false;
	//}
	//else
	//{
	//	if((x < startX) || (x >= endX)) return false;
	//}

	if(startX > endX)
	{
		for(int i=0;i<=endX;i++)
			h_win[WIN_NUM][i] = 1;
		for(int i=endX+1;i<startX;i++)
			h_win[WIN_NUM][i] = 0;
		for(int i=startX;i<256;i++)
			h_win[WIN_NUM][i] = 1;
	} else
	{
		for(int i=0;i<startX;i++)
			h_win[WIN_NUM][i] = 0;
		for(int i=startX;i<endX;i++)
			h_win[WIN_NUM][i] = 1;
		for(int i=endX;i<256;i++)
			h_win[WIN_NUM][i] = 0;
	}
}

void GPU_RenderLine(NDS_Screen *screen, const u16 l, bool skip)
{
	const float yScale = _gpuFramebufferHeight / 192.0f;
	const size_t dstLineCount = yScale;
	const size_t srcLine = (size_t)((float)l * yScale);
	
	GPU *gpu = screen->gpu;
	u16 *dstLine = GPU_screen + ((screen->offset + srcLine) * _gpuFramebufferWidth);

	//here is some setup which is only done on line 0
	if (l == 0)
	{
		//this is speculative. the idea is as follows:
		//whenever the user updates the affine start position regs, it goes into the active regs immediately
		//(this is handled on the set event from MMU)
		//maybe it shouldnt take effect until the next hblank or something..
		//this is a based on a combination of:
		//heroes of mana intro FMV
		//SPP level 3-8 rotoscale room
		//NSMB raster fx backdrops
		//bubble bobble revolution classic mode
		//NOTE:
		//I am REALLY unsatisfied with this logic now. But it seems to be working..
		gpu->refreshAffineStartRegs(-1,-1);
	}

	if (skip)
	{
		gpu->currLine = l;
		if (gpu->core == GPUCOREID_MAIN)
		{
			GPU_RenderLine_DispCapture<true>(l);
			if (l == 191) { disp_fifo.head = disp_fifo.tail = 0; }
		}
		return;
	}

	//blacken the screen if it is turned off by the user
	if (!CommonSettings.showGpu.screens[gpu->core])
	{
		memset(dstLine, 0, _gpuFramebufferWidth * dstLineCount * sizeof(u16));
		return;
	}

	// skip some work if master brightness makes the screen completely white or completely black
	if (gpu->MasterBrightFactor >= 16 && (gpu->MasterBrightMode == GPUMasterBrightMode_Up || gpu->MasterBrightMode == GPUMasterBrightMode_Down))
	{
		// except if it could cause any side effects (for example if we're capturing), then don't skip anything
		if (!(gpu->core == GPUCOREID_MAIN && (gpu->dispCapCnt.enabled || l == 0 || l == 191)))
		{
			gpu->currLine = l;
			GPU_RenderLine_MasterBrightness(gpu->MasterBrightMode, gpu->MasterBrightFactor, dstLine, dstLineCount);
			return;
		}
	}

	//cache some parameters which are assumed to be stable throughout the rendering of the entire line
	gpu->currLine = l;
	u16 mosaic_control = LE_TO_LOCAL_16(gpu->dispx_st->dispx_MISC.MOSAIC);
	u16 mosaic_width = (mosaic_control & 0xF);
	u16 mosaic_height = ((mosaic_control>>4) & 0xF);

	//mosaic test hacks
	//mosaic_width = mosaic_height = 3;

	GPU::mosaicLookup.widthValue = mosaic_width;
	GPU::mosaicLookup.heightValue = mosaic_height;
	GPU::mosaicLookup.width = &GPU::mosaicLookup.table[mosaic_width][0];
	GPU::mosaicLookup.height = &GPU::mosaicLookup.table[mosaic_height][0];

	if(gpu->need_update_winh[0]) gpu->update_winh(0);
	if(gpu->need_update_winh[1]) gpu->update_winh(1);

	gpu->setup_windows<0>();
	gpu->setup_windows<1>();

	//generate the 2d engine output
	if (gpu->dispMode == GPUDisplayMode_Normal)
	{
		//optimization: render straight to the output buffer when thats what we are going to end up displaying anyway
		gpu->tempScanline = gpu->currDst = dstLine;
	}
	else
	{
		//otherwise, we need to go to a temp buffer
		gpu->tempScanline = gpu->currDst = gpu->tempScanlineBuffer;
	}

	GPU_RenderLine_layer(gpu, l);

	switch (gpu->dispMode)
	{
		case GPUDisplayMode_Off: // Display Off(Display white)
			{
				for (size_t i = 0; i < _gpuFramebufferWidth * dstLineCount; i++)
				{
					dstLine[i] = 0x7FFF;
				}
			}
			break;
			
		case GPUDisplayMode_Normal: // Display BG and OBJ layers
			//do nothing: it has already been generated into the right place
			break;
			
		case GPUDisplayMode_VRAM: // Display vram framebuffer
			{
				u16 *src = (u16 *)gpu->VRAMaddr + (l * 256);
#ifdef LOCAL_BE
				for (size_t i = 0; i < 256; i++)
				{
					dstLine[i] = LE_TO_LOCAL_16(src[i]);
				}
#else
				memcpy(dstLine, src, 256 * sizeof(u16));
#endif
			}
			break;
			
		case GPUDisplayMode_MainMemory: // Display memory FIFO
			{
				//this has not been tested since the dma timing for dispfifo was changed around the time of
				//newemuloop. it may not work.
				for (size_t i = 0; i < _gpuFramebufferWidth/2; i++)
				{
					((u32 *)dstLine)[i] = DISP_FIFOrecv() & 0x7FFF7FFF;
				}
			}
			break;
	}
	
	//capture after displaying so that we can safely display vram before overwriting it here
	if (gpu->core == GPUCOREID_MAIN)
	{
		//BUG!!! if someone is capturing and displaying both from the fifo, then it will have been 
		//consumed above by the display before we get here
		//(is that even legal? i think so)
		GPU_RenderLine_DispCapture<false>(l);
		if (l == 191) { disp_fifo.head = disp_fifo.tail = 0; }
	}
	
	GPU_RenderLine_MasterBrightness(gpu->MasterBrightMode, gpu->MasterBrightFactor, dstLine, dstLineCount);
}

void gpu_savestate(EMUFILE* os)
{
	//version
	write32le(1,os);
	
	os->fwrite((u8 *)GPU_screen, 256 * 192 * sizeof(u16) * 2);
	
	write32le(MainScreen.gpu->affineInfo[0].x,os);
	write32le(MainScreen.gpu->affineInfo[0].y,os);
	write32le(MainScreen.gpu->affineInfo[1].x,os);
	write32le(MainScreen.gpu->affineInfo[1].y,os);
	write32le(SubScreen.gpu->affineInfo[0].x,os);
	write32le(SubScreen.gpu->affineInfo[0].y,os);
	write32le(SubScreen.gpu->affineInfo[1].x,os);
	write32le(SubScreen.gpu->affineInfo[1].y,os);
}

bool gpu_loadstate(EMUFILE* is, int size)
{
	//read version
	u32 version;

	//sigh.. shouldve used a new version number
	if(size == 256*192*2*2)
		version = 0;
	else if(size== 0x30024)
	{
		read32le(&version,is);
		version = 1;
	}
	else
		if(read32le(&version,is) != 1) return false;
		

	if(version > 1) return false;

	is->fread((u8 *)GPU_screen, 256 * 192 * sizeof(u16) * 2);

	if(version == 1)
	{
		read32le(&MainScreen.gpu->affineInfo[0].x,is);
		read32le(&MainScreen.gpu->affineInfo[0].y,is);
		read32le(&MainScreen.gpu->affineInfo[1].x,is);
		read32le(&MainScreen.gpu->affineInfo[1].y,is);
		read32le(&SubScreen.gpu->affineInfo[0].x,is);
		read32le(&SubScreen.gpu->affineInfo[0].y,is);
		read32le(&SubScreen.gpu->affineInfo[1].x,is);
		read32le(&SubScreen.gpu->affineInfo[1].y,is);
		//removed per nitsuja feedback. anyway, this same thing will happen almost immediately in gpu line=0
		//MainScreen.gpu->refreshAffineStartRegs(-1,-1);
		//SubScreen.gpu->refreshAffineStartRegs(-1,-1);
	}

	MainScreen.gpu->updateBLDALPHA();
	SubScreen.gpu->updateBLDALPHA();
	return !is->fail();
}

u32 GPU::getAffineStart(int layer, int xy)
{
	if(xy==0) return affineInfo[layer-2].x;
	else return affineInfo[layer-2].y;
}

void GPU::setAffineStartWord(int layer, int xy, u16 val, int word)
{
	u32 curr = getAffineStart(layer,xy);
	if(word==0) curr = (curr&0xFFFF0000)|val;
	else curr = (curr&0x0000FFFF)|(((u32)val)<<16);
	setAffineStart(layer,xy,curr);
}

void GPU::setAffineStart(int layer, int xy, u32 val)
{
	if(xy==0) affineInfo[layer-2].x = val;
	else affineInfo[layer-2].y = val;
	refreshAffineStartRegs(layer,xy);
}

void GPU::refreshAffineStartRegs(const int num, const int xy)
{
	if(num==-1)
	{
		refreshAffineStartRegs(2,xy);
		refreshAffineStartRegs(3,xy);
		return;
	}

	if(xy==-1)
	{
		refreshAffineStartRegs(num,0);
		refreshAffineStartRegs(num,1);
		return;
	}

	BGxPARMS * parms;
	if (num==2)
		parms = &(dispx_st)->dispx_BG2PARMS;
	else
		parms = &(dispx_st)->dispx_BG3PARMS;		

	if(xy==0)
		parms->BGxX = affineInfo[num-2].x;
	else
		parms->BGxY = affineInfo[num-2].y;
}

template<bool MOSAIC> void GPU::modeRender(int layer)
{
	switch(GPU_mode2type[dispCnt().BG_Mode][layer])
	{
		case BGType_Text: lineText<MOSAIC>(this); break;
		case BGType_Affine: lineRot<MOSAIC>(this); break;
		case BGType_AffineExt: lineExtRot<MOSAIC>(this); break;
		case BGType_Large8bpp: lineExtRot<MOSAIC>(this); break;
		case BGType_Invalid: 
			PROGINFO("Attempting to render an invalid BG type\n");
			break;
		default:
			break;
	}
}

u32 GPU::getHOFS(int bg)
{
	return LE_TO_LOCAL_16(dispx_st->dispx_BGxOFS[bg].BGxHOFS) & 0x01FF;
}

u32 GPU::getVOFS(int bg)
{
	return LE_TO_LOCAL_16(dispx_st->dispx_BGxOFS[bg].BGxVOFS) & 0x01FF;
}

void gpu_SetRotateScreen(u16 angle)
{
	gpu_angle = angle;
}
