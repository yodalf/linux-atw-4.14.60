#include <linux/module.h>
#include <linux/slab.h>
#include <linux/version.h>
#include <linux/proc_fs.h>
#include <linux/usb.h>
#include <linux/cdev.h>
#include <linux/input/mt.h>
#ifdef CONFIG_HAS_EARLYSUSPEND
#include <linux/earlysuspend.h>
#endif

#define PENMOUNT_MAXTOUCH       16
#define PENMOUNT_MTPROTO_A      0
#define PENMOUNT_MTPROTO_B      1
#define PENMOUNT_MAXTRACKID     0xFFFF
#define PENMOUNT_MAXPACKET      64
#define PENMOUNT_P2_PACKETSIZE  6
#define PENMOUNT_P3_PACKETSIZE  64

#define PENMOUNT_MTPROTO        PENMOUNT_MTPROTO_B

#ifndef PMDEVICE_VENDORID_USB
#define PMDEVICE_VENDORID_USB   0x14E1
#endif

#ifndef PMDEVICE_VENDORID_I2C
#define PMDEVICE_VENDORID_I2C   0x14E1
#endif

#ifndef PMDEVICE_MODEL_P1
#define PMDEVICE_MODEL_P1       0x3000
#endif

#ifndef PMDEVICE_MODEL_P2
#define PMDEVICE_MODEL_P2       0x3500
#endif

#ifndef PMDEVICE_MODEL_P3
#define PMDEVICE_MODEL_P3       0x1600
#endif

#ifndef PMDEVICE_MODEL_P3
#define PMDEVICE_MODEL_P3       0x1600
#endif

#ifndef PMDEVICE_MODEL_6000
#define PMDEVICE_MODEL_6000     0x6000
#endif

// #define PMDEVICE_TARGET_PRODUCT  PMDEVICE_MODEL_P3
#ifdef PMDEVICE_TARGET_PRODUCT
#define PMDEVICE_PRODUCTID_I2C   PMDEVICE_TARGET_PRODUCT
#else
#define PMDEVICE_PRODUCTID_I2C   -1
#endif

#ifndef USB_VENDOR_ID_PENMOUNT
#define USB_VENDOR_ID_PENMOUNT          0x14E1
#endif

#ifndef USB_DEVICE_ID_PENMOUNT_P1
#define USB_DEVICE_ID_PENMOUNT_P1       PMDEVICE_MODEL_P1
#endif

#ifndef USB_DEVICE_ID_PENMOUNT_P2
#define USB_DEVICE_ID_PENMOUNT_P2       PMDEVICE_MODEL_P2
#endif

#ifndef USB_DEVICE_ID_PENMOUNT_6000
#define USB_DEVICE_ID_PENMOUNT_6000     PMDEVICE_MODEL_6000
#endif

#ifndef USB_DEVICE_ID_PENMOUNT_5000
#define USB_DEVICE_ID_PENMOUNT_5000     0x5000
#endif

#ifndef USB_DEVICE_ID_PENMOUNT_PM1450
#define USB_DEVICE_ID_PENMOUNT_PM1450   0x3501
#endif

#ifndef USB_DEVICE_ID_PENMOUNT_PM1610
#define USB_DEVICE_ID_PENMOUNT_PM1610   0x1610
#endif

#ifndef USB_DEVICE_ID_PENMOUNT_PM1640
#define USB_DEVICE_ID_PENMOUNT_PM1640   0x1640
#endif

#define PENMOUNT_P2_RESOLUTION          0x7FF
#define PENMOUNT_P3_RESOLUTION_USB      0xFFF
#define PENMOUNT_P3_RESOLUTION_I2C      0x7FF

#ifdef  USB_CTRL_SET_TIMEOUT
#undef  USB_CTRL_SET_TIMEOUT
#endif
#define USB_CTRL_SET_TIMEOUT    5000

#ifdef  USB_CTRL_GET_TIMEOUT
#undef  USB_CTRL_GET_TIMEOUT
#endif
#define USB_CTRL_GET_TIMEOUT    5000

#define PMDRIVER_MAJORVER 2
#define PMDRIVER_MINORVER 1
#define PMDRIVER_BUILDVER 0
#define PMDRIVER_VERSION  ((PMDRIVER_MAJORVER<<8)|(PMDRIVER_MINORVER<<4)|PMDRIVER_BUILDVER)

/* Structures */
struct strPENMOUNT;

struct strPMTOUCH
{
	unsigned char bUpdated;
	__s32 ID;
	__s32 TrackID;
	unsigned char Slot;
	unsigned char bTouch;
	unsigned char bTouching;
	unsigned short X;
	unsigned short Y;
	unsigned short LastX;
	unsigned short LastY;
	unsigned char LastState;
};

struct strPMCDEV
{
	int MajorNum;
	struct class *pClass;
	struct device *pDevice;
} ;

struct strPMUSBIF
{
	struct urb * pUrb;
	struct usb_device * pDevice;
	dma_addr_t DmaAddress;
};

struct strPMI2CIF
{
	int (*power)(int on);
	unsigned char bUseIRQ;
	unsigned char bIRQEventTrigger;
	struct i2c_client *driver;
	struct hrtimer timer;
	struct irq_desc *desc;
	struct work_struct work;
	struct mutex mutex;
	struct strPMCDEV cdev;
#ifdef CONFIG_HAS_EARLYSUSPEND
	struct early_suspend Android;
#endif
	int gpio_intr;
};

struct strPENMOUNT
{
	char szPhysDevice[64];
	char szDeviceName[64];

	unsigned char      MTProtocol;
	unsigned char      MaxTouch;
	unsigned short     Model;
	unsigned char      cbPacket;
	unsigned char     *pInputBuffer;
	unsigned char      pResponse[PENMOUNT_P2_PACKETSIZE];
	unsigned char      OpenCount;
	unsigned char      bParallelReport ;
	unsigned char      TouchCount;
	unsigned char      CurrentIndex;
	int                Resolution;
	unsigned char      bIterated;

	struct input_dev  *pInputDev;
	struct strPMTOUCH *pMainTouch;
	struct strPMTOUCH *pLastTouch;	
	struct strPMTOUCH  Touch[PENMOUNT_MAXTOUCH];
	struct strPMTOUCH  CurrentTouch;
	struct strPMTOUCH  TempTouch[PENMOUNT_MAXTOUCH] ;	

	struct strPMUSBIF  USB;
	struct strPMI2CIF  I2C;
	struct cdev        ChrDev;
};

/* Extern Variables */
extern int penmount_DbgLevel;

/* Function prototypes */
struct strPENMOUNT * pmDriver_InitContext(void);
int pmDriver_InputDevInit(struct strPENMOUNT *, struct device *, struct input_id *);
void pmDriver_InputDevExit(struct strPENMOUNT *);
int pmDriver_InputDevOpen(struct input_dev *);
void pmDriver_InputDevClose(struct input_dev *);
void pmDriver_ProcessEvent(struct input_dev *, struct strPENMOUNT *, struct strPMTOUCH *);
void pmDriver_ProcessMTEvent(struct input_dev *, struct strPENMOUNT *);
int pmDriver_SkipMTEvent(struct input_dev *, struct strPENMOUNT *);
void pmDriver_ResetTouchData(struct strPENMOUNT *pPenMount);
int  pmI2CIF_Init(void);
void pmI2CIF_Exit(void);
int  pmHIDIF_Init(void);
void pmHIDIF_Exit(void);
