/* 
 * penmount-i2c.c - PenMount I2C Driver for PCT 30xx TouchScreens
 *
 * Copyright (C) 2011 SiS, Inc.
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
 * From original Android driver Date: 2013/05/02 Version: 1.0
 */

#include <linux/module.h>
#include <linux/delay.h>
#include <linux/hrtimer.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/platform_device.h>
#include <linux/linkage.h>
#include <linux/slab.h>
#include <linux/cdev.h>
#include <linux/fs.h> // Define file_operations
#include <linux/gpio.h>
#include <linux/irq.h>
#include <linux/of_gpio.h>
#include <linux/uaccess.h>
#include <asm/irq.h>
#include "penmount.h"

struct strRMIPLATFORMDATA {
	int (*power)(int on); /* Only valid in first array entry */
};

#define PMI2CIF_POLLINGINTERVAL 10000000 			/* 10ms */
#define PMI2CIF_DRIVERNAME      "penmount_i2c"
#define PMI2CIF_WORKQUEUENAME   "pmi2cintwq"
#define PMI2CIF_GPIONAME        "pmi2cint"

/* PenMount P3 Definitions */

#define PENMOUNT_P3_REPORTID                            0x10
#define PENMOUNT_P3_BYTECOUNT_NOTOUCH                   3
#define PENMOUNT_P3_DATAINDEX_BYTECOUNT                 0
#define PENMOUNT_P3_DATAINDEX_REPORTID                  2

#define PENMOUNT_P3_DATASIZE_BYTECOUNT                  2
#define PENMOUNT_P3_DATASIZE_REPORTID                   1
#define PENMOUNT_P3_DATASIZE_TOUCH                      6

#define PENMOUNT_P2_TOUCHSTATUS_TOUCH                   0x70
#define PENMOUNT_P2_TOUCHSTATUS_RELEASE                 0x40
#define PENMOUNT_P3_TOUCHSTATUS_TOUCH                   0x03 
#define PENMOUNT_P3_TOUCHSTATUS_RELEASE                 0x00

#define PENMOUNT_P3_CMDSIZE_1                           1
#define PENMOUNT_P3_CMDSIZE_5                           5
#define PENMOUNT_P3_CMDSIZE_16                          16
#define PENMOUNT_P3_CMD_SOFTRESET                       0x82
#define PENMOUNT_P3_CMD_POWERMODE                       0x90

#define	PENMOUNT_P3_POWERMODE_FWCTRL                    0x50
#define PENMOUNT_P3_POWERMODE_ACTIVE                    0x51
#define PENMOUNT_P3_POWERMODE_SLEEP                     0x52

/* Global Variables */

static struct workqueue_struct *g_pWorkQueue = NULL;
static struct strPENMOUNT *g_pPenMount = NULL;

/* IRQ Functions */

static enum hrtimer_restart pmI2CIF_DoPolling(struct hrtimer *timer)
{
	struct strPMI2CIF *pPMI2CIF = NULL;

	pPMI2CIF = container_of(timer, struct strPMI2CIF, timer);
	queue_work(g_pWorkQueue, &pPMI2CIF->work);

	if (!pPMI2CIF->bUseIRQ) {
		hrtimer_start(&pPMI2CIF->timer, ktime_set(0, PMI2CIF_POLLINGINTERVAL), HRTIMER_MODE_REL);
	}
	return HRTIMER_NORESTART;
}

static irqreturn_t pmI2CIF_IRQHandler(int irq, void *dev_id)
{
	struct strPENMOUNT *pPenMount = NULL;

	pPenMount = (struct strPENMOUNT *)dev_id;
	if (!irqd_irq_disabled(&pPenMount->I2C.desc->irq_data))
	{
		disable_irq_nosync(pPenMount->I2C.driver->irq);
	}

	queue_work(g_pWorkQueue, &pPenMount->I2C.work);

	return IRQ_HANDLED;
}

static void pmI2CIF_IRQEnable(struct strPENMOUNT *pPenMount,
							  unsigned char bStartTimer)
{
	if (!pPenMount->I2C.bUseIRQ)
	{
		if (bStartTimer)
			hrtimer_start(&pPenMount->I2C.timer, ktime_set(1, 0), HRTIMER_MODE_REL);
		return;
	}

	if (irqd_irq_disabled(&pPenMount->I2C.desc->irq_data))
	{
		enable_irq(pPenMount->I2C.driver->irq);
	}
	return;
}

static void pmI2CIF_IRQDisable(struct strPENMOUNT *pPenMount)
{
	if (!pPenMount->I2C.bUseIRQ)
	{
		hrtimer_cancel(&pPenMount->I2C.timer);
		return;
	}

	if (!irqd_irq_disabled(&pPenMount->I2C.desc->irq_data))
	{
		disable_irq(pPenMount->I2C.driver->irq);
	}

	return;
}

/* I2C Functions */

int pmI2CIF_WriteData(struct i2c_client *pI2CClient,
		int wlength,
		unsigned char *pBufferOUT)
{
	int ret = -1;
	struct i2c_msg msg[1];

	msg[0].addr = pI2CClient->addr;
	msg[0].flags = 0;		/* Write */
	msg[0].len = wlength;
	msg[0].buf = (unsigned char *) pBufferOUT;

	ret = i2c_transfer(pI2CClient->adapter, msg, 1);

	return ret;
}

int pmI2CIF_ReadData(struct i2c_client *pI2CClient,
					int cData,
					unsigned char *pData,
					unsigned char SyncCHR)
{
	struct strPENMOUNT *pPenMount = NULL;
	int rc = 0;
	struct i2c_msg msg[1];
	int i=0;
	int retry =0;

	pPenMount = i2c_get_clientdata(pI2CClient);

	memset (pData, 0, cData);

	msg[0].addr = pI2CClient->addr;
	msg[0].flags = I2C_M_RD | I2C_M_IGNORE_NAK ;	/* Read */

	if (pPenMount->bIterated)
	{
		msg[0].len = 1;
		for(retry=0; retry<10; retry++)
		{
			msg[0].buf = pData;
			rc |= i2c_transfer(pI2CClient->adapter, msg, 1);
			if (SyncCHR)
			{
				if (pData[0] == SyncCHR)
					break;
			}
			else
			{
				if (((pData[0]&0xF0) == PENMOUNT_P2_TOUCHSTATUS_TOUCH)
					||((pData[0]&0xF0) == PENMOUNT_P2_TOUCHSTATUS_RELEASE))
					break;
			}
		}
		if (retry==10)
			return -1;

		if (penmount_DbgLevel > 3)
			printk ("[penmount-i2c] [0x%02X]", pData[0]);

		for(i=1; i<cData; i++)
		{
			msg[0].buf = pData+i;
			rc |= i2c_transfer(pI2CClient->adapter, msg, 1);
	
			if (penmount_DbgLevel > 3)
				printk ("[0x%02X]", pData[i]);
		}
		if (penmount_DbgLevel > 3)
			printk ("\n");
		return rc;
	}

	msg[0].len = cData;
	msg[0].buf = pData;
	rc = i2c_transfer(pI2CClient->adapter, msg, 1);

	if (penmount_DbgLevel > 3)
	{
		printk ("[penmount-i2c] (%d) ", rc);
		for (i=0; i<cData; i++)
			printk ("[0x%02X]", pData[i]);
		printk ("\n");
	}

	return rc;
}

static int pmI2CIF_P2_CheckPacket(unsigned char * pBuffer)
{
	int i = 0;
	unsigned short Total = 0;
	// Check CheckSUM
	
	for ( i=0; i<5; i++ )
		Total += pBuffer[i];

	if ( pBuffer[5] != (~Total & 0xFF) )
		return 0;

	return 1;
}

unsigned short pmI2CIF_CheckModel(struct i2c_client *pI2CClient)
{
	struct strPENMOUNT *pPenMount = NULL;
	int ret = 0;
	unsigned char pBufferOUT[PENMOUNT_P2_PACKETSIZE] = {0xEE, 0x00, 0x00, 0x00, 0x00, 0x11};
	unsigned char pBufferIN[PENMOUNT_P2_PACKETSIZE] = {0};

	pPenMount = i2c_get_clientdata(pI2CClient);

	if ( pPenMount == NULL )
		return PMDEVICE_PRODUCTID_I2C;

	pPenMount->bIterated = 0;

#ifdef PMDEVICE_TARGET_PRODUCT
	return PMDEVICE_TARGET_PRODUCT;
#endif
	ret = pmI2CIF_WriteData(pI2CClient, PENMOUNT_P2_PACKETSIZE, pBufferOUT);
	if (ret >= 0) {
		msleep(10);
		ret = pmI2CIF_ReadData(pI2CClient, PENMOUNT_P2_PACKETSIZE, pBufferIN, 0xEE);
		if ((pBufferIN[0] == 0xEE)
			|| ((pBufferIN[0]&0xF0) == PENMOUNT_P2_TOUCHSTATUS_TOUCH)
			|| ((pBufferIN[0]&0xF0) == PENMOUNT_P2_TOUCHSTATUS_RELEASE))
		{
			if ( pmI2CIF_P2_CheckPacket(pBufferIN) )
			{
				pPenMount->bIterated = 0;
				printk ("[penmount-i2c] Read Mode : whole packet.\n");
			}
			else
			{
				pPenMount->bIterated = 1;
				printk ("[penmount-i2c] Read Mode : iterated.\n");
				pmI2CIF_ReadData(pI2CClient, PENMOUNT_P2_PACKETSIZE, pBufferIN, 0xEE);
			}
			return PMDEVICE_MODEL_P2;
		}
		else if (ret >=0)
			return PMDEVICE_MODEL_P3;
	}
	pPenMount->bIterated = 1;
	printk("[penmount-i2c] Using default model %X !\n", PMDEVICE_PRODUCTID_I2C);
	return PMDEVICE_PRODUCTID_I2C;
}

/* PenMount P3 CDEV Functions */

#define PMI2CIF_P3_CDEV_NAME    "sis_aegis_touch_device"

int     pmI2CIF_P3_CDEV_Open    (struct inode *, struct file *);
int     pmI2CIF_P3_CDEV_Release (struct inode *, struct file *);
ssize_t pmI2CIF_P3_CDEV_Read    (struct file *, char __user *, size_t, loff_t *);
ssize_t pmI2CIF_P3_CDEV_Write   (struct file *, const char __user *, size_t, loff_t *);

static const struct file_operations PMI2CIF_P3_CDEV_FOPS = {
	.owner	= THIS_MODULE,
	.open	= pmI2CIF_P3_CDEV_Open,
	.release= pmI2CIF_P3_CDEV_Release,
	.read	= pmI2CIF_P3_CDEV_Read,
	.write	= pmI2CIF_P3_CDEV_Write,
};

static int pmI2CIF_P3_CDEV_Setup(struct strPENMOUNT *pPenMount)
{	
	dev_t DevNum = 0;

	if (pPenMount == NULL) 
		return 0;

	DevNum = MKDEV(pPenMount->I2C.cdev.MajorNum, 0);
	if (alloc_chrdev_region(&DevNum, 0, 1, PMI2CIF_P3_CDEV_NAME))
	{
		printk ("[penmount-i2c] CDEV Setup : FAILED on allocating CDEV\n");
		return 0;
	}
		
	pPenMount->I2C.cdev.MajorNum = MAJOR(DevNum);
	cdev_init(&pPenMount->ChrDev, &PMI2CIF_P3_CDEV_FOPS);
	pPenMount->ChrDev.owner = THIS_MODULE;
	
	if (cdev_add(&pPenMount->ChrDev, MKDEV(pPenMount->I2C.cdev.MajorNum, 0), 1)) 
	{
		unregister_chrdev_region(MKDEV(pPenMount->I2C.cdev.MajorNum, 0), 1);
		printk ("[penmount-i2c] CDEV Setup : FAILED on adding CDEV\n");
		return 0;
	}
	
	/* register class */
	pPenMount->I2C.cdev.pClass = class_create(THIS_MODULE, PMI2CIF_P3_CDEV_NAME);
	if(IS_ERR(pPenMount->I2C.cdev.pClass)) 
	{
		cdev_del(&pPenMount->ChrDev);
		unregister_chrdev_region(MKDEV(pPenMount->I2C.cdev.MajorNum, 0), 1);
		printk ("[penmount-i2c] CDEV Setup : FAILED on creating CDEV class\n");
		return 0;
	}
	
	pPenMount->I2C.cdev.pDevice = device_create(pPenMount->I2C.cdev.pClass, NULL, MKDEV(pPenMount->I2C.cdev.MajorNum, 0), pPenMount, PMI2CIF_P3_CDEV_NAME);
	if(IS_ERR(pPenMount->I2C.cdev.pDevice)) 
	{
		class_destroy(pPenMount->I2C.cdev.pClass);
		cdev_del(&pPenMount->ChrDev);
		unregister_chrdev_region(MKDEV(pPenMount->I2C.cdev.MajorNum, 0), 1);
		printk ("[penmount-i2c] CDEV Setup : FAILED on creating CDEV device\n");
		return 0;
	}
	dev_set_drvdata (pPenMount->I2C.cdev.pDevice, pPenMount);
	
	return 1;
}

static int pmI2CIF_P3_CDEV_Cleaup(struct strPENMOUNT *pPenMount)
{
	dev_t DevNum = 0;
	if (pPenMount->I2C.cdev.pClass == NULL)
		return 0;

	DevNum = MKDEV(pPenMount->I2C.cdev.MajorNum, 0);
	cdev_del(&pPenMount->ChrDev);
	unregister_chrdev_region(DevNum, 1);
	device_destroy(pPenMount->I2C.cdev.pClass, MKDEV(pPenMount->I2C.cdev.MajorNum, 0));
	class_destroy(pPenMount->I2C.cdev.pClass);

	return 0;
}

int pmI2CIF_P3_CDEV_Open(struct inode *piNode,
				struct file *pFile)
{
	struct strPENMOUNT *pPenMount = NULL;
	
	pPenMount = (struct strPENMOUNT *) container_of(piNode->i_cdev, struct strPENMOUNT, ChrDev);	
	if (pPenMount == NULL)
	{
		printk(KERN_ERR "[penmount-i2c] CDEV Open : Fail to get driver data !\n");
		return -13;
	}
	pFile->private_data = pPenMount;

	msleep(200);
	if (pPenMount->I2C.bUseIRQ)
		pmI2CIF_IRQDisable(pPenMount);

	flush_workqueue(g_pWorkQueue); 	   /* only flush g_pWorkQueue */
    
	msleep(200);

	return 0; /* success */
}

int pmI2CIF_P3_CDEV_Release(struct inode *piNode,
				struct file *pFile)
{
	struct strPENMOUNT *pPenMount = NULL;

	pPenMount = (struct strPENMOUNT *) pFile->private_data;
	if (pPenMount == NULL)
	{
		printk(KERN_ERR "[penmount-i2c] CDEV Close : Fail to get driver data !\n");
		return -13;
	}
	 msleep(200);

	if (pPenMount->I2C.bUseIRQ)
		pmI2CIF_IRQEnable(pPenMount, 1);

	return 0;
}

ssize_t pmI2CIF_P3_CDEV_Write( struct file *pFile,
				 const char __user *pBufferUsr,
				 size_t count,
				 loff_t *pOffset )
{
	struct strPENMOUNT *pPenMount = NULL;
	unsigned char *pBufferKrn = NULL;

	pPenMount = (struct strPENMOUNT *) pFile->private_data;
	if (pPenMount == NULL)
	{
		printk(KERN_ERR "[penmount-i2c] CDEV Write : Fail to get driver data !\n");
		return -13;
    	}

	if (!access_ok(VERIFY_WRITE, pBufferUsr, PENMOUNT_P3_PACKETSIZE))
	{
		printk(KERN_ERR "[penmount-i2c] CDEV Write : cannot access user space memory !\n");	
		return -11;
	}

	pBufferKrn = kmalloc(PENMOUNT_P3_PACKETSIZE, GFP_KERNEL);
	if (pBufferKrn == NULL)
	{
	    	return -12;
	}

	if (copy_from_user(pBufferKrn, pBufferUsr, PENMOUNT_P3_PACKETSIZE))
	{
		printk(KERN_ERR "[penmount-i2c] CDEV Write : copy_from_user failed !\n");
		kfree(pBufferKrn);
        	return -14;
	} 

	if (penmount_DbgLevel >2)
		printk ("[penmount-i2c] Send Command : [0x%02X]\n", pBufferKrn[6] );

	if (pmI2CIF_WriteData(pPenMount->I2C.driver, count, pBufferKrn) < 0)
	{
		printk(KERN_ERR "[penmount-i2c] CDEV Write : write failed !\n");
		kfree(pBufferKrn);
        	return -21;
	}

	kfree(pBufferKrn);

	return 0;
}


ssize_t pmI2CIF_P3_CDEV_Read (struct file *pFile,
				char __user *pBufferUsr,
				size_t count,
				loff_t *pOffset)
{
	struct strPENMOUNT *pPenMount = NULL;
	unsigned char *pBufferKrn = NULL;
	int i = 0;
	int ret = 0;

	pPenMount = (struct strPENMOUNT *) pFile->private_data;
	if (pPenMount == NULL)
	{
		printk(KERN_ERR "[penmount-i2c] CDEV Read : Fail to get driver data !\n");
		return -13;
	}
    	
	if (!access_ok(VERIFY_WRITE, pBufferUsr, PENMOUNT_P3_PACKETSIZE))
	{
		printk(KERN_ERR "[penmount-i2c] CDEV Read : cannot access user space memory !\n");
		return -11;
	}

	pBufferKrn = kmalloc(PENMOUNT_P3_PACKETSIZE, GFP_KERNEL);
	if (pBufferKrn == 0)
	{
		return -12;
	}

	if (copy_from_user(pBufferKrn, pBufferUsr, PENMOUNT_P3_PACKETSIZE))
	{
		printk(KERN_ERR "[penmount-i2c] CDEV Read : copy_from_user failed !\n");
		kfree(pBufferKrn);
		return -14;
	}    

	/* for making sure AP communicates with SiS driver */
	if(pBufferKrn[6] == 0xA2)
	{
		pBufferKrn[0] = 5;
		pBufferKrn[1] = 0;
		pBufferKrn[3] = 'S';
		pBufferKrn[4] = 'i';
		pBufferKrn[5] = 'S';
		if (copy_to_user((char*) pBufferUsr, pBufferKrn, PENMOUNT_P3_PACKETSIZE))
		{
			printk(KERN_ERR "[penmount-i2c] CDEV Read : copy_to_user fail !\n" );
			kfree( pBufferKrn );
			return -19;
		}

		kfree( pBufferKrn );
		return 3;	
	}

	if (pmI2CIF_ReadData(pPenMount->I2C.driver, PENMOUNT_P3_PACKETSIZE, pBufferKrn, 0) < 0)
	{
		printk(KERN_ERR "[penmount-i2c] CDEV Read : i2c_transfer read error !\n");
		kfree(pBufferKrn);
		return -21;
	}

	ret = pBufferKrn[0] | (pBufferKrn[1] << 8);

	if (penmount_DbgLevel >2)
	{
		printk ("[penmount-i2c] Read Response ");
		for (i = 0; i < ret && i < PENMOUNT_P3_PACKETSIZE; i++)
			printk("[0x%02X]", pBufferKrn[i]);
		printk( "\n" );
	}

	if (copy_to_user((char*)pBufferUsr, pBufferKrn, PENMOUNT_P3_PACKETSIZE))
	{
		printk(KERN_ERR "[penmount-i2c] CDEV Read : copy_to_user fail !\n" );
		ret = -19;
	}

	kfree( pBufferKrn );

	return ret;
}

/* PenMount P3 Functions */

static const unsigned short PENMOUNT_P3_CRC16_TABLE[256] =
{
	0x0000,0x1021,0x2042,0x3063,0x4084,0x50a5,0x60c6,0x70e7,
        0x8108,0x9129,0xa14a,0xb16b,0xc18c,0xd1ad,0xe1ce,0xf1ef,
        0x1231,0x0210,0x3273,0x2252,0x52b5,0x4294,0x72f7,0x62d6,
        0x9339,0x8318,0xb37b,0xa35a,0xd3bd,0xc39c,0xf3ff,0xe3de,
        0x2462,0x3443,0x0420,0x1401,0x64e6,0x74c7,0x44a4,0x5485,
        0xa56a,0xb54b,0x8528,0x9509,0xe5ee,0xf5cf,0xc5ac,0xd58d,
        0x3653,0x2672,0x1611,0x0630,0x76d7,0x66f6,0x5695,0x46b4,
        0xb75b,0xa77a,0x9719,0x8738,0xf7df,0xe7fe,0xd79d,0xc7bc,
        0x48c4,0x58e5,0x6886,0x78a7,0x0840,0x1861,0x2802,0x3823,
        0xc9cc,0xd9ed,0xe98e,0xf9af,0x8948,0x9969,0xa90a,0xb92b,
        0x5af5,0x4ad4,0x7ab7,0x6a96,0x1a71,0x0a50,0x3a33,0x2a12,
        0xdbfd,0xcbdc,0xfbbf,0xeb9e,0x9b79,0x8b58,0xbb3b,0xab1a,
        0x6ca6,0x7c87,0x4ce4,0x5cc5,0x2c22,0x3c03,0x0c60,0x1c41,
        0xedae,0xfd8f,0xcdec,0xddcd,0xad2a,0xbd0b,0x8d68,0x9d49,
        0x7e97,0x6eb6,0x5ed5,0x4ef4,0x3e13,0x2e32,0x1e51,0x0e70,
        0xff9f,0xefbe,0xdfdd,0xcffc,0xbf1b,0xaf3a,0x9f59,0x8f78,
        0x9188,0x81a9,0xb1ca,0xa1eb,0xd10c,0xc12d,0xf14e,0xe16f,
        0x1080,0x00a1,0x30c2,0x20e3,0x5004,0x4025,0x7046,0x6067,
        0x83b9,0x9398,0xa3fb,0xb3da,0xc33d,0xd31c,0xe37f,0xf35e,
        0x02b1,0x1290,0x22f3,0x32d2,0x4235,0x5214,0x6277,0x7256,
        0xb5ea,0xa5cb,0x95a8,0x8589,0xf56e,0xe54f,0xd52c,0xc50d,
        0x34e2,0x24c3,0x14a0,0x0481,0x7466,0x6447,0x5424,0x4405,
        0xa7db,0xb7fa,0x8799,0x97b8,0xe75f,0xf77e,0xc71d,0xd73c,
        0x26d3,0x36f2,0x0691,0x16b0,0x6657,0x7676,0x4615,0x5634,
        0xd94c,0xc96d,0xf90e,0xe92f,0x99c8,0x89e9,0xb98a,0xa9ab,
        0x5844,0x4865,0x7806,0x6827,0x18c0,0x08e1,0x3882,0x28a3,
        0xcb7d,0xdb5c,0xeb3f,0xfb1e,0x8bf9,0x9bd8,0xabbb,0xbb9a,
        0x4a75,0x5a54,0x6a37,0x7a16,0x0af1,0x1ad0,0x2ab3,0x3a92,
        0xfd2e,0xed0f,0xdd6c,0xcd4d,0xbdaa,0xad8b,0x9de8,0x8dc9,
        0x7c26,0x6c07,0x5c64,0x4c45,0x3ca2,0x2c83,0x1ce0,0x0cc1,
        0xef1f,0xff3e,0xcf5d,0xdf7c,0xaf9b,0xbfba,0x8fd9,0x9ff8,
        0x6e17,0x7e36,0x4e55,0x5e74,0x2e93,0x3eb2,0x0ed1,0x1ef0
};

unsigned short pmI2CIF_P3_CalCMDCRC(unsigned char* pCommand,
					unsigned char CMD,
					int start,
					int end)
{
	int i = 0;
	unsigned short wCRC = 0;
	
	wCRC = (wCRC<<8) ^ PENMOUNT_P3_CRC16_TABLE[((wCRC>>8) ^ CMD)&0x00FF];
	for (i = start; i <= end; i++) {
		wCRC = (wCRC << 8) ^ PENMOUNT_P3_CRC16_TABLE[((wCRC >> 8) ^ pCommand[i]) & 0x00FF];
	}
	return wCRC;
}

int pmI2CIF_P3_SendCommand(struct i2c_client *pI2CClient,
		int wlength,
		unsigned char *pBufferOUT,
		int rlength,
		unsigned char *pBufferIN)
{
	int ret = -1;

	if (penmount_DbgLevel >2)
		printk ("============================================\n");

	if (penmount_DbgLevel >2)
	{
		int i = 0;
		printk ("[penmount-i2c] Sending CMD : \n");
		for (i=0; i<wlength; i++)
		{
			printk ("[0x%02X]", pBufferOUT[i]);
			if (i % 8 == 7)
				printk ("\n");
		}
		printk ("\n");
	}
	ret = pmI2CIF_WriteData(pI2CClient, wlength, pBufferOUT);
	if (ret < 0) {
		if (pBufferOUT[6] == 0x85) {
			printk(KERN_ERR "[penmount-i2c] Sending Power CMD : [0x%02X] failed ! (%d)\n", pBufferOUT[8] ,ret);
		} else {
			printk(KERN_ERR "[penmount-i2c] Sending CMD : [0x%2X] failed ! (%d)\n", pBufferOUT[6] ,ret);
		}
	} else {
		msleep(3000);
		ret = pmI2CIF_ReadData(pI2CClient, rlength, pBufferIN, 0);
		if (penmount_DbgLevel >2)
		{
			int i = 0;
			printk ("[penmount-i2c] Getting ACK : \n");
			for (i=0; i<pBufferIN[0]; i++)
			{
				printk ("[0x%02X]", pBufferIN[i]);
				if (i % 8 == 7)
					printk ("\n");
			}
			printk ("\n");
		}
		if (ret < 0) {
			printk(KERN_ERR "[penmount-i2c] Sending CMD : [0x%2X] read failed ! (%d)\n", pBufferOUT[0] ,ret);
		}
	}

	return ret;
}

#ifdef CONFIG_TOUCHSCREEN_PENMOUNT_I2C_POWERSUPPORT

#define PENMOUNT_P3_CMDACK_L       0xEF
#define PENMOUNT_P3_CMDACK_H       0xBE
#define PENMOUNT_P3_CMDNACK_L      0xAD
#define PENMOUNT_P3_CMDNACK_H      0xDE
#define PENMOUNT_P3_CMDINDEX_ACK_L 4
#define PENMOUNT_P3_CMDINDEX_ACK_H 5
#define PENMOUNT_P3_CMDINDEX_CRC   7

static int pmI2CIF_P3_SetPowerMode(struct i2c_client *pI2CClient,
				int mode)
{
	unsigned char  pBuffer[PENMOUNT_P3_PACKETSIZE]  = {0};
	unsigned char  pPWRCMD_FWCtrl[10] = {0x04, 0x00, 0x08, 0x00, 0x09, 0x00, 
						0x85, 0x3c, 0x50, 0x09};
	unsigned char  pPWRCMD_Active[10] = {0x04, 0x00, 0x08, 0x00, 0x09, 0x00,
						0x85, 0x0d, 0x51, 0x09};
	unsigned char  pPWRCMD_Sleep [10] = {0x04, 0x00, 0x08, 0x00, 0x09, 0x00,
						0x85, 0x5e, 0x52, 0x09};
	unsigned char *pCommand           = NULL;
		
	switch(mode)
	{
	case PENMOUNT_P3_POWERMODE_FWCTRL:
		pCommand = pPWRCMD_FWCtrl;
		break;
	case PENMOUNT_P3_POWERMODE_ACTIVE:
		pCommand = pPWRCMD_Active;
		break;
	case PENMOUNT_P3_POWERMODE_SLEEP:
		pCommand = pPWRCMD_Sleep;
		break;
	default:
		return 0;
	}

	if (pmI2CIF_P3_SendCommand(pI2CClient, 10, pCommand, PENMOUNT_P3_PACKETSIZE, pBuffer) < 0)
	{
		printk(KERN_ERR "[penmount-i2c] Set power command failed !\n");
		return 0;
	}
	
	if ((pBuffer[PENMOUNT_P3_CMDINDEX_ACK_L] == PENMOUNT_P3_CMDNACK_L)
	&& (pBuffer[PENMOUNT_P3_CMDINDEX_ACK_H] == PENMOUNT_P3_CMDNACK_H)) // 0xDEAD
	{
		printk(KERN_ERR "[penmount-i2c] Set power command return NACK !\n");
		return 0;
	}
	else if ((pBuffer[PENMOUNT_P3_CMDINDEX_ACK_L] != PENMOUNT_P3_CMDACK_L)
		|| (pBuffer[PENMOUNT_P3_CMDINDEX_ACK_H] != PENMOUNT_P3_CMDACK_H)) // BEEF
	{
		printk(KERN_ERR "[penmount-i2c] Set power command return unknown !\n");
		return 0;
	}

	msleep(100);

	return 1;
}

static int pmI2CIF_P3_GetPowerMode(struct i2c_client *pI2CClient,
				int *pMode)
{
	uint8_t pBuffer[PENMOUNT_P3_PACKETSIZE] = {0};
	uint8_t pCommand[14] = {0x04, 0x00, 0x0c, 0x00, 0x09, 0x00,
				0x86, 0x00, 0x00, 0x00,	0x00, 0x50, 0x34, 0x00};

	pCommand[PENMOUNT_P3_CMDINDEX_CRC] = pmI2CIF_P3_CalCMDCRC(pCommand, 0x86, 8, 13) & 0xFF ;
	
	if(pmI2CIF_P3_SendCommand(pI2CClient, sizeof(pCommand), pCommand, sizeof(pCommand), pBuffer) < 0)
	{
		printk(KERN_ERR "[penmount-i2c] Get power mode <FAILED> !\n");
		return 0;
	}

	if ((pBuffer[PENMOUNT_P3_CMDINDEX_ACK_L] == PENMOUNT_P3_CMDNACK_L)
	&& (pBuffer[PENMOUNT_P3_CMDINDEX_ACK_H] == PENMOUNT_P3_CMDNACK_H)) // 0xDEAD
	{
		printk(KERN_ERR "[penmount-i2c] Get power mode <FAILED> : return NACK !\n");
		return 0;
	}
	else if ((pBuffer[PENMOUNT_P3_CMDINDEX_ACK_L] != PENMOUNT_P3_CMDACK_L)
		|| (pBuffer[PENMOUNT_P3_CMDINDEX_ACK_H] != PENMOUNT_P3_CMDACK_H)) // BEEF
	{
		printk(KERN_ERR "[penmount-i2c] Get power mode <FAILED> : unknown ACK !\n");
		return 0;
	}
	
	switch(pBuffer[10])
	{
	case PENMOUNT_P3_POWERMODE_FWCTRL:
		*pMode = PENMOUNT_P3_POWERMODE_FWCTRL;
		if (penmount_DbgLevel > 0)
			printk ("[penmount-i2c] Get power mode <OK> : Firmware Control !\n");
		return 1;
	case PENMOUNT_P3_POWERMODE_ACTIVE:
		*pMode = PENMOUNT_P3_POWERMODE_ACTIVE;
		if (penmount_DbgLevel > 0)
			printk ("[penmount-i2c] Get power mode <OK> : Active !\n");
		return 1;
	case PENMOUNT_P3_POWERMODE_SLEEP :
		*pMode = PENMOUNT_P3_POWERMODE_SLEEP;
		if (penmount_DbgLevel > 0)
			printk ("[penmount-i2c] Get power mode <OK> : Sleep !\n");
		return 1;
	}

	if (penmount_DbgLevel > 0)
		printk ("[penmount-i2c] Get power mode <FAILED> : Unknown mode %d !\n", pBuffer[10]);

	return 0;
}
#endif //CONFIG_TOUCHSCREEN_PENMOUNT_I2C_POWERSUPPORT

static int pmI2CIF_P3_SuspendDevice(struct strPENMOUNT *pPenMount)
{
	int ret = 0;
#ifdef CONFIG_TOUCHSCREEN_PENMOUNT_I2C_POWERSUPPORT
	int retry = 5;	
	int PowerMode = -1;
#endif
	if (penmount_DbgLevel > 0)
		printk ("[penmount-i2c] Suspending Device !\n");

	pmI2CIF_IRQDisable(pPenMount);
	flush_workqueue(g_pWorkQueue);

#ifdef CONFIG_TOUCHSCREEN_PENMOUNT_I2C_POWERSUPPORT
	pmI2CIF_P3_GetPowerMode (pPenMount->I2C.driver, &PowerMode);		
	for (retry=5 ; (retry>0)&&(PowerMode!=PENMOUNT_P3_POWERMODE_SLEEP) ; retry--)
	{
		pmI2CIF_P3_SetPowerMode (pPenMount->I2C.driver, PENMOUNT_P3_POWERMODE_SLEEP);
		msleep(50);
		pmI2CIF_P3_GetPowerMode (pPenMount->I2C.driver, &PowerMode);		
	}

	if (retry == 0)
		printk(KERN_ERR "[penmount-i2c] Suspend Failed !\n");
	else
		printk(KERN_ERR "[penmount-i2c] Device suspended !\n");
#endif

	if (pPenMount->I2C.power) {
		ret = pPenMount->I2C.power(0);
		if (ret < 0)
			printk(KERN_ERR "[penmount-i2c] Failed on powering off device !\n");
	}

	return 0;
}

static int pmI2CIF_P3_ResumeDevice(struct strPENMOUNT *pPenMount)
{
	int ret = 0;

#ifdef CONFIG_TOUCHSCREEN_PENMOUNT_I2C_POWERSUPPORT
	int retry = 5;
	int PowerMode = -1;
#endif

	if (penmount_DbgLevel > 0)
		printk ("[penmount-i2c] Resuming Device !\n");

	if (pPenMount->I2C.power) {
		ret = pPenMount->I2C.power(1);
		if (ret < 0)
			printk(KERN_ERR "[penmount-i2c] Failed on resuming device !\n");
	}

#ifdef CONFIG_TOUCHSCREEN_PENMOUNT_I2C_POWERSUPPORT
	pmI2CIF_P3_GetPowerMode (pPenMount->I2C.driver, &PowerMode);		
	for (retry=5 ; (retry>0)&&(PowerMode!=PENMOUNT_P3_POWERMODE_FWCTRL) ; retry--)
	{
		pmI2CIF_P3_SetPowerMode (pPenMount->I2C.driver, PENMOUNT_P3_POWERMODE_ACTIVE);
		msleep(50);
		pmI2CIF_P3_GetPowerMode (pPenMount->I2C.driver, &PowerMode);		
	}

	if (retry == 0)
		printk(KERN_ERR "[penmount-i2c] Resume Failed !\n");
	else
		printk(KERN_ERR "[penmount-i2c] Device resumed !\n");
#endif

	pmI2CIF_IRQEnable(pPenMount, 1);

	return 0;
}

/* PenMount Read Data Functions */

static int pmI2CIF_P2_ReadPacket(struct i2c_client *pI2CClient, 
		unsigned char * pBuffer)
{
	unsigned char pTempBuffer[PENMOUNT_P2_PACKETSIZE] = { 0 };
	int ret = -1;

	ret = pmI2CIF_ReadData(pI2CClient, PENMOUNT_P2_PACKETSIZE, pTempBuffer, 0);

	if (ret < 0)
	{
		printk(KERN_ERR "[penmount-i2c] Failed on reading P2 data : (%d)\n", ret);
		return ret;
	}

	/* Check Packet Data */
	if ( ! pmI2CIF_P2_CheckPacket ( pTempBuffer ) )
	{
		printk(KERN_ERR "[penmount-i2c] Invalid CheckSUM !\n");
		return -1;
	}

	if ( ( ( pTempBuffer[0] & 0xF0 ) != PENMOUNT_P2_TOUCHSTATUS_TOUCH)
		&& ( ( pTempBuffer[0] & 0xF0 ) != PENMOUNT_P2_TOUCHSTATUS_RELEASE) )
	{
		printk(KERN_ERR "[penmount-i2c] Invalid touch data !\n");
		return -1;
	}

	memcpy(pBuffer, pTempBuffer, PENMOUNT_P2_PACKETSIZE);

	return 0;
}

static void pmI2CIF_P2_ReadTouchData(struct strPENMOUNT *pPenMount)
{
	int ret = -1;
	unsigned char pBuffer[PENMOUNT_P2_PACKETSIZE] = { 0 };
	unsigned char i = 0;
	int bProcessEvents=1;
	unsigned char TouchID = 0;
	
	/* I2C or SMBUS block data read */
	ret = pmI2CIF_P2_ReadPacket(pPenMount->I2C.driver, pBuffer);

	if (ret < 0)
		return ;

	if (penmount_DbgLevel >2)
		printk ("[penmount-i2c] [0x%02X][0x%02X][0x%02X][0x%02X][0x%02X][0x%02X]\n", pBuffer[0], pBuffer[1], pBuffer[2], pBuffer[3], pBuffer[4], pBuffer[5]);
	/* [TOUCH][X][X][Y][Y] */
	TouchID = pBuffer[0]&0x0F;
	if ( pPenMount->Touch[TouchID].bUpdated )
	{
		if ( !pmDriver_SkipMTEvent ( pPenMount->pInputDev, pPenMount ) )
			pmDriver_ProcessMTEvent ( pPenMount->pInputDev, pPenMount );
	}

	pPenMount->Touch[TouchID].bTouch = (pBuffer[0]&0xF0)==PENMOUNT_P2_TOUCHSTATUS_TOUCH;
	pPenMount->Touch[TouchID].ID = TouchID;
	pPenMount->Touch[TouchID].X = ((pBuffer[1] & 0xFF) | ((pBuffer[2] & 0xFF) << 8));
	pPenMount->Touch[TouchID].Y = ((pBuffer[3] & 0xFF) | ((pBuffer[4] & 0xFF) << 8));
	pPenMount->Touch[TouchID].bUpdated = 1 ;

	/* Set unupdated pPenMount->Touch[i].bTouching to 0, but keeps original ID value */
	pmDriver_ProcessEvent ( pPenMount->pInputDev, pPenMount, &pPenMount->Touch[TouchID] ) ;

	i = 0;
	do
	{
		if ( ( pPenMount->Touch[i].bUpdated )
			&& ( pPenMount->Touch[i].bTouching ) )
		{
			bProcessEvents = 0 ;
			break;
		}
	} while (i++ < TouchID-1);

	if ( !bProcessEvents )
		return;

	if ( pmDriver_SkipMTEvent ( pPenMount->pInputDev, pPenMount ) )
		return;

	pmDriver_ProcessMTEvent ( pPenMount->pInputDev, pPenMount ) ;

	return;
}

static int pmI2CIF_P3_ReadPacket(struct i2c_client *pI2CClient, 
				unsigned char* pBuffer)
{
	unsigned char pTempBuffer[PENMOUNT_P3_PACKETSIZE] = { 0 };

	int ret = -1;
	int TouchCount = 0;

	ret = pmI2CIF_ReadData(pI2CClient, PENMOUNT_P3_PACKETSIZE, pTempBuffer, 0);

	if (ret < 0) {
		printk(KERN_ERR "[penmount-i2c] Failed on reading P3 data : (%d)\n", ret);
		return ret;
	}
	/* error package length of receiving data */
	else if (pTempBuffer[PENMOUNT_P3_DATAINDEX_BYTECOUNT] > PENMOUNT_P3_PACKETSIZE) {
		printk(KERN_ERR "[penmount-i2c] Invalid touch data : Bytecount (%d)\n", pTempBuffer[PENMOUNT_P3_DATAINDEX_BYTECOUNT]);
		return -1;
	}

	/* access NO TOUCH event unless BUTTON NO TOUCH event */
	if (pTempBuffer[PENMOUNT_P3_DATAINDEX_BYTECOUNT] == PENMOUNT_P3_BYTECOUNT_NOTOUCH) {
		return 0;	//touchnum is 0
	}

	/* skip parsing data when two devices are registered at the same slave address
	   parsing data when PENMOUNT_P3_DATAINDEX_REPORTID is ALL_IN_ONE_PACKAGE */
	if (pTempBuffer[PENMOUNT_P3_DATAINDEX_REPORTID] != PENMOUNT_P3_REPORTID){
		printk(KERN_ERR "[penmount-i2c] Invalid touch data : Report ID (%X)\n", pTempBuffer[PENMOUNT_P3_DATAINDEX_REPORTID]);
		return -1;
	}

	TouchCount = pTempBuffer[pTempBuffer[PENMOUNT_P3_DATAINDEX_BYTECOUNT] - 1];

	memcpy(pBuffer, pTempBuffer, PENMOUNT_P3_PACKETSIZE);

	return TouchCount;
}

static void pmI2CIF_P3_ReadTouchData(struct strPENMOUNT *pPenMount)
{
	int ret = -1;
	unsigned char pBuffer[PENMOUNT_P3_PACKETSIZE] = { 0 };
	unsigned char i = 0, TouchCount = 0;
	unsigned char PacketIndex = 0;
	int bPacketValid=1;

	/* I2C or SMBUS block data read */
	ret = pmI2CIF_P3_ReadPacket(pPenMount->I2C.driver, pBuffer);
	if (ret < 0)
	{
		printk(KERN_INFO "[penmount-i2c] Read Packet Error : %d\n", ret);
		return;
	}

	/* TouchCount check, make sure not exceeding PENMOUNT_MAXTOUCH */
	TouchCount = (ret > PENMOUNT_MAXTOUCH ? PENMOUNT_MAXTOUCH : ret);

	if (penmount_DbgLevel >2)
	{
		printk ("============================================\n");
		if (TouchCount == 0)
			printk ("No Touch !\n");
	}
	for (i = 0; i < TouchCount; i++)
	{
		unsigned char TouchID = 0;
		PacketIndex = PENMOUNT_P3_DATASIZE_BYTECOUNT + PENMOUNT_P3_DATASIZE_REPORTID + (i * PENMOUNT_P3_DATASIZE_TOUCH);
		/* [TOUCH][ID][X][X][Y][Y] */
		TouchID = (pBuffer[PacketIndex + 1]);
		if (penmount_DbgLevel >2)
		{
			printk("[0x%02X][0x%02X][0x%02X][0x%02X][0x%02X][0x%02X] => ", pBuffer[PacketIndex], pBuffer[PacketIndex+1], pBuffer[PacketIndex+2], pBuffer[PacketIndex+3], pBuffer[PacketIndex+4], pBuffer[PacketIndex+5]);
		}
		
		switch (pBuffer[PacketIndex])
		{
		case PENMOUNT_P3_TOUCHSTATUS_RELEASE:
			pPenMount->Touch[TouchID].bTouch = 0;
			break;
		case PENMOUNT_P3_TOUCHSTATUS_TOUCH:
			pPenMount->Touch[TouchID].bTouch = 1;
			break;
		default:
			bPacketValid = 0;
			printk(KERN_ERR "[penmount-i2c] Invalid touch status : %X !\n", pBuffer[PacketIndex]);
			break;
		}

		if (!bPacketValid)
			break;

		pPenMount->Touch[TouchID].ID = TouchID;
		pPenMount->Touch[TouchID].X = ((pBuffer[PacketIndex+2] & 0xFF) | ((pBuffer[PacketIndex+3] & 0xFF) << 8));
		pPenMount->Touch[TouchID].Y = ((pBuffer[PacketIndex+4] & 0xFF) | ((pBuffer[PacketIndex+5] & 0xFF) << 8));

		if (penmount_DbgLevel >1)
		{
			printk ("[%02d] %s (%d,%d)\n", TouchID, pPenMount->Touch[TouchID].bTouch?"DOWN":" UP ", pPenMount->Touch[TouchID].X, pPenMount->Touch[TouchID].Y );
		}
	}

	/* Set unupdated pPenMount->Touch[i].bTouching to 0, but keeps original ID value */
	for ( i = 0; i < PENMOUNT_MAXTOUCH; i++ )
		pmDriver_ProcessEvent ( pPenMount->pInputDev, pPenMount, &pPenMount->Touch[i] ) ;

	if ( pmDriver_SkipMTEvent ( pPenMount->pInputDev, pPenMount ) )
		return;

	pmDriver_ProcessMTEvent ( pPenMount->pInputDev, pPenMount ) ;

	return;
}

static void pmI2CIF_ReadTouchData(struct work_struct *work)
{
	struct strPMI2CIF *pPMI2CIF = NULL;
	struct strPENMOUNT *pPenMount = NULL;

	pPMI2CIF = container_of(work, struct strPMI2CIF, work);
	pPenMount = i2c_get_clientdata(pPMI2CIF->driver);
	
	mutex_lock(&pPMI2CIF->mutex);

	switch (pPenMount->Model)
	{
	case PMDEVICE_MODEL_P2:
		pmI2CIF_P2_ReadTouchData(pPenMount);
		break;
	case PMDEVICE_MODEL_P3:
		pmI2CIF_P3_ReadTouchData(pPenMount);
		break;
	}

	if (pPenMount->I2C.bUseIRQ)
	{
		if (!gpio_get_value(pPenMount->I2C.gpio_intr))
		{
			/* If interrupt pin is still LOW, read data until interrupt pin is released. */
			hrtimer_start(&pPenMount->I2C.timer, ktime_set(0, PMI2CIF_POLLINGINTERVAL), HRTIMER_MODE_REL);
		}
		else
		{
			pmI2CIF_IRQEnable(pPenMount, 0);
		}
	}

	mutex_unlock(&pPMI2CIF->mutex);

	return;
}

/* I2C Driver Functions */

static int pmI2CIF_DriverProbe(struct i2c_client *, const struct i2c_device_id *);
static int pmI2CIF_DriverRemove(struct i2c_client *);
static int pmI2CIF_DriverSuspend(struct device *dev);
static int pmI2CIF_DriverResume(struct device *dev);


static int pmI2CIF_DriverProbe(struct i2c_client *pI2CClient,
		const struct i2c_device_id *ID)
{
	int ret = 0;
	struct strPENMOUNT *pPenMount = NULL;
	struct strRMIPLATFORMDATA *pdata = NULL;
	struct input_id InputID ;
	struct device_node *np = pI2CClient->dev.of_node;

	printk(KERN_INFO "[penmount-i2c] Probing for device ...\n");

	if (!np) {
		printk(KERN_ERR "[penmount-i2c] No device node found!\n");
		return -ENODEV;
	}

	pPenMount = pmDriver_InitContext ();
	if (pPenMount == NULL) {
		return -ENOMEM;
	}
	g_pPenMount = pPenMount;

	mutex_init(&pPenMount->I2C.mutex);

	pPenMount->I2C.gpio_intr = of_get_named_gpio(np, "gpio_intr", 0);
	if (!gpio_is_valid(pPenMount->I2C.gpio_intr)) {
		printk(KERN_ERR "[penmount-i2c] Invalid gpio_intr!\n");
		return -ENODEV;
	}

	/* Init Work queue and necessary buffers */
	g_pWorkQueue = create_singlethread_workqueue(PMI2CIF_WORKQUEUENAME);
	if (g_pWorkQueue==NULL)
		return -ENOMEM;

	INIT_WORK(&pPenMount->I2C.work, pmI2CIF_ReadTouchData);
	pPenMount->I2C.driver = pI2CClient;
	i2c_set_clientdata(pI2CClient, pPenMount);

	pdata = pI2CClient->dev.platform_data;

	if (pdata)
		pPenMount->I2C.power = pdata->power;

	if (pPenMount->I2C.power) {
		ret = pPenMount->I2C.power(1);
		if (ret < 0) {
			printk(KERN_ERR "[penmount-i2c] Failed on setting power !\n");
			kfree(pPenMount);
			return ret;
		}
	}

	pPenMount->Model = pmI2CIF_CheckModel(pI2CClient);
	InputID.bustype = BUS_I2C;
	InputID.vendor  = PMDEVICE_VENDORID_I2C ;
	InputID.product = pPenMount->Model ;
	InputID.version = PMDRIVER_VERSION ;
	switch (pPenMount->Model)
	{
	case PMDEVICE_MODEL_P2:
		pPenMount->I2C.bIRQEventTrigger = 0;
		snprintf(pPenMount->szDeviceName, sizeof(pPenMount->szDeviceName),
				"PenMount P2 I2C TouchScreen");
		break;
	case PMDEVICE_MODEL_P3:
		pPenMount->I2C.bIRQEventTrigger = 1;
		snprintf(pPenMount->szDeviceName, sizeof(pPenMount->szDeviceName),
				"PenMount P3 I2C TouchScreen");
#ifdef CONFIG_TOUCHSCREEN_PENMOUNT_I2C_POWERSUPPORT
		pmI2CIF_P3_SetPowerMode(pPenMount->I2C.driver, PENMOUNT_P3_POWERMODE_ACTIVE);
#endif
		break;
	default:
		printk(KERN_ERR "[penmount-i2c] Unknown device or device not found !\n");
		kfree(pPenMount);
		return -ENODEV;
	}

	snprintf(pPenMount->szPhysDevice, sizeof(pPenMount->szPhysDevice), "%s/input0", pI2CClient->name);

	ret = pmDriver_InputDevInit(pPenMount, &pI2CClient->dev, &InputID);
	if (ret)
	{
		kfree(pPenMount);
		return ret;
	}

	/* irq setup */

	ret = request_irq(pI2CClient->irq, pmI2CIF_IRQHandler, IRQF_TRIGGER_LOW, pI2CClient->name, pPenMount);
	if (ret == 0) {
		printk( "[penmount-i2c] Using IRQ : %d name=%s\n", pI2CClient->irq, pI2CClient->name);
		pPenMount->I2C.bUseIRQ = 1;
	} else {
		printk( "[penmount-i2c] Failed on requesting IRQ ! GPIO : %d\n", pI2CClient->irq);
	}

	ret = gpio_request(pPenMount->I2C.gpio_intr, "penmount_gpio_intr");
	if (ret < 0)
		printk( "[penmount-i2c] Failed on requesting gpio_intr (%d): %d\n", pPenMount->I2C.gpio_intr, ret);

	pPenMount->I2C.desc = irq_to_desc(pI2CClient->irq);

	/* timer setup */
	hrtimer_init(&pPenMount->I2C.timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	pPenMount->I2C.timer.function = pmI2CIF_DoPolling;

	if (!pPenMount->I2C.bUseIRQ) {
		hrtimer_start(&pPenMount->I2C.timer, ktime_set(1, 0), HRTIMER_MODE_REL);
	}

	printk(KERN_INFO "[penmount-i2c] Using %s mode for getting touch data !\n", pPenMount->I2C.bUseIRQ ? "interrupt" : "polling");

	switch (pPenMount->Model)
	{
	case PMDEVICE_MODEL_P2:
		break;
	case PMDEVICE_MODEL_P3:
		pmI2CIF_P3_CDEV_Setup(pPenMount);
		break;
	}
		
	return 0;
}

static int pmI2CIF_DriverRemove(struct i2c_client *pI2CClient)
{
	struct strPENMOUNT *pPenMount = NULL;

	pPenMount = i2c_get_clientdata(pI2CClient);

	if (pPenMount->I2C.bUseIRQ)
		free_irq(pI2CClient->irq, pPenMount);
	else
		hrtimer_cancel(&pPenMount->I2C.timer);
	input_unregister_device(pPenMount->pInputDev);
	if (g_pPenMount != NULL)
		pmI2CIF_P3_CDEV_Cleaup(g_pPenMount);
	if (g_pWorkQueue != NULL)
		destroy_workqueue(g_pWorkQueue);

	kfree(pPenMount);
	return 0;
}

#ifdef CONFIG_PM_SLEEP
static int pmI2CIF_DriverSuspend(struct device *dev)
{
	struct i2c_client *pI2CClient = to_i2c_client(dev);
	struct strPENMOUNT *pPenMount = i2c_get_clientdata(pI2CClient);

	if (pPenMount->Model == PMDEVICE_MODEL_P3)
		pmI2CIF_P3_SuspendDevice (pPenMount);

	return 0;
}

static int pmI2CIF_DriverResume(struct device *dev)
{
	struct i2c_client *pI2CClient = to_i2c_client(dev);
	struct strPENMOUNT *pPenMount = i2c_get_clientdata(pI2CClient);

	if (pPenMount->Model == PMDEVICE_MODEL_P3)
		pmI2CIF_P3_ResumeDevice (pPenMount);

	return 0;
}
#endif

static SIMPLE_DEV_PM_OPS(pmI2CIF_pm_ops,
		pmI2CIF_DriverSuspend, pmI2CIF_DriverResume);

MODULE_DEVICE_TABLE(i2c, PMI2CIF_DEVICES);

static const struct i2c_device_id PMI2CIF_DEVICES[] = {
	{ PMI2CIF_DRIVERNAME, 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, PMI2CIF_DEVICES);

#ifdef CONFIG_OF
static const struct of_device_id PMI2CIF_DEVICES_dt_ids[] = {
	{ .compatible = "penmount,pct30xx", },
	{ }
};
MODULE_DEVICE_TABLE(of, PMI2CIF_DEVICES_dt_ids);
#endif

static struct i2c_driver PMI2CIF_DRIVER = {
	.probe        = pmI2CIF_DriverProbe,
	.remove       = pmI2CIF_DriverRemove,
	.id_table     = PMI2CIF_DEVICES,
	.driver       = {
		.name = PMI2CIF_DRIVERNAME,
		.owner	= THIS_MODULE,
		.of_match_table = of_match_ptr(PMI2CIF_DEVICES_dt_ids),
		.pm	= &pmI2CIF_pm_ops,

		},
};

module_i2c_driver(PMI2CIF_DRIVER);

MODULE_AUTHOR("PenMount Touch Solutions <penmount@seed.net.tw>");
MODULE_DESCRIPTION("PenMount I2C Driver for PCT 30xx TouchScreens");
MODULE_LICENSE("GPL");
