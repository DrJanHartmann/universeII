/*
 *    Linux driver for Tundra universeII PCI to VME bridge, kernel 2.6.x
 *    Copyright (C) 2006 Andreas Ehmanns <universeII@gmx.de>
 * 
 *    This program is free software; you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation; either version 2 of the License, or
 *    (at your option) any later version.
 * 
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 * 
 *    You should have received a copy of the GNU General Public License
 *    along with this program; if not, write to the Free Software
 *    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <linux/version.h>

#include <linux/cdev.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/moduleparam.h>
#include <linux/spinlock.h>

#include <linux/poll.h>
#include <linux/seq_file.h>
#include <linux/proc_fs.h>
#include <linux/delay.h>
#include <linux/sched.h>

#include "universeII.h"
#include "vmeioctl.h"

MODULE_DESCRIPTION("VME driver for the Tundra Universe II PCI to VME bridge");
MODULE_AUTHOR("Andreas Ehmanns <universeII@gmx.de>, Jan Hartmann <hartmann@hiskp.uni-bonn.de");
MODULE_LICENSE("GPL");

static const char Version[] = "0.98 (July 2023)";

#define VMIC
#ifdef VMIC
#include "vmic.h"
#endif

//----------------------------------------------------------------------------
// Module parameters
//----------------------------------------------------------------------------

static int sys_ctrl = 1;
module_param(sys_ctrl, int, 0);
MODULE_PARM_DESC(sys_ctrl, " Set to 1 to enable VME system controller (default)");

static int br_level = 3;
module_param(br_level, int, 0);
MODULE_PARM_DESC(br_level, " VMEBus request level (default is BR3)");

static int req_mode = 0;
module_param(req_mode, int, 0);
MODULE_PARM_DESC(req_mode, " Request mode. Default: demand");

static int rel_mode = 0;
module_param(rel_mode, int, 0);
MODULE_PARM_DESC(rel_mode, " Release mode. Default: Release when done (RWD)");

static int vrai_bs = 0;
module_param(vrai_bs, int, 0);
MODULE_PARM_DESC(vrai_bs, "  Enable VMEBus access to universeII registers. Default: Disabled");

static int vbto = 3;
module_param(vbto, int, 0);
MODULE_PARM_DESC(vbto, "     VMEBus Time-out");

static int varb = 0;
module_param(varb, int, 0);
MODULE_PARM_DESC(varb, "     VMEBus Arbitration Mode");

static int varbto = 1;
module_param(varbto, int, 0);
MODULE_PARM_DESC(varbto, "   VMEBus Arbitration Time-out");

static int img_ovl = 1;
module_param(img_ovl, int, 0);
MODULE_PARM_DESC(img_ovl, "  Set to 0 to forbid overlapping images. Default: Allowed");

//----------------------------------------------------------------------------
// Prototypes
//----------------------------------------------------------------------------

static int universeII_open(struct inode*, struct file*);
static int universeII_release(struct inode*, struct file*);
static ssize_t universeII_read(struct file *, char __user *, size_t, loff_t *);
static ssize_t universeII_write(struct file *, const char __user *, size_t, loff_t *);
static long universeII_ioctl(struct file*, unsigned int, unsigned long);
static int universeII_mmap(struct file*, struct vm_area_struct*);

/*
 * _/_/_/_/_/_/_/_/_/_/_/_/_/_/_/_/_/_/_/_/_/_/_/_/
 * _/                                            _/
 * _/  Types and Constants                       _/
 * _/                                            _/
 * _/_/_/_/_/_/_/_/_/_/_/_/_/_/_/_/_/_/_/_/_/_/_/_/
 */

static struct file_operations universeII_fops = {
    .owner = THIS_MODULE,
    .open = universeII_open,
    .release = universeII_release,
    .read = universeII_read,
    .write = universeII_write,
    .unlocked_ioctl = universeII_ioctl,
    .mmap = universeII_mmap
};

static struct cdev *universeII_cdev; /* Character device */
static struct class *universeII_sysfs_class;

static char pci_driver_name[] = "universeII";
static const char driver_name[] = "universeII";

static const int aCTL[18] = { LSI0_CTL, LSI1_CTL, LSI2_CTL, LSI3_CTL,
LSI4_CTL, LSI5_CTL, LSI6_CTL, LSI7_CTL, 0, 0,
VSI0_CTL, VSI1_CTL, VSI2_CTL, VSI3_CTL,
VSI4_CTL, VSI5_CTL, VSI6_CTL, VSI7_CTL };

static const int aBS[18] = { LSI0_BS, LSI1_BS, LSI2_BS, LSI3_BS,
LSI4_BS, LSI5_BS, LSI6_BS, LSI7_BS, 0, 0,
VSI0_BS, VSI1_BS, VSI2_BS, VSI3_BS,
VSI4_BS, VSI5_BS, VSI6_BS, VSI7_BS };

static const int aBD[18] = { LSI0_BD, LSI1_BD, LSI2_BD, LSI3_BD,
LSI4_BD, LSI5_BD, LSI6_BD, LSI7_BD, 0, 0,
VSI0_BD, VSI1_BD, VSI2_BD, VSI3_BD,
VSI4_BD, VSI5_BD, VSI6_BD, VSI7_BD };

static const int aTO[18] = { LSI0_TO, LSI1_TO, LSI2_TO, LSI3_TO,
LSI4_TO, LSI5_TO, LSI6_TO, LSI7_TO, 0, 0,
VSI0_TO, VSI1_TO, VSI2_TO, VSI3_TO,
VSI4_TO, VSI5_TO, VSI6_TO, VSI7_TO };

static const int aVIrq[7] = { V1_STATID, V2_STATID, V3_STATID, V4_STATID,
V5_STATID, V6_STATID, V7_STATID };

static const int mbx[4] = { MAILBOX0, MAILBOX1, MAILBOX2, MAILBOX3 };

/*
 * _/_/_/_/_/_/_/_/_/_/_/_/_/_/_/_/_/_/_/_/_/_/_/_/
 * _/                                            _/
 * _/  Vars and Defines                          _/
 * _/                                            _/
 * _/_/_/_/_/_/_/_/_/_/_/_/_/_/_/_/_/_/_/_/_/_/_/_/
 */

#define UNI_MAJOR   221
#define MAX_IMAGE     8
#define MAX_MINOR    17
#define CONTROL_MINOR 8
#define DMA_MINOR     9

#define PCI_BUF_SIZE  0x20000            // Size of one slave image buffer
#define DMA_ACTIVE_TIMEOUT HZ            // 1s is the maximum time the
// DMA is allowed to be active

static struct pci_dev *universeII_dev = NULL;
#ifdef VMIC
static struct pci_dev *vmic_dev = NULL;
#endif

// Tundra chip and image internal handling addresses

static void __iomem *baseaddr = 0;// Base address of Tundra chip

static void __iomem *dmaBuf = 0;// DMA buf address in kernel space
static dma_addr_t dmaHandle = 0;

static unsigned int dmaBufSize = 0;      // Size of one DMA buffer
static unsigned int dma_dctl;            // DCTL register for DMA
static int dma_in_use = 0;
static int dma_blt_berr = 0;            // for DMA BLT until BERR

// All image related information like start address, end address, ...
static image_desc_t image[18];

// Pointers to 256 available linked lists
static struct cpl cpLists[256];

// Interrupt information
static irq_device_t irq_device[7][256];

// Structure holds information about driver statistics (reads, writes, ...)
static driver_stats_t statistics;

// VMEBus interrupt wait queue
DECLARE_WAIT_QUEUE_HEAD( vmeWait);

// DMA timer and DMA wait queue
static struct timer_list DMA_timer;      // This is a timer for returning status
DECLARE_WAIT_QUEUE_HEAD( dmaWait);

// Mailbox information
static mbx_device_t mbx_device[4];

// A circular buffer for storing the last 32 VME BERR.
static vme_Berr_t vmeBerrList[32];       

// Spinlocks
static DEFINE_SPINLOCK( get_image_lock);
static DEFINE_SPINLOCK( set_image_lock);
static DEFINE_SPINLOCK( vme_lock);
static DEFINE_SPINLOCK( dma_lock);
static DEFINE_SPINLOCK( mbx_lock);

// Autoprobing 
static int __init universeII_init(void);
static int universeII_probe(struct pci_dev*, const struct pci_device_id*);
static void universeII_remove(struct pci_dev*);
static void __exit universeII_exit(void);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 0, 0)
static const struct pci_device_id universeII_ids[] = {
#else
static DEFINE_PCI_DEVICE_TABLE (universeII_ids) = {
#endif
    { PCI_DEVICE(PCI_VENDOR_ID_TUNDRA, PCI_DEVICE_ID_TUNDRA_CA91C042) },
    { },
};
MODULE_DEVICE_TABLE( pci, universeII_ids);

static struct pci_driver universeII_driver = {
    .name = pci_driver_name,
    .id_table = universeII_ids,
    .probe = universeII_probe,
    .remove = universeII_remove,
};


#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 9, 0)
#define file_inode(file) ((file)->f_dentry->d_inode)
#endif


/*
 * _/_/_/_/_/_/_/_/_/_/_/_/_/_/_/_/_/_/_/_/_/_/_/_/
 * _/                                            _/
 * _/  Functions                                 _/
 * _/                                            _/ 
 * _/_/_/_/_/_/_/_/_/_/_/_/_/_/_/_/_/_/_/_/_/_/_/_/
 */

//----------------------------------------------------------------------------
//
//  DMA_timeout
//
//----------------------------------------------------------------------------
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 14, 0)
static void DMA_timeout(unsigned long ptr)
#else
static void DMA_timeout(struct timer_list *t)
#endif
{
  wake_up_interruptible(&dmaWait);
  statistics.timeouts++;
}

//----------------------------------------------------------------------------
//
//  MBX_timeout
//
//----------------------------------------------------------------------------
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 14, 0)
static void MBX_timeout(unsigned long ptr)
#else
static void MBX_timeout(struct timer_list *t)
#endif
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 14, 0)
  mbx_device_t *mbx = &mbx_device[ptr];
#else
  mbx_device_t *mbx = from_timer(mbx, t, mbxTimer);
#endif
  mbx->timeout = 1;
  wake_up_interruptible(&mbx->mbxWait);
  statistics.timeouts++;
}

//----------------------------------------------------------------------------
//
//  VIRQ_timeout
//
//----------------------------------------------------------------------------
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 14, 0)
static void VIRQ_timeout(unsigned long ptr)
#else
static void VIRQ_timeout(struct timer_list *t)
#endif
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 14, 0)
  irq_device_t *irq_dev = &irq_device[ptr >> 8][ptr & 0xFF];
#else
  irq_device_t *irq_dev = from_timer(irq_dev, t, virqTimer);
#endif
  irq_dev->timeout = 1;
  wake_up_interruptible(&irq_dev->irqWait);
  statistics.timeouts++;
}

//----------------------------------------------------------------------------
//
//  irq_handler()
//
//----------------------------------------------------------------------------
static irqreturn_t irq_handler(int irq, void *dev_id)
{
  int i;
  u32 status, enable, statVme;

  enable = readl(baseaddr + LINT_EN);
  status = readl(baseaddr + LINT_STAT);

  status &= enable;        // check only irq sources that are enabled

  if (!status)             // we use shared ints, so we first check
    return IRQ_NONE;     // if this irq origins from universeII chip

  statistics.irqs++;

  // VMEbus interrupt

  if (status & 0x00FE)
  {
    for (i = 7; i > 0; i--)          // find which VME irq line is set
      if (status & (1 << i))
        break;

    if (i)
    {
      i--;
      statVme = readl(baseaddr + aVIrq[i]);   // read Status/ID byte
      if (statVme & 0x100)
      {
        printk("%s: VMEbus error during IACK cycle level %d, Stat/Id %d !\n", driver_name, i + 1, statVme & 0xff);
      }
      else
      {
        if (irq_device[i][statVme].ok)
        {
          if (irq_device[i][statVme].vmeAddrCl != 0)
            writel(irq_device[i][statVme].vmeValCl, irq_device[i][statVme].vmeAddrCl);
          wake_up_interruptible(&irq_device[i][statVme].irqWait);
        }
      }
      udelay(2);
    }
  }

  // DMA interrupt
  if (status & 0x0100)
    wake_up_interruptible(&dmaWait);

  // mailbox interrupt
  if (status & 0xF0000)
    for (i = 0; i < 4; i++)
      if (status & (0x10000 << i))
        wake_up_interruptible(&mbx_device[i].mbxWait);

  // IACK interrupt
  if (status & 0x1000)
    wake_up_interruptible(&vmeWait);

  // VMEBus error
  if (status & 0x0400)
  {
    statVme = readl(baseaddr + V_AMERR);
    if (statVme & 0x00800000)   // Check if error log is valid
    {
      if (statVme & 0x01000000)   // Check if multiple errors occured
      {
        printk("%s: Multiple VMEBus errors detected! "
            "Lost interrupt?\n", driver_name);
        vmeBerrList[statistics.berrs & 0x1F].merr = 1;
      }
      vmeBerrList[statistics.berrs & 0x1F].valid = 1;
      vmeBerrList[statistics.berrs & 0x1F].AM = (statVme >> 26) & 0x3f;
      vmeBerrList[statistics.berrs & 0x1F].address = readl(baseaddr + VAERR);
      statistics.berrs++;

      writel(0x00800000, baseaddr + V_AMERR);
    }
    else
      printk("%s: VMEBus error log invalid!\n", driver_name);
  }

  // other interrupt sources are (at the moment) not supported

  writel(status, baseaddr + LINT_STAT);   // Clear all pending irqs

  return IRQ_HANDLED;
}

//----------------------------------------------------------------------------
//
//  universeII_procinfo()
//
//----------------------------------------------------------------------------
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 10, 0)
static int universeII_procinfo(char *buf, char **start, off_t fpos, int lenght, int *eof, void *data)
#else
static int universeII_procinfo(struct seq_file *p, void *data)
#endif
{
  const char *const Axx[8] = { "A16", "A24", "A32", "Reserved", "Reserved", "CR/SCR", "User1", "User2" };
  const char *const Dxx[4] = { "D8", "D16", "D32", "D64" };

  int i, index;
  u32 ctl, bs, bd, to;

#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 10, 0)
  char *p = buf;
#define seq_printf p += sprintf
#endif

  seq_printf(p, "%s driver version %s\n", driver_name, Version);

  seq_printf(p, "  baseaddr = %p\n\n", baseaddr);

  if (vrai_bs != 0)
    seq_printf(p, "Access to universeII registers from VME at: "
        "0x%08x\n\n", vrai_bs);

  seq_printf(p, "  Status variables:          DMA: ");
  if (dma_in_use)
    seq_printf(p, "in use\n\n");
  else
    seq_printf(p, "free\n\n");

  seq_printf(p, "    reads      = %li\n    writes     = %li\n"
      "    ioctls     = %li\n    irqs       = %li\n"
      "    DMA errors = %li\n    timeouts   = %li \n\n",
      statistics.reads, statistics.writes, statistics.ioctls,
      statistics.irqs, statistics.dmaErrors, statistics.timeouts);

  seq_printf(p, "Allocated master images:\n");

  for (i = 0; i < 8; i++)
  {
    if (image[i].opened)
    {
      ctl = readl(baseaddr + aCTL[i]);
      bs = readl(baseaddr + aBS[i]);
      bd = readl(baseaddr + aBD[i]);
      to = readl(baseaddr + aTO[i]);

      seq_printf(p, "  Image %i:\n", i);
      seq_printf(p, "    Registers                VMEBus range\n");
      seq_printf(p, "    LSI%i_CTL = %08x        %s/%s\n", i, ctl,
          Axx[(ctl >> 16) & 0x7], Dxx[(ctl >> 22) & 0x3]);
      seq_printf(p, "    LSI%i_BS  = %08x\n", i, bs);
      seq_printf(p, "    LSI%i_BD  = %08x       %08x\n", i, bd,
          bs + to);
      seq_printf(p, "    LSI%i_TO  = %08x       %08x\n\n", i, to,
          bd + to);
    }
  }

  seq_printf(p, "Allocated slave images:\n");

  for (i = 10; i < 18; i++)
  {
    if (image[i].opened)
    {
      ctl = readl(baseaddr + aCTL[i]);
      bs = readl(baseaddr + aBS[i]);
      bd = readl(baseaddr + aBD[i]);
      to = readl(baseaddr + aTO[i]);

      seq_printf(p, "  Image %i:\n", i);
      seq_printf(p, "    Registers                VMEBus range\n");
      seq_printf(p, "    VSI%i_CTL = %08x          %s\n", i, ctl,
          Axx[(ctl >> 16) & 0x7]);
      seq_printf(p, "    VSI%i_BS  = %08x\n", i, bs);
      seq_printf(p, "    VSI%i_BD  = %08x       %08x\n", i, bd, bs);
      seq_printf(p, "    VSI%i_TO  = %08x       %08x\n\n", i, to, bd);
    }
  }

  seq_printf(p, "\nNumber of occured VMEBus errors: %li\n", statistics.berrs);

  if (statistics.berrs > 0)
  {
    seq_printf(p, "Showing last 32 BERRs (maximum)\n"
        " BERR address   AM code     MERR\n");
    for (i = 0; i < 32; i++)
    {
      index = (statistics.berrs - 31 + i) & 0x1F;
      if (vmeBerrList[index].valid)
        seq_printf(p, "   %08x       %02x         %01x\n",
            vmeBerrList[index].address, vmeBerrList[index].AM,
            vmeBerrList[index].merr);
    }
  }

#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 10, 0)
  *eof = 1;
  return p - buf;
#else
  return 0;
#endif
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 10, 0) && LINUX_VERSION_CODE < KERNEL_VERSION(4, 18, 0)
/*because proc_create_single is not yet available, we need to create a wrapper for single_open */
static int universeII_procopen(struct inode *inode_ptr, struct file *fp)
{
  return single_open(fp, universeII_procinfo, PDE_DATA(inode_ptr));
}

static struct file_operations universeII_procfops = {
  .owner = THIS_MODULE,
  .open = universeII_procopen,
  .read = seq_read,
  .llseek = seq_lseek,
  .release = single_release,
};
#endif


//----------------------------------------------------------------------------
//
//  register_proc()
//
//----------------------------------------------------------------------------
static void register_proc(void)
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 10, 0)
  create_proc_read_entry(driver_name, 0, NULL, universeII_procinfo, NULL);
#elif LINUX_VERSION_CODE < KERNEL_VERSION(4, 18, 0)
  proc_create(driver_name, 0, NULL, &universeII_procfops);
#else
  proc_create_single(driver_name, 0, NULL, universeII_procinfo);
#endif
}

//----------------------------------------------------------------------------
//
//  unregister_proc()
//
//----------------------------------------------------------------------------
static void unregister_proc(void)
{
  remove_proc_entry(driver_name, NULL);
}

//----------------------------------------------------------------------------
//
//  testAndClearBERR()
//
//----------------------------------------------------------------------------
static int testAndClearBERR(void)
{
  u32 tmp = readl(baseaddr + PCI_CSR);            // Check for a bus error

  if (tmp & 0x08000000)                           // S_TA is Set
  {
    writel(tmp, baseaddr + PCI_CSR);
    statistics.berrs++;
    return 1;
  }

  return 0;
}

//----------------------------------------------------------------------------
//
//  testAndClearDMAErrors()
//
//----------------------------------------------------------------------------
static int testAndClearDMAErrors(void)
{
  u32 tmp = readl(baseaddr + DGCS);

  if (!(tmp & 0x00000800))      // Check if DMA status is done
  {
    if (tmp & 0x00008000)
    {   // Check for timeout (i.e. ACT bit still set)
      printk("%s: DMA stopped with timeout. DGCS = %08x !\n", driver_name, tmp);
      writel(0x40000000, baseaddr + DGCS);    // Stop DMA
    }

    writel(0x00006F00, baseaddr + DGCS);    // Clear all errors and disable all DMA irqs
    statistics.dmaErrors++;
    return (tmp & 0x0000E700);
  }

  return 0;
}

//----------------------------------------------------------------------------
//
//  execDMA()
//
//----------------------------------------------------------------------------
static void execDMA(u32 chain)
{
  DEFINE_WAIT(wait);

  DMA_timer.expires = jiffies + DMA_ACTIVE_TIMEOUT;  // We need a timer to
  add_timer(&DMA_timer);                             // timeout DMA transfers

  prepare_to_wait(&dmaWait, &wait, TASK_INTERRUPTIBLE);
  writel(0x80006F0F | chain, baseaddr + DGCS);    // Start DMA, clear errors
  // and enable all DMA irqs
  schedule();                                     // Wait for DMA to finish

  del_timer(&DMA_timer);
  finish_wait(&dmaWait, &wait);
}

//----------------------------------------------------------------------------
//
//  universeII_read()
//
//----------------------------------------------------------------------------
static ssize_t universeII_read(struct file *file, char __user *buf,
    size_t count, loff_t *ppos)
{
  int i = 0, okcount = 0, offset = 0, berr = 0;
  unsigned int dw, pci = 0;
  char *temp = buf;
  int res=0;

  u8 vc;          // 8 bit transfers
  u16 vs;// 16 bit transfers
  u32 vi;// 32 bit transfers

  void __iomem *image_ptr;
  dma_param_t dmaParam;
  unsigned int minor = MINOR(file_inode(file)->i_rdev);

  statistics.reads++;
  switch (minor)
  {
    case CONTROL_MINOR:
    vi = readl(baseaddr + (*ppos & 0x0FFFFFFF));
    res = __copy_to_user(temp, &vi, 4);
    if(res)
    {
      printk("%s: Line %d  __copy_to_user returned %02d", driver_name, __LINE__, res);
      return -1;
    }
    break;

    case DMA_MINOR:
    res = __copy_from_user(&dmaParam, buf, sizeof(dmaParam));
    if(res)
    {
      printk("%s: Line %d  __copy_from_user returned %02d", driver_name, __LINE__, res);
      return -1;
    }
    if (dmaBufSize * dmaParam.bufNr + dmaParam.count > PCI_BUF_SIZE)
    {
      printk("%s: DMA operation exceeds DMA buffer size!", driver_name);
      return -1;
    }

    dma_dctl = dmaParam.dma_ctl | dmaParam.vas | dmaParam.vdw;
    pci = dmaHandle + dmaBufSize * dmaParam.bufNr;

    if ((pci < dmaHandle) || (pci + dmaParam.count > dmaHandle + PCI_BUF_SIZE))
      return -2;

    // Check that DMA is idle
    if (readl(baseaddr + DGCS) & 0x00008000)
    {
      printk("%s: DMA device is not idle!\n", driver_name);
      return 0;
    }

    writel(dma_dctl, baseaddr + DCTL);          // Setup Control Reg
    writel(dmaParam.count, baseaddr + DTBC);// Count
    writel(dmaParam.addr, baseaddr + DVA);// VME Address

    // lower 3 bits of VME and PCI address must be identical,
    if ((pci & 0x7) == (dmaParam.addr & 0x7))
      writel(pci, baseaddr + DLA);// PCI address
    else
    {
      offset = (((dmaParam.addr & 0x7) + 0x8) - (pci & 0x7)) & 0x7;
      writel(pci + offset, baseaddr + DLA);
    }

    execDMA(0);                          // Start and wait for DMA

    res = testAndClearDMAErrors();
    if (dma_blt_berr && (res == 0x200))
    {
      // DMA BLT until VME BERR is valild (but bad practice)
      // If we read something before the BERR, it's a success.
      if (dmaParam.count > readl(baseaddr + DTBC))
        res = 0;
    }
    if (res)
      okcount = -1;
    else
      okcount = offset;

    break;

    default:
    if (image[minor].okToWrite)
    {
      if ((*ppos & 0x0FFFFFFF) + count > image[minor].size)
      return -1;

      image_ptr = image[minor].vBase + (*ppos & 0x0FFFFFFF);

      dw = (*ppos >> 28) & 0xF;   // Data width 1, 2 or 4 byte(s)

      switch (dw)
      {
        case 1:
        for (i = 0; i < count; i++)
        {
          spin_lock(&vme_lock);
          vc = readb(image_ptr);
          berr = testAndClearBERR();  // Check for a bus error
          spin_unlock(&vme_lock);

          if (berr)
          return okcount;
          else
          okcount++;

          res = __copy_to_user(temp, &vc, 1);
          if(res)
          {
            printk("%s: Line %d  __copy_to_user returned %02d", driver_name, __LINE__, res);
            return -1;
          }

          image_ptr++;
          temp++;
        }
        break;

        case 2:
        count /= 2;                     // Calc number of words
        for (i = 0; i < count; i++)
        {
          spin_lock(&vme_lock);
          vs = readw(image_ptr);
          berr = testAndClearBERR();  // Check for a bus error
          spin_unlock(&vme_lock);

          if (berr)
            return okcount;
          else
            okcount += 2;

          res = __copy_to_user(temp, &vs, 2);
          if(res)
          {
            printk("%s: Line %d  __copy_to_user returned %02d", driver_name, __LINE__, res);
            return -1;
          }
          image_ptr += 2;
          temp += 2;
        }
        break;

        case 4:
        count /= 4;                     // Calc number of longs
        for (i = 0; i < count; i++)
        {
          spin_lock(&vme_lock);
          vi = readl(image_ptr);
          berr = testAndClearBERR();  // Check for a bus error
          spin_unlock(&vme_lock);

          if (berr)
            return okcount;
          else
            okcount += 4;

          res = __copy_to_user(temp, &vi, 4);
          if(res)
          {
            printk("%s: Line %d  __copy_to_user returned %02d", driver_name, __LINE__, res);
            return -1;
          }

          image_ptr += 4;
          temp += 4;
        }
        break;
      }       // of switch(dw)
    }       // of if (okToWrite)
    break;
  }           // switch(minor)

  *ppos += count;
  return okcount;
}

//----------------------------------------------------------------------------
//
//  universeII_write()
//
//----------------------------------------------------------------------------
static ssize_t universeII_write(struct file *file, const char __user *buf,
    size_t count, loff_t *ppos)
{
  int i = 0, okcount = 0, offset = 0, berr = 0, res = 0;
  unsigned int dw, pci = 0;
  char *temp = (char *) buf;

  u8 vc;          // 8 bit transfers
  u16 vs;// 16 bit transfers
  u32 vi;// 32 bit transfers

  void __iomem *image_ptr;
  dma_param_t dmaParam;
  unsigned int minor = MINOR(file_inode(file)->i_rdev);

  statistics.writes++;
  switch (minor)
  {
    case CONTROL_MINOR:
    res = __copy_from_user(&vi, temp, 4);
    if(res)
    {
      printk("%s: Line %d  __copy_from_user returned %02d", driver_name, __LINE__, res);
      return -1;
    }
    writel(vi, baseaddr + (*ppos & 0x0FFFFFFF));
    break;

    case DMA_MINOR:
    res = __copy_from_user(&dmaParam, buf, sizeof(dmaParam));
    if(res)
    {
      printk("%s: Line %d  __copy_from_user returned %02d", driver_name, __LINE__, res);
      return -1;
    }
    if (dmaBufSize * dmaParam.bufNr + dmaParam.count > PCI_BUF_SIZE)
    {
      printk("%s: DMA operation exceeds DMA buffer size!", driver_name);
      return -1;
    }

    dma_dctl = dmaParam.dma_ctl | dmaParam.vas | dmaParam.vdw;
    pci = dmaHandle + dmaBufSize * dmaParam.bufNr;

    if ((pci < dmaHandle) || (pci + dmaParam.count > dmaHandle + PCI_BUF_SIZE))
      return -2;

    // Check that DMA is idle
    if (readl(baseaddr + DGCS) & 0x00008000)
    {
      printk("%s: DMA device is not idle!\n", driver_name);
      return 0;
    }

    writel(0x80000000 | dma_dctl, baseaddr + DCTL);  // Setup Control Reg
    writel(dmaParam.count, baseaddr + DTBC);// Count
    writel(dmaParam.addr, baseaddr + DVA);// VME address

    // lower 3 bits of VME and PCI address must be identical,
    if ((pci & 0x7) == (dmaParam.addr & 0x7))
    {
      writel(pci, baseaddr + DLA);// PCI address
    }
    else
    {
      offset = (((dmaParam.addr & 0x7) + 0x8) - (pci & 0x7)) & 0x7;
      writel(pci + offset, baseaddr + DLA);
    }

    execDMA(0);                          // Start and wait for DMA

    if (testAndClearDMAErrors())// Check for DMA errors
      okcount = -1;
    else
      okcount = offset;

    break;

    default:
    if (image[minor].okToWrite)
    {
      if ((*ppos & 0x0FFFFFFF) + count > image[minor].size)
        return -1;

      image_ptr = image[minor].vBase + (*ppos & 0x0FFFFFFF);

      dw = (*ppos >> 28) & 0xF;        // Data width 1, 2 or 4 byte(s)

      switch (dw)
      {
        case 1:
        for (i = 0; i < count; i++)
        {
          res = __copy_from_user(&vc, temp, 1);
          if(res)
          {
            printk("%s: Line %d  __copy_from_user returned %02d", driver_name, __LINE__, res);
            return -1;
          }
          spin_lock(&vme_lock);
          writeb(vc, image_ptr);
          berr = testAndClearBERR();  // Check for a bus error
          spin_unlock(&vme_lock);

          if (berr)
            return okcount;
          else
            okcount++;

          image_ptr++;
          temp++;
        }
        break;

        case 2:
        count /= 2;                     // Calc number of words
        for (i = 0; i < count; i++)
        {
          res = __copy_from_user(&vs, temp, 2);
          if(res)
          {
            printk("%s: Line %d  __copy_from_user returned %02d", driver_name, __LINE__, res);
            return -1;
          }
          spin_lock(&vme_lock);
          writew(vs, image_ptr);
          berr = testAndClearBERR();  // Check for a bus error
          spin_unlock(&vme_lock);

          if (berr)
            return okcount;
          else
            okcount += 2;

          image_ptr += 2;
          temp += 2;
        }
        break;

        case 4:
        count /= 4;                     // Calc number of longs
        for (i = 0; i < count; i++)
        {
          res = __copy_from_user(&vi, temp, 4);
          if(res)
          {
            printk("%s: Line %d  __copy_from_user returned %02d", driver_name, __LINE__, res);
            return -1;
          }
          spin_lock(&vme_lock);
          writel(vi, image_ptr);
          berr = testAndClearBERR();  // Check for a bus error
          spin_unlock(&vme_lock);

          if (berr)
            return okcount;
          else
            okcount += 4;

          image_ptr += 4;
          temp += 4;
        }
        break;
      }       // of switch(dw)
    }       // of if (okToWrite)
    break;
  }           // switch(minor)

  *ppos += count;
  return okcount;
}

//----------------------------------------------------------------------------
//
//  universeII_mmap()
//
//----------------------------------------------------------------------------
static int universeII_mmap(struct file *file, struct vm_area_struct *vma)
{
  unsigned int minor = MINOR(file_inode(file)->i_rdev);
  image_desc_t *p;

  file->private_data = &image[minor];
  p = file->private_data;

  if (minor < MAX_IMAGE)
  {                     // master image
    if (vma->vm_end - vma->vm_start > p->size)
    {
      printk("%s mmap: INVALID, start at 0x%08lx end 0x%08lx, "
          "pstart 0x%08x, pend 0x%08x\n", driver_name, vma->vm_start,
          vma->vm_end, p->phys_start, p->phys_end);
      return -EINVAL;
    }

    vma->vm_pgoff = p->phys_start >> PAGE_SHIFT;
  }

  if (minor == DMA_MINOR)
  {                    // DMA
    if (vma->vm_end - vma->vm_start > PCI_BUF_SIZE)
    {
      printk("%s mmap: INVALID, start at 0x%08lx end "
          "0x%08lx\n", driver_name, vma->vm_start, vma->vm_end);
      return -EINVAL;
    }

    vma->vm_pgoff = dmaHandle >> PAGE_SHIFT;
  }

  if ((minor > 9) && (minor <= MAX_MINOR))
  {   // slave image
    if (vma->vm_end - vma->vm_start > PCI_BUF_SIZE)
    {
      printk("%s mmap: INVALID, start at 0x%08lx end "
          "0x%08lx\n", driver_name, vma->vm_start, vma->vm_end);
      return -EINVAL;
    }

    vma->vm_pgoff = p->buffer >> PAGE_SHIFT;
  }

  if ((minor == CONTROL_MINOR) || (minor > MAX_MINOR))
    return -EBADF;

  if (remap_pfn_range(vma, vma->vm_start, vma->vm_pgoff,
      vma->vm_end - vma->vm_start, vma->vm_page_prot) != 0)
  {
    printk("%s mmap: remap_pfn_range failed !\n", driver_name);
    return (-EAGAIN);
  }

  vma->vm_file = file;

  return 0;
}

//----------------------------------------------------------------------------
//
//  universeII_open()
//
//----------------------------------------------------------------------------
static int universeII_open(struct inode *inode, struct file *file)
{
  unsigned int minor = MINOR(inode->i_rdev);

  if (minor > MAX_MINOR)
    return (-ENODEV);

  switch (minor)
  {
  case CONTROL_MINOR:
    case DMA_MINOR:
    image[minor].opened++;
    return 0;
  }

  if (image[minor].opened != 1) // this images wasn't allocated by IOCTL_GET_IMAGE
    return (-EBUSY);

  image[minor].opened = 2;
  image[minor].buffer = 0;

  return 0;
}

//----------------------------------------------------------------------------
//
//  universeII_release()
//
//----------------------------------------------------------------------------
static int universeII_release(struct inode *inode, struct file *file)
{
  unsigned int minor = MINOR(inode->i_rdev);
  int i, j;

  if (image[minor].vBase != NULL)
  {
    iounmap(image[minor].vBase);
    image[minor].vBase = NULL;

    if (minor < MAX_IMAGE && image[minor].masterRes.start) // release pci mapping when master image
    {
      release_resource(&image[minor].masterRes);
      memset(&image[minor].masterRes, 0, sizeof(image[minor].masterRes));
    }
  }

  image[minor].opened = 0;
  image[minor].okToWrite = 0;
  image[minor].phys_start = 0;
  image[minor].phys_end = 0;
  image[minor].size = 0;

  if ((minor > 9) && (minor < 18))
  {    // Slave image
    image[minor].buffer = 0;
  }

  for (i = 0; i < 7; i++)               // make sure to free all VMEirq/Status
    for (j = 0; j < 256; j++)         // combinations of this image
      if (irq_device[i][j].ok == minor + 1)
        irq_device[i][j].ok = 0;

  return 0;
}

//----------------------------------------------------------------------------
//
//  universeII_ioctl()
//
//----------------------------------------------------------------------------
static long universeII_ioctl(struct file *file, unsigned int cmd,
    unsigned long arg)
{
  unsigned int minor = MINOR(file_inode(file)->i_rdev);
  unsigned int i = 0, res = 0;
  u32 ctl = 0, to = 0, bs = 0, bd = 0, imageStart, imageEnd;

  statistics.ioctls++;
  switch (cmd)
  {
  case IOCTL_SET_CTL:
    writel(arg, baseaddr + aCTL[minor]);
    break;

  case IOCTL_SET_OPT:
    if (arg & 0x10000000)
      writel(readl(baseaddr + aCTL[minor]) & ~arg,
          baseaddr + aCTL[minor]);
    else
      writel(readl(baseaddr + aCTL[minor]) | arg,
          baseaddr + aCTL[minor]);
    break;

  case IOCTL_SET_IMAGE:
  {
    unsigned int pciBase = 0;
    image_regs_t iRegs;

    res = copy_from_user(&iRegs, (char*) arg, sizeof(iRegs));
    if (res)
    {
      printk("%s: Line %d  copy_from_user returned %02d", driver_name, __LINE__, res);
      return -1;
    }
    if ((iRegs.ms < 0) || (iRegs.ms > 1))
      return -1;

    spin_lock(&set_image_lock);

    if (image[minor].opened != 2)
    {
      spin_unlock(&set_image_lock);
      printk("%s: Allocation of image %d conflicts with "
          "existing image!\n", driver_name, minor);
      return -2;  // the requested image seems to be already configured
    }

    if (!iRegs.ms)
    {   // master image
      image[minor].masterRes.name = pci_driver_name;
      image[minor].masterRes.start = 0;
      image[minor].masterRes.end = iRegs.size;
      image[minor].masterRes.flags = IORESOURCE_MEM;

      if (pci_bus_alloc_resource(universeII_dev->bus, &image[minor].masterRes, iRegs.size, 0x10000, PCIBIOS_MIN_MEM, 0, NULL, NULL))
      {
        spin_unlock(&set_image_lock);
        printk("%s: Not enough iomem found for "
            "requested image size!\n", driver_name);
        return -3;
      }
      pciBase = image[minor].masterRes.start;
    }
    else
    {    // slave image
      if (minor < 10)
      {
        spin_unlock(&set_image_lock);
        printk("%s: IOCTL_SET_IMAGE, Image %d is not "
            "a slave image!\n", driver_name, minor);
        return -4;
      }

      if (image[minor].slaveBuf == 0)
      {   // check if high memory is
        // available
        spin_unlock(&set_image_lock);
        printk("%s: IOCTL_SET_IMAGE, No memory for "
            "slave image available!\n", driver_name);
        return -5;
      }
    }

    // First we check if this image overlaps with existing ones
    if (img_ovl == 0)
    {
      for (i = 0; i < MAX_IMAGE; i++)
      {
        if ((image[i].opened == 2) && (i != minor))
        {
          imageStart = readl(baseaddr + aBS[i]) + readl(baseaddr + aTO[i]);
          imageEnd = readl(baseaddr + aBD[i]) + readl(baseaddr + aTO[i]);

          if (!((iRegs.base + iRegs.size <= imageStart)
              || (iRegs.base >= imageEnd)))
          {
            spin_unlock(&set_image_lock);
            if (!iRegs.ms)
            {
              release_resource(&image[minor].masterRes);
              memset(&image[minor].masterRes, 0, sizeof(image[minor].masterRes));
            }
            printk("%s: Overlap of image %d and %d !\n", driver_name,
                i, minor);
            printk("imageStart1 = %x, imageEnd1 = %x, "
                "imageStart2 = %x, imageEnd2 = %x !\n",
                iRegs.base, iRegs.base + iRegs.size,
                imageStart, imageEnd);
            return -6;  // overlap with existing image
          }
        }
      }
    }

    if (!iRegs.ms)
    {   // master image
      writel(pciBase, baseaddr + aBS[minor]);
      writel(pciBase + iRegs.size, baseaddr + aBD[minor]);
      writel(-pciBase + iRegs.base, baseaddr + aTO[minor]);
    }
    else
    {             // slave image
      writel(iRegs.base, baseaddr + aBS[minor]);
      writel(iRegs.base + iRegs.size, baseaddr + aBD[minor]);
      writel(image[minor].buffer - iRegs.base,
          baseaddr + aTO[minor]);
    }

    image[minor].okToWrite = 1;
    image[minor].opened = 3;

    spin_unlock(&set_image_lock);

    image[minor].phys_start = readl(baseaddr + aBS[minor]);
    image[minor].phys_end = readl(baseaddr + aBD[minor]);
    image[minor].size = image[minor].phys_end -
        image[minor].phys_start;

    if (image[minor].vBase != NULL)
      iounmap(image[minor].vBase);

    image[minor].vBase = ioremap(image[minor].phys_start, iRegs.size);
    if (!(image[minor].vBase))
    {
      image[minor].okToWrite = 0;
      image[minor].opened = 2;
      if (!iRegs.ms)
      {
        release_resource(&image[minor].masterRes);
        memset(&image[minor].masterRes, 0, sizeof(image[minor].masterRes));
      }
      printk("%s: IOCTL_SET_IMAGE, Error in ioremap!\n", driver_name);
      return -7;
    }

    break;
  }

  case IOCTL_GET_IMAGE:
  {
    unsigned int offset = 0;

    if ((arg < 0) || (arg > 1))
      return -1;

    if (arg)                       // slave image was requested
      offset = 10;

    i = 0;                         // look for a free image
    spin_lock(&get_image_lock);    // lock to prevent allocation of
    // same image
    while ((image[i + offset].opened) && (i < MAX_IMAGE))
      i++;

    if (i >= MAX_IMAGE)
    {
      spin_unlock(&get_image_lock);
      return -2;
    }
    else
    {
      image[i + offset].opened = 1;
      spin_unlock(&get_image_lock);
      return (i + offset);
    }

    break;
  }

  case IOCTL_GEN_VME_IRQ:
  {
    int level;

    DEFINE_WAIT(wait);

    if (arg & 0x01FFFFF8)          // unused bit is set
      return -1;

    writel(arg & 0xFE000000, baseaddr + STATID);
    level = 0x1000000 << (arg & 0x7);
    writel(~level & readl(baseaddr + VINT_EN), baseaddr + VINT_EN);

    prepare_to_wait(&vmeWait, &wait, TASK_INTERRUPTIBLE);

    writel(level | readl(baseaddr + VINT_EN), baseaddr + VINT_EN);

    schedule();

    finish_wait(&vmeWait, &wait);
    writel(~level & readl(baseaddr + VINT_EN), baseaddr + VINT_EN);

    break;
  }

  case IOCTL_SET_IRQ:
  {
    u32 base, toffset;
    void __iomem
    *virtAddr;
    int virq, vstatid;
    irq_setup_t isetup;

    res = copy_from_user(&isetup, (char*) arg, sizeof(isetup));
    if (res)
    {
      printk("%s: Line %d  __copy_from_user returned %02d", driver_name, __LINE__, res);
      return -1;
    }
    virq = isetup.vmeIrq - 1;
    vstatid = isetup.vmeStatus;

    if ((virq < 0) || (virq > 6) || (vstatid < 0) || (vstatid > 255))
    {
      printk("%s: IOCTL_SET_IRQ: Parameter out of range!\n", driver_name);
      return -1;
    }

    if (irq_device[virq][vstatid].ok)
    {
      printk("%s: IOCTL_SET_IRQ: irq/status combination is already in use!\n", driver_name);
      return -2;
    }

    toffset = readl(baseaddr + aTO[minor]);
    base = readl(baseaddr + aBS[minor]);

    if (isetup.vmeAddrSt != 0)
    {
      if ((isetup.vmeAddrSt - toffset < image[minor].phys_start) ||
          (isetup.vmeAddrSt - toffset > image[minor].phys_end))
        return -3;

      virtAddr = image[minor].vBase +
          (isetup.vmeAddrSt - toffset - base);
      irq_device[virq][vstatid].vmeAddrSt = virtAddr;
      irq_device[virq][vstatid].vmeValSt = isetup.vmeValSt;
    }
    else
      irq_device[virq][vstatid].vmeAddrSt = 0;

    if (isetup.vmeAddrCl != 0)
    {
      if ((isetup.vmeAddrCl - toffset < image[minor].phys_start) ||
          (isetup.vmeAddrCl - toffset > image[minor].phys_end))
        return -3;

      virtAddr = image[minor].vBase +
          (isetup.vmeAddrCl - toffset - base);

      irq_device[virq][vstatid].vmeAddrCl = virtAddr;
      irq_device[virq][vstatid].vmeValCl = isetup.vmeValCl;
    }
    else
      irq_device[virq][vstatid].vmeAddrCl = 0;

    init_waitqueue_head(&irq_device[virq][vstatid].irqWait);
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 14, 0)
    init_timer(&irq_device[virq][vstatid].virqTimer);
    irq_device[virq][vstatid].virqTimer.function = VIRQ_timeout;
    irq_device[virq][vstatid].virqTimer.data = (virq << 8) + vstatid;
#else
    timer_setup(&irq_device[virq][vstatid].virqTimer, VIRQ_timeout, 0);
#endif
    irq_device[virq][vstatid].ok = minor + 1;

    break;
  }

  case IOCTL_FREE_IRQ:
  {
    int virq, vstatid;
    irq_setup_t isetup;

    res = copy_from_user(&isetup, (char*) arg, sizeof(isetup));
    if (res)
    {
      printk("%s: Line %d  __copy_from_user returned %02d", driver_name, __LINE__, res);
      return -1;
    }
    virq = isetup.vmeIrq - 1;
    vstatid = isetup.vmeStatus;

    if ((virq < 0) || (virq > 6) || (vstatid < 0) || (vstatid > 255))
    {
      printk("%s: IOCTL_FREE_IRQ: Parameter out of range!\n", driver_name);
      return -1;
    }

    if (irq_device[virq][vstatid].ok == 0)
    {
      printk("%s: IOCTL_FREE_IRQ: irq/status combination not found!\n", driver_name);
      return -2;
    }

    irq_device[virq][vstatid].ok = 0;

    break;
  }

  case IOCTL_WAIT_IRQ:
  {
    int vmeIrq, vmeStatus;
    unsigned long timeout = 0;
    irq_wait_t irqData;
    struct timer_list *vTimer = NULL;

    DEFINE_WAIT(wait);

    res = copy_from_user(&irqData, (char*) arg, sizeof(irqData));
    if (res)
    {
      printk("%s: Line %d  __copy_from_user returned %02d", driver_name, __LINE__, res);
      return -1;
    }
    vmeIrq = irqData.irqLevel - 1;
    vmeStatus = irqData.statusID;

    if ((vmeIrq < 0) || (vmeIrq > 6) || (vmeStatus < 0) || (vmeStatus > 255))
    {
      printk("%s: IOCTL_WAIT_IRQ: Parameter out of range!\n", driver_name);
      return -1;
    }

    if (!irq_device[vmeIrq][vmeStatus].ok)
    {
      printk("%s: IOCTL_WAIT_IRQ: irq/status combination not found.\n", driver_name);
      return -1;
    }

    if (irqData.timeout > 0)
    {
      timeout = (irqData.timeout * HZ) / 1000;
      if (timeout == 0)
        timeout = 1;

      vTimer = &irq_device[vmeIrq][vmeStatus].virqTimer;
      vTimer->expires = jiffies + timeout;
      irq_device[vmeIrq][vmeStatus].timeout = 0;
    }

    prepare_to_wait(&irq_device[vmeIrq][vmeStatus].irqWait, &wait,
        TASK_INTERRUPTIBLE);
    if (irqData.timeout > 0)
      add_timer(vTimer);

    if (irq_device[vmeIrq][vmeStatus].vmeAddrSt != 0)
      writel(irq_device[vmeIrq][vmeStatus].vmeValSt,
          irq_device[vmeIrq][vmeStatus].vmeAddrSt);

    schedule();

    finish_wait(&irq_device[vmeIrq][vmeStatus].irqWait, &wait);
    if (irqData.timeout > 0)
    {
      del_timer(vTimer);
      if (irq_device[vmeIrq][vmeStatus].timeout)
        return -2;
    }

    break;
  }

  case IOCTL_SET_MBX:
  {
    u32 mbx_en;
    unsigned int mbxNr;

    mbxNr = 0x10000 << (arg & 0x3);

    spin_lock(&mbx_lock);               // lock access to mbx

    mbx_en = readl(baseaddr + LINT_EN);
    if (mbx_en & mbxNr)
    {    // mbx already in use
      spin_unlock(&mbx_lock);
      return -1;
    }

    writel(mbx_en | mbxNr, baseaddr + LINT_EN);

    spin_unlock(&mbx_lock);

    break;
  }

  case IOCTL_WAIT_MBX:
  {
    u32 lintEn;
    unsigned int mbxNr;

    DEFINE_WAIT(wait);

    mbxNr = arg & 0x3;

    lintEn = readl(baseaddr + LINT_EN);   // disable mailbox
    writel(lintEn & ~(0x10000 << mbxNr), baseaddr + LINT_EN);

    writel(0, baseaddr + mbx[mbxNr]);     // set mbx to 0
    writel(lintEn, baseaddr + LINT_EN);   // enable mailbox

    readl(baseaddr + LINT_EN);

    mbx_device[mbxNr].mbxTimer.expires = jiffies + (arg >> 16) * HZ;
    mbx_device[mbxNr].timeout = 0;
    add_timer(&mbx_device[mbxNr].mbxTimer);

    prepare_to_wait(&mbx_device[mbxNr].mbxWait, &wait, TASK_INTERRUPTIBLE);
    if (readl(baseaddr + LINT_STAT) & ~(0x10000 << mbxNr))
    {
      finish_wait(&mbx_device[mbxNr].mbxWait, &wait);
      printk("%s: previous mailbox interrupt detected!\n", driver_name);
    }
    else
    {
      schedule();                       // Wait for mbx interrupt
      finish_wait(&mbx_device[mbxNr].mbxWait, &wait);
    }

    del_timer(&mbx_device[mbxNr].mbxTimer);

    if (mbx_device[mbxNr].timeout)
      return -1;

    return readl(baseaddr + mbx[mbxNr]);

    break;
  }

  case IOCTL_RELEASE_MBX:
  {
    u32 lintEn;
    unsigned int mbxNr;

    mbxNr = 0x10000 << (arg & 0x3);

    spin_lock(&mbx_lock);

    lintEn = readl(baseaddr + LINT_EN); // check if mbx is enabled
    if ((lintEn & mbxNr) == 0)
    {
      spin_unlock(&mbx_lock);
      return -1;
    }

    writel(lintEn & ~mbxNr, baseaddr + LINT_EN);

    spin_unlock(&mbx_lock);
    break;
  }

  case IOCTL_NEW_DCP:
  {
    for (i = 0; i < 256; i++)   // find a free list
      if (cpLists[i].free)
        break;

    if (i > 255)                // can't create more lists
      return -1;

    cpLists[i].free = 0;        // mark list as not free
    return i;

    break;
  }

  case IOCTL_ADD_DCP:
  {
    unsigned int dla, offset;
    list_packet_t lpacket;
    struct kcp *newP, *ptr;

    res = copy_from_user(&lpacket, (char*) arg, sizeof(lpacket));
    if (res)
    {
      printk("%s: Line %d  __copy_from_user returned %02d", driver_name, __LINE__, res);
      return -1;
    }
    newP = kmalloc(sizeof(*newP), GFP_KERNEL | GFP_DMA);

    ptr = cpLists[lpacket.list].commandPacket;
    if (ptr == NULL)
    {
      cpLists[lpacket.list].commandPacket = newP;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 0, 0)
      cpLists[lpacket.list].start = dma_map_single(&universeII_dev->dev, &(newP->dcp.dctl), sizeof(*newP), DMA_BIDIRECTIONAL);
#else
      cpLists[lpacket.list].start = pci_map_single(universeII_dev, &(newP->dcp.dctl), sizeof(*newP), DMA_BIDIRECTIONAL);
#endif
    }
    else
    {
      while (ptr->next != NULL)     // find end of list
        ptr = ptr->next;
      ptr->next = newP;              // append new command packet
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 0, 0)
      ptr->dcp.dcpp = dma_map_single(&universeII_dev->dev, &(newP->dcp.dctl), sizeof(*newP), DMA_BIDIRECTIONAL);
#else
      ptr->dcp.dcpp = pci_map_single(universeII_dev, &(newP->dcp.dctl), sizeof(*newP), DMA_BIDIRECTIONAL);
#endif

      if (ptr->dcp.dcpp & 0x0000001F)
      {
        printk("%s: last 5 bits of dcpp != 0. dcpp "
            "is: %08x !\n", driver_name, ptr->dcp.dcpp);
        kfree(newP);
        return -1;
      }

      ptr->dcp.dcpp &= 0xFFFFFFFE;   // clear end bit
    }

    // fill newP command packet
    newP->next = NULL;
    newP->dcp.dctl = lpacket.dctl;   // control register
    newP->dcp.dtbc = lpacket.dtbc;   // number of bytes to transfer
    newP->dcp.dva = lpacket.dva;     // VMEBus address
    newP->dcp.dcpp = 0x00000001;     // last packet in list

    // last three bits of PCI and VME address MUST be identical!

    if (ptr == NULL)                 // calculate offset
      dla = dmaHandle;
    else
      dla = ptr->pciStart + ptr->dcp.dtbc;

    offset = (((lpacket.dva & 0x7) + 0x8) - (dla & 0x7)) & 0x7;

    if (dla + offset + lpacket.dtbc > dmaHandle + PCI_BUF_SIZE)
    {
      ptr->next = NULL;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 0, 0)
      dma_unmap_single(&universeII_dev->dev, ptr->dcp.dcpp, sizeof(*newP), DMA_BIDIRECTIONAL);
#else
      pci_unmap_single(universeII_dev, ptr->dcp.dcpp, sizeof(*newP), DMA_BIDIRECTIONAL);
#endif
      ptr->dcp.dcpp = 0x00000001;
      kfree(newP);
      printk("%s: DMA linked list packet exceeds global DMA "
          "buffer size!", driver_name);
      return -1;
    }

    newP->dcp.dla = dla + offset;    // PCI address
    newP->pciStart = dla + offset;

    return offset;
    break;
  }

  case IOCTL_EXEC_DCP:
  {
    int n = 0;
    u32 val;
    struct kcp *scan;

    // Check that DMA is idle
    val = readl(baseaddr + DGCS);
    if (val & 0x00008000)
    {
      printk("%s: Can't execute list %ld! DMA status = "
          "%08x!\n", driver_name, arg, val);
      return -1;
    }

    writel(0, baseaddr + DTBC);              // clear DTBC register
    writel(cpLists[arg].start, baseaddr + DCPP);

    execDMA(0x08000000);                     // Enable chained mode

    if (testAndClearDMAErrors())             // Check for DMA errors
      return -2;

    // Check that all command packets have been processed properly

    scan = cpLists[arg].commandPacket;
    while (scan != NULL)
    {
      n++;
      if (!(scan->dcp.dcpp & 0x00000002))
      {
        printk("%s: Processed bit of packet number "
            "%d is not set!\n", driver_name, n);
        return n;
      }
      scan = scan->next;
    }

    break;
  }

  case IOCTL_DEL_DCL:
  {
    struct kcp *del, *search;

    search = cpLists[arg].commandPacket;
    while (search != NULL)
    {
      del = search;
      search = search->next;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 0, 0)
      dma_unmap_single(&universeII_dev->dev, del->dcp.dcpp, sizeof(*del), DMA_BIDIRECTIONAL);
#else
      pci_unmap_single(universeII_dev, del->dcp.dcpp, sizeof(*del), DMA_BIDIRECTIONAL);
#endif
      kfree(del);
    }
    cpLists[arg].commandPacket = NULL;
    cpLists[arg].free = 1;

    break;
  }

  case IOCTL_TEST_ADDR:
  {
    void __iomem
    *virtAddr;
    int berr;
    there_data_t there;

    res = __copy_from_user(&there, (char*) arg, sizeof(there));
    if (res)
    {
      printk("%s: Line %d  __copy_from_user returned %02d", driver_name, __LINE__, res);
      return -1;
    }
    for (i = 0; i < MAX_IMAGE; i++)       // Find image that covers
      // address
      if (image[i].opened)
      {
        ctl = readl(baseaddr + aCTL[i]);
        bs = readl(baseaddr + aBS[i]);
        bd = readl(baseaddr + aBD[i]);
        to = readl(baseaddr + aTO[i]);
        if ((there.addr >= bs + to) && (there.addr < bd + to))
          break;
      }
    if (i == MAX_IMAGE)          // no image for this address found
      return -1;

    virtAddr = image[i].vBase + (there.addr - to - bs);

    spin_lock(&vme_lock);

    if (testAndClearBERR())
      printk("%s: Resetting previous uncleared bus error!\n", driver_name);

    if (there.mode != 1)
      ctl = there.mode;

    switch (ctl & 0x00C00000)
    {
    case 0:
      readb(virtAddr);
      break;
    case 0x00400000:
      readw(virtAddr);
      break;
    case 0x00800000:
      readl(virtAddr);
      break;
    default:
      spin_unlock(&vme_lock);
      return -2; // D64 is only supported for block transfers
    }

    berr = testAndClearBERR();
    spin_unlock(&vme_lock);

    return !berr;
    break;
  }

  case IOCTL_TEST_BERR:
  {
    int berr;

    spin_lock(&vme_lock);
    berr = testAndClearBERR();
    spin_unlock(&vme_lock);

    return berr;
    break;
  }

  case IOCTL_REQUEST_DMA:
  {
    int code = 0;

    spin_lock(&dma_lock);   // set spinlock to protect "dma_in_use"
    if ((dma_in_use) || (!dmaBuf))
      code = 0;
    else
    {
      if (arg)
        dmaBufSize = PCI_BUF_SIZE / arg; // Divide DMA buf in
      // multiple blocks
      else
        dmaBufSize = 0;
      dma_in_use = 1;
      code = 1;
    }
    spin_unlock(&dma_lock);
    return code;
    break;
  }

  case IOCTL_RELEASE_DMA:
  {
    dma_in_use = 0;
    dma_blt_berr = 0;
    break;
  }

  case IOCTL_DMA_BLT_BERR:
  {
    dma_blt_berr = 1;
    break;
  }

  case IOCTL_VMESYSRST:
  {
    writel(readl(baseaddr + MISC_CTL) | 0x400000, baseaddr + MISC_CTL);
    printk("%s: VME SYSRST initiated!\n", driver_name);
    break;
  }

  case IOCTL_RESET_ALL:
  {
    int j, error = 0;
    u32 csr;
    struct kcp *del, *search;

    printk("%s: General driver reset requested by user!", driver_name);

    // clear all previous PCI errors

    csr = readl(baseaddr + PCI_CSR);
    writel(0xF9000000 | csr, baseaddr + PCI_CSR);

    // clear and release DMA

    if (dma_in_use)
    {
      writel(0x40000000, baseaddr + DGCS);  // stop DMA
      udelay(100);
      if (readl(baseaddr + DGCS) & 0x8000)  // DMA still active?
        error = -1;

      writel(0x00006F00, baseaddr + DGCS);  // clear all previous
      // errors and
      // disable DMA irqs
      dma_in_use = 0;
      dma_blt_berr = 0;
    }

    // remove all existing command packet lists

    for (i = 0; i < 256; i++)
      if (cpLists[i].free == 0)
      {
        search = cpLists[arg].commandPacket;
        cpLists[arg].commandPacket = NULL;
        cpLists[arg].free = 1;
        while (search != NULL)
        {
          del = search;
          search = search->next;
          kfree(del);
        }
      }

    // remove all irq setups

    for (i = 0; i < 7; i++)
      for (j = 0; j < 256; j++)
        irq_device[i][j].ok = 0;

    // free all mailboxes by disabling MBX irq

    writel(0x000005FE, baseaddr + LINT_EN);

    // free all images

    for (i = 0; i < MAX_IMAGE; i++)
    {
      writel(0x00800000, baseaddr + aCTL[i]);
      writel(0x00800000, baseaddr + aCTL[i + 10]);

      if (image[i].vBase != NULL)
      {
        iounmap(image[i].vBase);
        image[i].vBase = NULL;

        if (image[i].masterRes.start)
        {
          release_resource(&image[i].masterRes);
          memset(&image[i].masterRes, 0, sizeof(image[i].masterRes));
        }
      }

      image[i].opened = 0;
      image[i].okToWrite = 0;
    }

    // reset all counters

    statistics.reads = 0;
    statistics.writes = 0;
    statistics.ioctls = 0;
    statistics.irqs = 0;
    statistics.berrs = 0;
    statistics.dmaErrors = 0;
    statistics.timeouts = 0;

    return error;

    break;
  }

  default:
    return -ENOIOCTLCMD;
  }

  return 0;
}

//----------------------------------------------------------------------------
//
//  cleanup_module()
//
//----------------------------------------------------------------------------

static void __exit universeII_exit(void)
{
  pci_unregister_driver(&universeII_driver);
}

static void universeII_remove(struct pci_dev *pdev)
{
  int i;
  void __iomem
  *virtAddr;
  struct page *page;

  writel(0, baseaddr + LINT_EN);  // Turn off Ints
  //pcivector = readl(baseaddr + PCI_MISC1) & 0x000000FF;
  free_irq(universeII_dev->irq, universeII_dev);   // Free Vector

  for (i = 1; i < MAX_IMAGE + 1; i++)
    if (image[i].vBase != NULL)
      iounmap(image[i].vBase);

  if (baseaddr != 0)
  {
    pci_release_regions(universeII_dev);
    iounmap(baseaddr);
  }
#ifdef VMIC
  if (vmic_dev != NULL)
    pci_release_regions(vmic_dev);
#endif

  unregister_proc();
  unregister_chrdev(UNI_MAJOR, driver_name);

  for (i = 10; i < 18; i++)
  {
    if (image[i].buffer)
    {
      virtAddr = image[i].slaveBuf;
      for (page = virt_to_page(virtAddr);
          page < virt_to_page(virtAddr + PCI_BUF_SIZE); ++page)
        ClearPageReserved(page);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 0, 0)
      dma_free_coherent(&universeII_dev->dev, PCI_BUF_SIZE, virtAddr, image[i].buffer);
#else
      pci_free_consistent(universeII_dev, PCI_BUF_SIZE, virtAddr, image[i].buffer);
#endif
    }
  }

  if (dmaHandle)
  {
    virtAddr = dmaBuf;
    for (page = virt_to_page(virtAddr);
        page < virt_to_page(virtAddr + PCI_BUF_SIZE); ++page)
      ClearPageReserved(page);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 0, 0)
    dma_free_coherent(&universeII_dev->dev, PCI_BUF_SIZE, virtAddr, dmaHandle);
#else
    pci_free_consistent(universeII_dev, PCI_BUF_SIZE, virtAddr, dmaHandle);
#endif
  }
  // Clean Device Tree
  for (i = 17; i >= 0; i--)
    device_destroy(universeII_sysfs_class, MKDEV(UNI_MAJOR, i));
  class_destroy(universeII_sysfs_class);
  /* Unregister device driver */
  cdev_del(universeII_cdev);

  /* Unregiser the major and minor device numbers */
  unregister_chrdev_region(MKDEV(UNI_MAJOR, 0), MAX_MINOR + 1);

  printk("%s driver removed!\n", driver_name);
}

//----------------------------------------------------------------------------
//
//  init_module()
//
//----------------------------------------------------------------------------

static int __init universeII_init(void)
{
  return pci_register_driver(&universeII_driver);
}

static int universeII_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
  u32 ba, temp, status, misc_ctl, mast_ctl, vrai_ctl, pci_csr;
  int i, j, result, err, num;
  void __iomem
  *virtAddr;
  struct page *page;
  char name[12];
  int retval;

  printk("%s driver version %s\n", driver_name, Version);

  universeII_dev = pdev;

  retval = pci_enable_device(universeII_dev);
  if (retval)
  {
    dev_err(&pdev->dev, "%s: Unable to enable device\n", driver_name);
    return -1;
  }
  if (!universeII_dev)
  {
    printk("%s: device not found!\n", driver_name);
    return -1;
  }

  pci_write_config_dword(universeII_dev, PCI_MISC0, 0); // Turn latency off

  printk("UniverseII found at bus %x device %x\n",
      universeII_dev->bus->number, universeII_dev->devfn);

  pci_read_config_dword(universeII_dev, PCI_CSR, &status);
  printk(" Vendor = %04X Device = %04X Status = %08X",
      universeII_dev->vendor, universeII_dev->device, status);
  printk("  Class = %08X\n", universeII_dev->class);

  pci_read_config_dword(universeII_dev, PCI_MISC0, &temp);
  printk("  Misc0 = %08X\n", temp);

  // Setup Universe Config Space
  // This is a 4k wide memory area that needs to be mapped into the
  // kernel virtual memory space so we can access it.
  // Note: even though we only map te first BAR, we need to request all BARs!
  //       otherwise, those addresses might be used for the master images
  if (pci_request_regions(universeII_dev, driver_name))
  {
    printk("%s: Could not read PCI base adress register from UniverseII config space\n", driver_name);
    return -2;
  }
  ba = pci_resource_start(universeII_dev, 0); //BAR 0 is the BS register at PCI_BS
  baseaddr = (void __iomem *) ioremap(ba, 4096);
  if (!baseaddr)
  {
    pci_release_regions(universeII_dev);
    printk("%s: Ioremap failed to map UniverseII to kernel space.\n", driver_name);
    return -2;
  }

  // Check to see if the mapping worked out

  if (readl(baseaddr) != 0x000010E3)
  {
    iounmap(baseaddr);
    pci_release_regions(universeII_dev);
    baseaddr = 0;
    printk("UniverseII chip failed to return PCI_ID in memory map.\n");
    return -3;
  }

  // Set universe II to be VMEbus system controller
  // (set module option sys_ctrl=0 to disable system controller)

  misc_ctl = readl(baseaddr + MISC_CTL);
  if (sys_ctrl)
  {
    misc_ctl |= 0x00020000;

    if ((vbto < 0) || (vbto > 7))
      printk("%s: Invalid VMEBus Timeout-out value: %d, "
          "ignoring!\n", driver_name, vbto);
    else
      misc_ctl |= (vbto & 0x7) << 28;

    if ((varb < 0) || (varb > 1))
      printk("%s: Invalid VMEBus Arbitration Mode: %d, "
          "ignoring!\n", driver_name, varb);
    else if (varb)
      misc_ctl |= 0x04000000;
    else
      misc_ctl &= 0xFBFFFFFF;

    if ((varbto < 0) || (varbto > 2))
      printk("%s: Invalid VMEBus Arbitration Timeout-out "
          "value: %d, ignoring!\n", driver_name, varbto);
    else
      misc_ctl |= (varbto & 0x3) << 24;
  }
  else
  {
    misc_ctl &= 0xFFFDFFFF;
    printk("%s: VMEBus system controller disabled !\n", driver_name);
  }

  writel(misc_ctl, baseaddr + MISC_CTL);
  printk("%s: MISC_CTL is %08x\n", driver_name, readl(baseaddr + MISC_CTL));

  mast_ctl = readl(baseaddr + MAST_CTL);

  if ((br_level < 0) || (br_level > 3))
    printk("%s: Invalid VME BR level: %d, ignoring!\n", driver_name, br_level);
  else if (br_level != 3)
  {
    mast_ctl &= 0xFF3FFFFF;
    mast_ctl |= br_level << 22;
  }

  if ((req_mode < 0) || (req_mode > 1))
    printk("%s: Invalid VMEBus request mode: %d, ignoring!\n", driver_name,
        req_mode);
  else if (req_mode)
    mast_ctl |= 0x00200000;
  else
    mast_ctl &= 0xFFDFFFFF;

  if ((rel_mode < 0) || (rel_mode > 1))
    printk("%s: Invalid VMEBus release mode: %d, ignoring!\n", driver_name,
        rel_mode);
  else if (rel_mode)
    mast_ctl |= 0x00100000;
  else
    mast_ctl &= 0xFFEFFFFF;

  writel(mast_ctl, baseaddr + MAST_CTL);
  printk("%s: MAST_CTL is %08x\n", driver_name, readl(baseaddr + MAST_CTL));

  // Setup access to universeII registers via VME if desired by option

  if (vrai_bs != 0)
  {
    if (vrai_bs & 0x00000FFF)          // lower 12 bits must be zero
      printk("%s: Ignoring invalid vrai_bs %08x!\n", driver_name, vrai_bs);
    else
    {
      vrai_ctl = 0x80F00000;
      if (vrai_bs & 0xFF000000)
        vrai_ctl |= 0x00020000;
      else if (vrai_bs & 0x00FF0000)
        vrai_ctl |= 0x00010000;

      writel(vrai_ctl, baseaddr + VRAI_CTL);
      writel(vrai_bs, baseaddr + VRAI_BS);
      printk("%s: Enabling VME access to regs from addr. "
          "%08x\n", driver_name, vrai_bs);
    }
  }

#ifdef VMIC
  // Enable byte-lane-swapping for master and slave images and VMEbus
  // access which is disabled by default!!!

  if ((vmic_dev = pci_get_device(VMIC_VEND_ID, VMIC_FPGA_DEVICE_ID1, NULL)) ||
      (vmic_dev = pci_get_device(VMIC_VEND_ID, VMIC_FPGA_DEVICE_ID2, NULL)) ||
      (vmic_dev = pci_get_device(VMIC_VEND_ID, VMIC_FPGA_DEVICE_ID3, NULL)))
  {

    printk("%s: VMIC subsystem ID: %x\n", driver_name, vmic_dev->subsystem_device);

    // Note: even though we only map te first BAR, we need to request all BARs!
    //       otherwise, those addresses might be used for the master images
    if (pci_request_regions(vmic_dev, driver_name))
      printk("%s: Could not read PCI base adress register from VMIC config cpace\n", driver_name);
    else
    {
      unsigned int VmicBase;
      static void __iomem *VmicBaseAddr;

      VmicBase = pci_resource_start(vmic_dev, 0);
      VmicBaseAddr = ioremap(VmicBase, PAGE_SIZE);

      if (VmicBaseAddr == NULL)
      {
        printk("%s: Mapping of VMIC registers failed!\n", driver_name);
      }
      else
      {
        writew(VME_EN | BTO_EN | BTO_64 | MEC_BE | SEC_BE,
            VmicBaseAddr + FPGA_COMM_OFFSET);

        iounmap(VmicBaseAddr);
      }
    }
  }
  else
    printk("%s: Can't find VMIC FPGA device!\n", driver_name);
#endif

  // To use VMEbus slave images, the master bit must be set

  pci_csr = readl(baseaddr + PCI_CSR);
  if (!(pci_csr & 0x00000004))
  {
    pci_csr |= 0x00000004;
    writel(pci_csr, baseaddr + PCI_CSR);
  }

  // Clear sysfail line which (on some boards) is active by default

  if (readl(baseaddr + VCSR_CLR) & 0x40000000)
  {
    writel(0x40000000, baseaddr + VCSR_CLR);
    printk("%s: Switching off active SYSFAIL line!\n", driver_name);
  }

  // Everything is ok so lets turn off the windows and set VDW to A32

  for (i = 0; i < MAX_IMAGE; i++)
  {
    writel(0x00800000, baseaddr + aCTL[i]);         // Master images
    writel(0x00800000, baseaddr + aCTL[i + 10]);    // Slave images
  }

  // Lets turn off interrupts

  writel(0x00000000, baseaddr + LINT_EN);             // Disable interrupts
  writel(0x0000FFFF, baseaddr + LINT_STAT);           // Clear Any Pending irqs
  result = request_irq(universeII_dev->irq, irq_handler, IRQF_SHARED, driver_name, universeII_dev);
  if (result)
  {
    printk("%s: Can't get assigned pci irq vector %02X\n", driver_name, universeII_dev->irq);
    iounmap(baseaddr);
    pci_release_regions(universeII_dev);
#ifdef VMIC
    if (vmic_dev != NULL)
      pci_release_regions(vmic_dev);
#endif
    return -4;
  }
  else
  {
    printk("%s: Using PCI irq %02d (shared)!\n", driver_name, universeII_dev->irq);

    writel(0x000015FE, baseaddr + LINT_EN);      // enable DMA IRQ, BERR,
    // VME IRQ#1..#7 and SW_IACK
    writel(0, baseaddr + LINT_MAP0);                // Map all irqs to LINT#0
    writel(0, baseaddr + LINT_MAP1);                // Map all irqs to LINT#0
    writel(0, baseaddr + LINT_MAP2);                // Map all irqs to LINT#0
  }

  // Clear all image descriptors

  for (i = 0; i < MAX_MINOR + 1; i++)
  {
    image[i].phys_start = 0;
    image[i].phys_end = 0;
    image[i].size = 0;
    image[i].vBase = NULL;
    image[i].opened = 0;
    image[i].okToWrite = 0;
    image[i].slaveBuf = NULL;
    image[i].buffer = 0;
  }

  // Reserve 128kB wide memory area for DMA buffer

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 0, 0)
  virtAddr = dma_alloc_coherent(&universeII_dev->dev, PCI_BUF_SIZE, &dmaHandle, GFP_ATOMIC);
#else
  virtAddr = pci_alloc_consistent(universeII_dev, PCI_BUF_SIZE, &dmaHandle);
#endif
  if (virtAddr == NULL)
  {
    printk("%s: Unable to allocate memory for DMA buffer!\n", driver_name);
    dmaBuf = 0;
    iounmap(baseaddr);
    pci_release_regions(universeII_dev);
#ifdef VMIC
    if (vmic_dev != NULL)
      pci_release_regions(vmic_dev);
#endif
    return -5;
  }
  else
  {
    for (page = virt_to_page(virtAddr);
        page < virt_to_page(virtAddr + PCI_BUF_SIZE); ++page)
      SetPageReserved(page);

    dmaBuf = virtAddr;
  }

  // Reserve 8 memory areas (128kB wide) for slave images

  for (i = 10; i < 18; i++)
  {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 0, 0)
    virtAddr = dma_alloc_coherent(&universeII_dev->dev, PCI_BUF_SIZE, &image[i].buffer, GFP_ATOMIC);
#else
    virtAddr = pci_alloc_consistent(universeII_dev, PCI_BUF_SIZE, &image[i].buffer);
#endif

    if (virtAddr == NULL)
    {
      printk("%s: Unable to allocate memory for slave image!\n", driver_name);
      image[i].buffer = 0;
    }
    else
    {
      for (page = virt_to_page(virtAddr);
          page < virt_to_page(virtAddr + PCI_BUF_SIZE); ++page)
        SetPageReserved(page);

      image[i].slaveBuf = virtAddr;
    }
  }

  /* Assign major and minor numbers for the driver */
  err = register_chrdev_region(MKDEV(UNI_MAJOR, 0), MAX_MINOR + 1, driver_name);
  if (err)
  {
    printk(KERN_WARNING "%s: Error getting Major Number %d for "
        "driver.\n", driver_name , UNI_MAJOR);
    iounmap(baseaddr);
    pci_release_regions(universeII_dev);
#ifdef VMIC
    if (vmic_dev != NULL)
      pci_release_regions(vmic_dev);
#endif
    return -6;
  }
  universeII_cdev = cdev_alloc();
  universeII_cdev->ops = &universeII_fops;
  universeII_cdev->owner = THIS_MODULE;
  err = cdev_add(universeII_cdev, MKDEV(UNI_MAJOR, 0), MAX_MINOR + 1);
  if (err)
  {
    printk(KERN_WARNING "%s: cdev_all failed\n", driver_name);
    iounmap(baseaddr);
    pci_release_regions(universeII_dev);
#ifdef VMIC
    if (vmic_dev != NULL)
      pci_release_regions(vmic_dev);
#endif
    return -6;
  }

  /* Create sysfs entries - on udev systems this creates the dev files */
  universeII_sysfs_class = class_create(THIS_MODULE, driver_name);
  if (IS_ERR(universeII_sysfs_class))
  {
    printk(KERN_ERR "Error creating universeII class.\n");
    iounmap(baseaddr);
    pci_release_regions(universeII_dev);
#ifdef VMIC
    if (vmic_dev != NULL)
      pci_release_regions(vmic_dev);
#endif
    return -6;
  }

  /* Add sysfs Entries */
  for (i = 0; i <= MAX_MINOR; i++)
  {
    if (i < MAX_IMAGE)
    {
      sprintf(name, "vme_m%%d");
    }
    else if (i == CONTROL_MINOR)
    {
      sprintf(name, "vme_ctl");
    }
    else if (i == DMA_MINOR)
    {
      sprintf(name, "vme_dma");
    }
    else
    {
      sprintf(name, "vme_s%%d");
    }
    if (i < MAX_IMAGE)
      num = i; //Master images
    else if (i > 9)
      num = i - 10; //Slave images
    else
      num = 0;
    if (IS_ERR(device_create(universeII_sysfs_class, NULL, MKDEV(UNI_MAJOR, i), NULL, name, num)))
    {
      printk(KERN_INFO "%s: Error creating sysfs device\n", driver_name);
      while (i > 0)
      {
        i--;
        device_destroy(universeII_sysfs_class, MKDEV(UNI_MAJOR, i));
      }
      class_destroy(universeII_sysfs_class);
      return -6;
    }
  }

  // Create entry "/proc/universeII"

  register_proc();

  dma_in_use = 0;
  dma_blt_berr = 0;

  // Setup a DMA and MBX timer to timeout 'infinite' transfers or hangups

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 14, 0)
  init_timer(&DMA_timer);
  DMA_timer.function = DMA_timeout;
#else
  timer_setup(&DMA_timer, DMA_timeout, 0);
#endif

  for (i = 0; i < 4; i++)
  {
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 14, 0)
    init_timer(&mbx_device[i].mbxTimer);
    mbx_device[i].mbxTimer.function = MBX_timeout;
    mbx_device[i].mbxTimer.data = i;
#else
    timer_setup(&mbx_device[i].mbxTimer, MBX_timeout, 0);
#endif
  }

  // Initialize list for DMA command packet structures

  for (i = 0; i < 256; i++)
  {
    cpLists[i].free = 1;
    cpLists[i].commandPacket = NULL;
  }

  // Initialize wait queues for DMA, VME irq and mailbox handling

  init_waitqueue_head(&dmaWait);
  init_waitqueue_head(&vmeWait);

  for (i = 0; i < 4; i++)
    init_waitqueue_head(&mbx_device[i].mbxWait);

  // Reset all irq devices

  for (i = 0; i < 7; i++)
    for (j = 0; j < 256; j++)
      irq_device[i][j].ok = 0;

  // Initialize VMEBuserr list

  for (i = 0; i < 32; i++)
  {
    vmeBerrList[i].valid = 0;
    vmeBerrList[i].merr = 0;
  }

  // Reset all statistic counters

  statistics.reads = 0;
  statistics.writes = 0;
  statistics.ioctls = 0;
  statistics.irqs = 0;
  statistics.berrs = 0;
  statistics.dmaErrors = 0;
  statistics.timeouts = 0;

  return 0;
}

module_init( universeII_init);
module_exit( universeII_exit);
