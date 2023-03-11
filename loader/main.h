#ifndef __MAIN_H__
#define __MAIN_H__

#include <vitasdk.h>
#include "config.h"
#include "so_util.h"

extern so_module conduit_mod;

extern SceTouchPanelInfo panelInfoFront;

int debugPrintf(char *text, ...);

int ret0();

#endif
