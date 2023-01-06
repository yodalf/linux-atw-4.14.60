/* drivers/input/touchscreen/penmount-driver.c - PenMount TouchScreen Driver
 *
 * Copyright (C) 2013 PenMount Touch Solutions
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * From original Android driver Date: 2013/05/21 Version: 2.0
 */

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/version.h>
#include <linux/proc_fs.h>
#include <linux/input/mt.h>
#include <linux/usb.h>
#include "penmount.h"

#define PMMODULE_DESC                    "PenMount Touchscreen Driver"
#define PENMOUNT_MINOFFSET               30

int penmount_DbgLevel = 1;
module_param(penmount_DbgLevel, int, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH);

/* InputDev */
int pmDriver_InputDevOpen(struct input_dev *pInputDev)
{
	struct strPENMOUNT *pPenMount = NULL;

	pPenMount = (struct strPENMOUNT *) input_get_drvdata(pInputDev);
	if (pPenMount == NULL)
		return -ENXIO;

	if (pInputDev->id.bustype != BUS_USB)
		return 0;

	if (!pPenMount->OpenCount++)
	{
		pPenMount->USB.pUrb->dev = pPenMount->USB.pDevice;
		usb_submit_urb(pPenMount->USB.pUrb, GFP_KERNEL);
	}

	return 0;
}

void pmDriver_InputDevClose(struct input_dev *pInputDev)
{
	struct strPENMOUNT *pPenMount = NULL;

	pPenMount = (struct strPENMOUNT *) input_get_drvdata(pInputDev);

	if (pPenMount == NULL)
		return;

	if (pInputDev->id.bustype != BUS_USB)
		return;

	if (!--pPenMount->OpenCount)
		usb_kill_urb(pPenMount->USB.pUrb);

	return;
}

struct strPENMOUNT * pmDriver_InitContext(void)
{
	struct strPENMOUNT *pPenMount = NULL;
	int i = 0;

	pPenMount = kzalloc(sizeof(struct strPENMOUNT), GFP_KERNEL);
	if (pPenMount == NULL)
		return NULL;

	for (i = 0; i < PENMOUNT_MAXTOUCH; i++)
		pPenMount->Touch[i].Slot = i;

	return pPenMount;
}

void pmDriver_ResetTouchData(struct strPENMOUNT *pPenMount)
{
	int i = 0;
	for (i = 0; i < PENMOUNT_MAXTOUCH ; i++) {
		pPenMount->Touch[i].ID = -1;
		pPenMount->Touch[i].TrackID = -1;
		pPenMount->Touch[i].X = 0;
		pPenMount->Touch[i].Y = 0;
	}
	pPenMount->TouchCount = 0;
	return;
}

int pmDriver_InputDevInit(struct strPENMOUNT *pPenMount,
							struct device *pParentDev,
							struct input_id *pInputID)
{
	int rc = 0;
	struct input_dev *pInputDev = NULL;

	pInputDev = input_allocate_device();
	if (pInputDev == NULL)
		return -ENOMEM;

	memcpy(&pInputDev->id, pInputID, sizeof(struct input_id));
	
	pPenMount->Resolution = PENMOUNT_P2_RESOLUTION;
	switch (pPenMount->Model)
	{
	case PMDEVICE_MODEL_P2:
		pPenMount->bParallelReport = 0;
		pPenMount->MaxTouch = 2 ;
		pPenMount->MTProtocol = PENMOUNT_MTPROTO ;
		break;
	case PMDEVICE_MODEL_P3:
		pPenMount->bParallelReport = 1;
		if (pInputDev->id.bustype == BUS_USB)
			pPenMount->Resolution = PENMOUNT_P3_RESOLUTION_USB;
		pPenMount->MaxTouch = 16 ;
		pPenMount->MTProtocol = PENMOUNT_MTPROTO ;
		break;
	}

	pInputDev->evbit[0] = BIT_MASK(EV_KEY) | BIT_MASK(EV_ABS);
	pInputDev->keybit[BIT_WORD(BTN_TOUCH)] = BIT_MASK(BTN_TOUCH);
	input_set_abs_params ( pInputDev, ABS_X, 0, pPenMount->Resolution, 0, 0 );
	input_set_abs_params ( pInputDev, ABS_Y, 0, pPenMount->Resolution, 0, 0 );
	input_set_abs_params ( pInputDev, ABS_MT_POSITION_X, 0, pPenMount->Resolution, 0, 0 );
	input_set_abs_params ( pInputDev, ABS_MT_POSITION_Y, 0, pPenMount->Resolution, 0, 0 );

	pPenMount->MTProtocol = PENMOUNT_MTPROTO;
	switch ( pPenMount->MTProtocol )
	{
	case PENMOUNT_MTPROTO_A:
		printk ("[penmount] Using MT Protocol: A Type\n");
		input_set_abs_params ( pInputDev, ABS_MT_TOUCH_MAJOR, 0, 5, 0, 0 );
		if ( pPenMount->bParallelReport )
			input_set_abs_params ( pInputDev, ABS_MT_TRACKING_ID, 0, 15, 0, 0 );
		break;
	case PENMOUNT_MTPROTO_B:
		printk ("[penmount] Using MT Protocol: B Type\n");
		input_mt_init_slots   ( pInputDev, pPenMount->MaxTouch, INPUT_MT_DIRECT ) ;
		break;
	}

	printk("[penmount] Registering input device for %s\n", pPenMount->szDeviceName);

	pInputDev->name = pPenMount->szDeviceName;
	pInputDev->phys = pPenMount->szPhysDevice;
	pInputDev->open = pmDriver_InputDevOpen;
	pInputDev->close = pmDriver_InputDevClose;

	/* Set up "dev" field */
	pInputDev->dev.parent = pParentDev;

	input_set_drvdata(pInputDev, pPenMount);

	/* Register input device */
	rc = input_register_device(pInputDev);

	pPenMount->pInputDev = pInputDev;

	return rc;
}

void pmDriver_InputDevExit(struct strPENMOUNT *pPenMount)
{
	if (pPenMount == NULL)
		return;

	input_set_drvdata(pPenMount->pInputDev, NULL);
	input_unregister_device(pPenMount->pInputDev);

	return;
}

/* Event Processing */
void pmDriver_ProcessEvent(struct input_dev *pInputDev,
		struct strPENMOUNT *pPenMount, struct strPMTOUCH *pTouch)
{
	if (pTouch->bTouch)
	{
		if (!pTouch->bTouching)
		{
			if (pPenMount->MaxTouch == 1)
				input_report_key(pInputDev, BTN_TOUCH, 1);
			pTouch->bTouching = 1;
		}
	}
	else
	{
		if (pTouch->bTouching)
		{
			if (pPenMount->MaxTouch == 1)
				input_report_key(pInputDev, BTN_TOUCH, 0);
			pTouch->bTouching = 0;
		}
	}

	if (pPenMount->MaxTouch == 1)
	{
		input_report_abs(pInputDev, ABS_X, pTouch->X);
		input_report_abs(pInputDev, ABS_Y, pTouch->Y);
		input_sync(pInputDev);
	}

	pTouch->bTouch = 0;

	return;
}

int pmDriver_SkipMTEvent ( struct input_dev *pInputDev,
		struct strPENMOUNT *pPenMount )
{
	unsigned char i = 0;
	unsigned short MinOffset = PENMOUNT_MINOFFSET;

	if ( pPenMount->MTProtocol != PENMOUNT_MTPROTO_A )
		return 0;

	if ( pPenMount->MaxTouch == 1 )
		return 0;

	if ( pPenMount->Resolution != PENMOUNT_P2_RESOLUTION )
		MinOffset = MinOffset * pPenMount->Resolution / PENMOUNT_P2_RESOLUTION;

	for ( i = 0; i < pPenMount->MaxTouch; i++ )
	{
		if  ( abs ( pPenMount->Touch[i].X - pPenMount->Touch[i].LastX ) > MinOffset )
			return 0;

		if  ( abs ( pPenMount->Touch[i].Y - pPenMount->Touch[i].LastY ) > MinOffset )
			return 0;

		if  ( pPenMount->Touch[i].bTouching != pPenMount->Touch[i].LastState )
			return 0;
	}

	for ( i = 0 ; i < pPenMount->MaxTouch ; i++ )
		pPenMount->Touch[i].bUpdated = 0 ;

	return 1;
}

void pmDriver_ProcessMTEvent ( struct input_dev   *pInputDev ,
								struct strPENMOUNT *pPenMount )
{
	unsigned char i = 0 ;

	if ( pPenMount->MaxTouch > 1 )
	{
		unsigned char TouchCount = 0 ;

		switch ( pPenMount->MTProtocol )
		{
		case PENMOUNT_MTPROTO_A :
			for ( i = 0 ; i < pPenMount->MaxTouch ; i++ )
				if ( pPenMount->Touch[i].bTouching )
					TouchCount++ ;

			if ( !TouchCount )
				input_mt_sync ( pInputDev ) ;
			else
			{
				for ( i = 0 ; i < pPenMount->MaxTouch; i++ )
				{
					if ( pPenMount->bParallelReport )
					{
						if ( pPenMount->Touch[i].bTouching )
							pPenMount->Touch[i].TrackID = i;	

						if ( pPenMount->Touch[i].TrackID == -1 )
							continue;

						// printk ( "[%d] %d (%d,%d)\n", pPenMount->Touch[i].TrackID, pPenMount->Touch[i].bTouching, pPenMount->Touch[i].X, pPenMount->Touch[i].Y );

						input_report_abs ( pInputDev, ABS_MT_TRACKING_ID, pPenMount->Touch[i].TrackID ) ;
					}

					if ( ( pPenMount->pMainTouch == NULL ) && ( pPenMount->Touch[i].bTouching ) )
						pPenMount->pMainTouch = &pPenMount->Touch[i] ;

					if ( pPenMount->Touch[i].bTouching )
						input_report_abs ( pInputDev, ABS_MT_TOUCH_MAJOR , 2 ) ;
					else
						input_report_abs ( pInputDev, ABS_MT_TOUCH_MAJOR , 0 ) ;

					input_report_abs ( pInputDev, ABS_MT_POSITION_X, pPenMount->Touch[i].X ) ;
					input_report_abs ( pInputDev, ABS_MT_POSITION_Y, pPenMount->Touch[i].Y ) ;

					input_mt_sync ( pInputDev ) ;
					if ( pPenMount->bParallelReport )
					{
						if ( ! pPenMount->Touch[i].bTouching )
							pPenMount->Touch[i].TrackID = -1;
					}

				}
			}

			/* Single-Touch Emulation */
			if ( ! pPenMount->pMainTouch )
				break ;

			if ( pPenMount->pMainTouch->bTouching )
			{
				input_report_key ( pInputDev, BTN_TOUCH , 1 ) ;
				input_report_abs ( pInputDev, ABS_X  , pPenMount->pMainTouch->X ) ;
				input_report_abs ( pInputDev, ABS_Y  , pPenMount->pMainTouch->Y ) ;
			}
			else
			{
				input_report_key ( pInputDev, BTN_TOUCH , 0 ) ;
				pPenMount->pMainTouch = NULL ;
			}
			break ;
		case PENMOUNT_MTPROTO_B :
			for ( i = 0 ; i < pPenMount->MaxTouch ; i++ )
			{
				if ( ( pPenMount->pMainTouch == NULL ) && ( pPenMount->Touch[i].bTouching ) )
					pPenMount->pMainTouch = &pPenMount->Touch[i] ;

				if ( ( pPenMount->Touch[i].X == pPenMount->Touch[i].LastX )
				&& ( pPenMount->Touch[i].Y == pPenMount->Touch[i].LastY )
				&& ( pPenMount->Touch[i].bTouching == pPenMount->Touch[i].LastState ) )
					continue;

				input_mt_slot ( pInputDev, i ) ;
				input_mt_report_slot_state ( pInputDev, MT_TOOL_FINGER, pPenMount->Touch[i].bTouching ) ;
				if ( pPenMount->Touch[i].bTouching )
				{
					if ( pPenMount->Touch[i].X != pPenMount->Touch[i].LastX )
						input_report_abs ( pInputDev, ABS_MT_POSITION_X  , pPenMount->Touch[i].X ) ;
					if ( pPenMount->Touch[i].Y != pPenMount->Touch[i].LastY )
						input_report_abs ( pInputDev, ABS_MT_POSITION_Y  , pPenMount->Touch[i].Y ) ;
				}
			}
			/* Single-Touch Emulation */
			input_mt_report_pointer_emulation ( pInputDev, true ) ;
			break ;
		}

		input_sync ( pInputDev ) ;
	}

	for ( i = 0 ; i < pPenMount->MaxTouch ; i++ )
	{
		pPenMount->Touch[i].bUpdated = 0 ;
		pPenMount->Touch[i].LastState = pPenMount->Touch[i].bTouching ;
		pPenMount->Touch[i].LastX = pPenMount->Touch[i].X ;
		pPenMount->Touch[i].LastY = pPenMount->Touch[i].Y ;
	}
	return ;
}

/* Module */
static int __init pmModule_Init ( void )
{
	int rc = 0;

#if defined(CONFIG_TOUCHSCREEN_PENMOUNT_HID)
	rc |= pmHIDIF_Init();
#endif
	
#if defined(CONFIG_TOUCHSCREEN_PENMOUNT_I2C)
	rc |= pmI2CIF_Init();
#endif

	return rc;
}

static void __exit pmModule_Exit ( void )
{

#if defined(CONFIG_TOUCHSCREEN_PENMOUNT_I2C)
	pmI2CIF_Exit();
#endif

#if defined(CONFIG_TOUCHSCREEN_PENMOUNT_HID)
	pmHIDIF_Exit();
#endif
	return;
}

module_init (pmModule_Init);
module_exit (pmModule_Exit);

MODULE_AUTHOR("PenMount Touch Solutions <penmount@seed.net.tw>");
MODULE_DESCRIPTION(PMMODULE_DESC);
MODULE_LICENSE("GPL");

