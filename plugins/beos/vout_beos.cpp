/*****************************************************************************
 * vout_beos.cpp: beos video output display method
 *****************************************************************************
 * Copyright (C) 2000, 2001 VideoLAN
 *
 * Authors: Jean-Marc Dressler <polux@via.ecp.fr>
 *          Samuel Hocevar <sam@zoy.org>
 *          Richard Shepherd <richard@rshepherd.demon.co.uk>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111, USA.
 *****************************************************************************/

#define MODULE_NAME beos
#include "modules_inner.h"

/*****************************************************************************
 * Preamble
 *****************************************************************************/
#include "defs.h"

#include <errno.h>                                                 /* ENOMEM */
#include <stdlib.h>                                                /* free() */
#include <stdio.h>
#include <string.h>                                            /* strerror() */
#include <kernel/OS.h>
#include <View.h>
#include <Application.h>
#include <Window.h>
#include <Locker.h>
#include <Screen.h>
#include <malloc.h>
#include <string.h>

extern "C"
{
#include "config.h"
#include "common.h"
#include "threads.h"
#include "mtime.h"
#include "tests.h"
#include "modules.h"

#include "video.h"
#include "video_output.h"

#include "interface.h"
#include "intf_msg.h"

#include "main.h"
}

#include "VideoWindow.h"
#include <Screen.h>

#define WIDTH 128
#define HEIGHT 64
#define BITS_PER_PLANE 16
#define BYTES_PER_PIXEL 2

/*****************************************************************************
 * vout_sys_t: BeOS video output method descriptor
 *****************************************************************************
 * This structure is part of the video output thread descriptor.
 * It describes the BeOS specific properties of an output thread.
 *****************************************************************************/
 
typedef struct vout_sys_s
{
    VideoWindow *         p_window;

    byte_t *              pp_buffer[2];
    s32                   i_width;
    s32                   i_height;
} vout_sys_t;


/*****************************************************************************
 * beos_GetAppWindow : retrieve a BWindow pointer from the window name
 *****************************************************************************/

BWindow *beos_GetAppWindow(char *name)
{
    int32       index;
    BWindow     *window;
    
    for (index = 0 ; ; index++)
    {
        window = be_app->WindowAt(index);
        if (window == NULL)
            break;
        if (window->LockWithTimeout(200000) == B_OK)
        {
            if (strcmp(window->Name(), name) == 0)
            {
                window->Unlock();
                break;
            }
            window->Unlock();
        }
    }
    return window; 
}

/*****************************************************************************
 * DrawingThread : thread that really does the drawing
 *****************************************************************************/

int32 DrawingThread(void *data)
{
    VideoWindow *w;
    w = (VideoWindow*) data;
    
    while(!w->teardownwindow)
    {
    w->Lock();
       if( w->fDirty )
            {
              if(!w->fUsingOverlay)
              	w->view->DrawBitmap(w->bitmap[w->i_buffer_index], w->bitmap[w->i_buffer_index]->Bounds(), w->Bounds());
              w->fDirty = false;
            }
    w->Unlock();
   	snooze(20000);
    }
    return B_OK;
}

/*****************************************************************************
 * VideoWindow constructor and destructor
 *****************************************************************************/

VideoWindow::VideoWindow(BRect frame, const char *name, vout_thread_t *p_video_output )
        : BWindow(frame, name, B_TITLED_WINDOW, NULL)
{
	float minWidth, minHeight, maxWidth, maxHeight; 

    teardownwindow = false;
    is_zoomed = false;
    p_vout = p_video_output;

    rect = Frame();
    view = new VLCView(Bounds());
    AddChild(view);
	bitmap[0] = new BBitmap(Bounds(), B_BITMAP_WILL_OVERLAY|B_BITMAP_RESERVE_OVERLAY_CHANNEL, B_YCbCr422);
	fUsingOverlay = true;
	i_screen_depth = 32;
	p_vout->b_YCbr = true;
	
	if (bitmap[0]->InitCheck() != B_OK)
	{
		delete bitmap[0];
		p_vout->b_YCbr = false;
		fUsingOverlay = false;
		BScreen *screen;
		screen = new BScreen();
		color_space space = screen->ColorSpace();
		delete screen;

		if(space == B_RGB15)
			{
			bitmap[0] = new BBitmap(Bounds(), B_RGB15);
			bitmap[1] = new BBitmap(Bounds(), B_RGB15);
	    	i_screen_depth = 15;
		    }
		    else if(space == B_RGB16)
		 	{
			bitmap[0] = new BBitmap(Bounds(), B_RGB16);
			bitmap[1] = new BBitmap(Bounds(), B_RGB16);
	   		i_screen_depth = 16;
			}
			else //default to 32bpp
			{
			bitmap[0] = new BBitmap(Bounds(), B_RGB32);
			bitmap[1] = new BBitmap(Bounds(), B_RGB32);
	   		i_screen_depth = 32;
			}
		memset(bitmap[0]->Bits(), 0, bitmap[0]->BitsLength());
	 	memset(bitmap[1]->Bits(), 0, bitmap[1]->BitsLength());
	 	SetTitle("VideoLAN (BBitmap)");
	 }

	if(fUsingOverlay)
		{
		memset(bitmap[0]->Bits(), 0, bitmap[0]->BitsLength());
		rgb_color key;
		view->SetViewOverlay(bitmap[0], bitmap[0]->Bounds(), Bounds(), &key, B_FOLLOW_ALL,
				B_OVERLAY_FILTER_HORIZONTAL|B_OVERLAY_FILTER_VERTICAL);
		view->SetViewColor(key);
	 	SetTitle("VideoLAN (Overlay)");
		GetSizeLimits(&minWidth, &maxWidth, &minHeight, &maxHeight); 
		SetSizeLimits((float) Bounds().IntegerWidth(), maxWidth, (float) Bounds().IntegerHeight(), maxHeight);
		}

	i_bytes_per_pixel = bitmap[0]->BytesPerRow()/bitmap[0]->Bounds().IntegerWidth();
    fRowBytes = bitmap[0]->BytesPerRow();
    fDirty = false;
    fDrawThreadID = spawn_thread(DrawingThread, "drawing_thread",
                    B_DISPLAY_PRIORITY, (void*) this);
    resume_thread(fDrawThreadID);
    Show();
}

VideoWindow::~VideoWindow()
{
    int32 result;

    Hide();
    Sync();
    wait_for_thread(fDrawThreadID, &result);
    delete bitmap[0];
    if(!fUsingOverlay)
    	delete bitmap[1];
 }


/*****************************************************************************
 * VideoWindow::FrameResized
 *****************************************************************************/
void VideoWindow::FrameResized( float width, float height )
{
}

/*****************************************************************************
 * VideoWindow::Zoom
 *****************************************************************************/

void VideoWindow::Zoom(BPoint origin, float width, float height )
{
if(is_zoomed)
	{
	MoveTo(rect.left, rect.top);
	ResizeTo(rect.IntegerWidth(), rect.IntegerHeight());
	be_app->ShowCursor();
	}
else
	{
	rect = Frame();
	BScreen *screen;
	screen = new BScreen(this);
	BRect rect = screen->Frame();
	delete screen;
	MoveTo(0,0);
	ResizeTo(rect.IntegerWidth(), rect.IntegerHeight());
	be_app->HideCursor();
	}
is_zoomed = !is_zoomed;
}

/*****************************************************************************
 * VideoWindow::MessageReceived
 *****************************************************************************/

void VideoWindow::MessageReceived( BMessage * p_message )
{
    BWindow * p_win;
    
    switch( p_message->what )
    {
    case B_KEY_DOWN:
    case B_SIMPLE_DATA:
        // post the message to the interface window which will handle it
        p_win = beos_GetAppWindow( "interface" );
        if( p_win != NULL )
        {
            p_win->PostMessage( p_message );
        }
        break;
    
    default:
        BWindow::MessageReceived( p_message );
        break;
    }
}

/*****************************************************************************
 * VideoWindow::QuitRequested
 *****************************************************************************/

bool VideoWindow::QuitRequested()
{
    /* FIXME: send a message ! */
    p_main->p_intf->b_die = 1;
    teardownwindow = true;
    return( false );
}

/*****************************************************************************
 * VLCView::VLCView
 *****************************************************************************/
VLCView::VLCView(BRect bounds) : BView(bounds, "", B_FOLLOW_ALL, B_WILL_DRAW)
{
SetViewColor(B_TRANSPARENT_32_BIT);
}

/*****************************************************************************
 * VLCView::~VLCView
 *****************************************************************************/
VLCView::~VLCView()
{

}

/*****************************************************************************
 * VLCVIew::~VLCView
 *****************************************************************************/
void VLCView::MouseDown(BPoint point)
{
VideoWindow *w = (VideoWindow *) Window();
if(w->is_zoomed)
	{
	BWindow *win = Window();
	win->Zoom();
	}
}

extern "C"
{

/*****************************************************************************
 * Local prototypes
 *****************************************************************************/
static int  vout_Probe      ( probedata_t *p_data );
static int  vout_Create     ( struct vout_thread_s * );
static int  vout_Init       ( struct vout_thread_s * );
static void vout_End        ( struct vout_thread_s * );
static void vout_Destroy    ( struct vout_thread_s * );
static int  vout_Manage     ( struct vout_thread_s * );
static void vout_Display    ( struct vout_thread_s * );

static int  BeosOpenDisplay ( vout_thread_t *p_vout );
static void BeosCloseDisplay( vout_thread_t *p_vout );

/*****************************************************************************
 * Functions exported as capabilities. They are declared as static so that
 * we don't pollute the namespace too much.
 *****************************************************************************/
void _M( vout_getfunctions )( function_list_t * p_function_list )
{
    p_function_list->pf_probe = vout_Probe;
    p_function_list->functions.vout.pf_create     = vout_Create;
    p_function_list->functions.vout.pf_init       = vout_Init;
    p_function_list->functions.vout.pf_end        = vout_End;
    p_function_list->functions.vout.pf_destroy    = vout_Destroy;
    p_function_list->functions.vout.pf_manage     = vout_Manage;
    p_function_list->functions.vout.pf_display    = vout_Display;
    p_function_list->functions.vout.pf_setpalette = NULL;
}

/*****************************************************************************
 * vout_Probe: probe the video driver and return a score
 *****************************************************************************
 * This function tries to initialize SDL and returns a score to the
 * plugin manager so that it can select the best plugin.
 *****************************************************************************/
static int vout_Probe( probedata_t *p_data )
{
    if( TestMethod( VOUT_METHOD_VAR, "beos" ) )
    {
        return( 999 );
    }

    return( 100 );
}

/*****************************************************************************
 * vout_Create: allocates BeOS video thread output method
 *****************************************************************************
 * This function allocates and initializes a BeOS vout method.
 *****************************************************************************/
int vout_Create( vout_thread_t *p_vout )
{
    /* Allocate structure */
    p_vout->p_sys = (vout_sys_t*) malloc( sizeof( vout_sys_t ) );
    if( p_vout->p_sys == NULL )
    {
        intf_ErrMsg( "error: %s", strerror(ENOMEM) );
        return( 1 );
    }
    
    /* Set video window's size */
    p_vout->i_width =  main_GetIntVariable( VOUT_WIDTH_VAR,
                                            VOUT_WIDTH_DEFAULT );
    p_vout->i_height = main_GetIntVariable( VOUT_HEIGHT_VAR,
                                            VOUT_HEIGHT_DEFAULT );

    /* Open and initialize device */
    if( BeosOpenDisplay( p_vout ) )
    {
        intf_ErrMsg("vout error: can't open display");
        free( p_vout->p_sys );
        return( 1 );
    }

    return( 0 );
}

/*****************************************************************************
 * vout_Init: initialize BeOS video thread output method
 *****************************************************************************/
int vout_Init( vout_thread_t *p_vout )
{
    VideoWindow * p_win = p_vout->p_sys->p_window;
    u32 i_page_size;


    i_page_size =   p_vout->i_width * p_vout->i_height * p_vout->i_bytes_per_pixel;
    
    p_vout->p_sys->i_width =         p_vout->i_width;
    p_vout->p_sys->i_height =        p_vout->i_height;    

    if(p_win->fUsingOverlay)
    	{
	    vout_SetBuffers( p_vout, (byte_t *)p_win->bitmap[0]->Bits(),
    	                 (byte_t *)p_win->bitmap[0]->Bits());
    	}
    else
   		{
	    vout_SetBuffers( p_vout, (byte_t *)p_win->bitmap[0]->Bits(),
   	                 (byte_t *)p_win->bitmap[1]->Bits());
    	}
    return( 0 );
}

/*****************************************************************************
 * vout_End: terminate BeOS video thread output method
 *****************************************************************************/
void vout_End( vout_thread_t *p_vout )
{
}

/*****************************************************************************
 * vout_Destroy: destroy BeOS video thread output method
 *****************************************************************************
 * Terminate an output method created by DummyCreateOutputMethod
 *****************************************************************************/
void vout_Destroy( vout_thread_t *p_vout )
{
    BeosCloseDisplay( p_vout );
    
    free( p_vout->p_sys );
}

/*****************************************************************************
 * vout_Manage: handle BeOS events
 *****************************************************************************
 * This function should be called regularly by video output thread. It manages
 * console events. It returns a non null value on error.
 *****************************************************************************/
int vout_Manage( vout_thread_t *p_vout )
{
   return( 0 );
}

/*****************************************************************************
 * vout_Display: displays previously rendered output
 *****************************************************************************
 * This function send the currently rendered image to BeOS image, waits until
 * it is displayed and switch the two rendering buffers, preparing next frame.
 *****************************************************************************/
void vout_Display( vout_thread_t *p_vout )
{
    VideoWindow * p_win = p_vout->p_sys->p_window;
    
   	p_win->i_buffer_index = p_vout->i_buffer_index;
   	p_vout->i_buffer_index = ++p_vout->i_buffer_index & 1;
    p_win->fDirty = true;
}

/* following functions are local */

/*****************************************************************************
 * BeosOpenDisplay: open and initialize BeOS device
 *****************************************************************************
 * XXX?? The framebuffer mode is only provided as a fast and efficient way to
 * display video, providing the card is configured and the mode ok. It is
 * not portable, and is not supposed to work with many cards. Use at your
 * own risk !
 *****************************************************************************/

static int BeosOpenDisplay( vout_thread_t *p_vout )
{ 
    p_vout->p_sys->p_window =
        new VideoWindow(  BRect( 50, 150, 50+p_vout->i_width-1, 150+p_vout->i_height-1 ), NULL, p_vout );
    if( p_vout->p_sys->p_window == 0 )
    {
        free( p_vout->p_sys );
        intf_ErrMsg( "error: cannot allocate memory for VideoWindow" );
        return( 1 );
    }   
    VideoWindow * p_win = p_vout->p_sys->p_window;
    
    p_vout->i_screen_depth =         p_win->i_screen_depth;
    p_vout->i_bytes_per_pixel =      p_win->i_bytes_per_pixel;
    p_vout->i_bytes_per_line =       p_vout->i_width*p_win->i_bytes_per_pixel;
    
    switch( p_vout->i_screen_depth )
    {
    case 8:
        intf_ErrMsg( "vout error: 8 bit mode not fully supported" );
        break;
    case 15:
        p_vout->i_red_mask =        0x7c00;
        p_vout->i_green_mask =      0x03e0;
        p_vout->i_blue_mask =       0x001f;
        break;
    case 16:
        p_vout->i_red_mask =        0xf800;
        p_vout->i_green_mask =      0x07e0;
        p_vout->i_blue_mask =       0x001f;
        break;
    case 24:
    case 32:
    default:
        p_vout->i_red_mask =        0xff0000;
        p_vout->i_green_mask =      0x00ff00;
        p_vout->i_blue_mask =       0x0000ff;
        break;
    }
    return( 0 );
}

/*****************************************************************************
 * BeosDisplay: close and reset BeOS device
 *****************************************************************************
 * Returns all resources allocated by BeosOpenDisplay and restore the original
 * state of the device.
 *****************************************************************************/
static void BeosCloseDisplay( vout_thread_t *p_vout )
{    
    /* Destroy the video window */
    p_vout->p_sys->p_window->Lock();
    p_vout->p_sys->p_window->Quit();
}

} /* extern "C" */
