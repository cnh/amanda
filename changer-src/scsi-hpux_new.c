#ifndef lint
static char rcsid[] = "$Id: scsi-hpux_new.c,v 1.8 2000/07/17 18:55:47 ant Exp $";
#endif
/*
 * Interface to execute SCSI commands on an HP-UX Workstation
 *
 * Copyright (c) Thomas Hepper th@ant.han.de
 */
#include <amanda.h>

#ifdef HAVE_HPUX_LIKE_SCSI

# ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif
/*
#ifdef HAVE_STDIO_H
*/
#include <stdio.h>
/*
#endif
*/
#ifdef HAVE_SYS_IOCTL_H
#include <sys/ioctl.h>
#endif
#ifdef HAVE_SYS_SCSI_H
#include <sys/scsi.h>
#endif

#include <sys/mtio.h>

#include <scsi-defs.h>

int SCSI_OpenDevice(OpenFiles_T *pwork, char *DeviceName)
{
  int DeviceFD;
  int i;

  if ((DeviceFD = open(DeviceName, O_RDWR| O_NDELAY)) > 0)
    {
      pwork->fd = DeviceFD;
      pwork->SCSI = 0;
      pwork->dev = strdup(DeviceName);
      pwork->inquiry = (SCSIInquiry_T *)malloc(INQUIRY_SIZE);
      
      if (SCSI_Inquiry(DeviceFD, pwork->inquiry, INQUIRY_SIZE) == 0)
          {
            if (pwork->inquiry->type == TYPE_TAPE || pwork->inquiry->type == TYPE_CHANGER)
              {
                for (i=0;i < 16;i++)
                  pwork->ident[i] = pwork->inquiry->prod_ident[i];
                for (i=15; i >= 0 && !isalnum(pwork->inquiry->prod_ident[i]) ; i--)
                  {
                    pwork->inquiry->prod_ident[i] = '\0';
                  }
                pwork->SCSI = 1;
                PrintInquiry(pwork->inquiry);
                return(1);    
              } else {
                close(DeviceFD);
                free(pwork->inquiry);
                return(0);
              }
          } else {
            free(pwork->inquiry);
            pwork->inquiry = NULL;
            return(1);
          }
      return(1);
    }

  return(0); 
}

int SCSI_CloseDevice(int DeviceFD)
{
  int ret;

  ret = close(DeviceFD);
  return(ret);
}

int SCSI_ExecuteCommand(int DeviceFD,
                        Direction_T Direction,
                        CDB_T CDB,
                        int CDB_Length,
                        void *DataBuffer,
                        int DataBufferLength,
                        char *RequestSense,
                        int RequestSenseLength)
{
  struct sctl_io sctl_io;
  extern int errno;
  int Retries = 3;
  int Zero = 0, Result;
  
  memset(&sctl_io, '\0', sizeof(struct sctl_io));

  sctl_io.flags = 0;  
  sctl_io.max_msecs = 240000;
  /* Set the cmd */
  memcpy(sctl_io.cdb, CDB, CDB_Length);
  sctl_io.cdb_length = CDB_Length;
  /* Data buffer for results */
  sctl_io.data = DataBuffer;
  sctl_io.data_length = (unsigned)DataBufferLength;

  switch (Direction) 
    {
    case Input:
      sctl_io.flags = sctl_io.flags | SCTL_READ;
      break;
    case Output:
      break;
    }

  while (--Retries > 0) {
    DecodeSCSI(CDB, "SCSI_ExecuteCommand : ");
    Result = ioctl(DeviceFD, SIOC_IO, &sctl_io);
    if (Result < 0)
      return(Result);
    
    memcpy(RequestSense, sctl_io.sense, RequestSenseLength);

    switch(sctl_io.cdb_status)
      {
      case S_GOOD:
      case S_CHECK_CONDITION:
        return(sctl_io.cdb_status);
        break;
      default:
        return(sctl_io.cdb_status);
      }
  }
  return(-1);
}

int Tape_Eject (int DeviceFD)
{
  struct mtop mtop;

  mtop.mt_op = MTOFFL;
  mtop.mt_count = 1;
  if (ioctl(DeviceFD, MTIOCTOP, &mtop) < 0) {
    return(-1);
  } else {
    return(0);
  }
}


int Tape_Status( int DeviceFD)
{
  struct mtget mtget;
  int ret = 0;

  if (ioctl(DeviceFD, MTIOCGET, &mtget) != 0)
  {
     dbprintf(("Tape_Status error ioctl %d\n",errno));
     return(-1);
  }

  dbprintf(("ioctl -> mtget.mt_gstat %X\n",mtget.mt_gstat));
  if (GMT_ONLINE(mtget.mt_gstat))
  {
    ret = TAPE_ONLINE;
  }

  if (GMT_BOT(mtget.mt_gstat))
  {
    ret = ret | TAPE_BOT;
  }

  if (GMT_EOT(mtget.mt_gstat))
  {
    ret = ret | TAPE_EOT;
  }

  if (GMT_WR_PROT(mtget.mt_gstat))
  {
    ret = ret | TAPE_WR_PROT;
  }
 
  return(ret); 
}
#endif
/*
 * Local variables:
 * indent-tabs-mode: nil
 * c-file-style: gnu
 * End:
 */
