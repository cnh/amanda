#ifndef lint
static char rcsid[] = "$Id: scsi-linux.c,v 1.12 2000/07/31 18:55:13 ant Exp $";
#endif
/*
 * Interface to execute SCSI commands on Linux
 *
 * Copyright (c) Thomas Hepper th@ant.han.de
 */
#include <amanda.h>

#ifdef HAVE_LINUX_LIKE_SCSI

/*
#ifdef HAVE_STDIO_H
*/
#include <stdio.h>
/*
#endif
*/
#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif
#ifdef HAVE_SYS_STAT_H
#include <sys/stat.h>
#endif
#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif
#include <time.h>

#ifdef HAVE_SCSI_SCSI_IOCTL_H
#include <scsi/scsi_ioctl.h>
#endif

#ifdef HAVE_SCSI_SG_H
#include <scsi/sg.h>
#define LINUX_SG
#endif

#ifdef HAVE_SYS_MTIO_H
#include <sys/mtio.h>
#endif

#include <scsi-defs.h>



int SCSI_CloseDevice(int DeviceFD)
{
  extern OpenFiles_T *pDev;
  int ret;
  
  if (pDev[DeviceFD].devopen == 1)
    {
      pDev[DeviceFD].devopen = 0;
      ret = close(pDev[DeviceFD].fd);
    }

  return(ret);
}

#ifdef LINUX_SG
int SCSI_OpenDevice(int ip)
{
  extern OpenFiles_T *pDev;
  int DeviceFD;
  int i;
  int timeout;
  struct stat pstat;
  char *buffer = NULL ;           /* Will contain the device name after checking */
  int openmode = O_RDONLY;

  if (pDev[ip].inqdone == 0)
    {
      pDev[ip].inqdone = 1;
      if (strncmp("/dev/sg", pDev[ip].dev, 7) != 0) /* Check if no sg device for an link .... */
        {
          dbprintf(("SCSI_OpenDevice : checking if %s is a sg device\n", pDev[ip].dev));
          if (lstat(pDev[ip].dev, &pstat) != -1)
            {
              if (S_ISLNK(pstat.st_mode) == 1)
                {
                  dbprintf(("SCSI_OpenDevice : is a link, checking destination\n"));
                  if ((buffer = (char *)malloc(512)) == NULL)
                    {
                      dbprintf(("SCSI_OpenDevice : malloc failed\n"));
                      return(0);
                    }
                  memset(buffer, 0, 512);
                  if (( i = readlink(pDev[ip].dev, buffer, 512)) == -1)
                    {
                      if (errno == ENAMETOOLONG )
                        {
                        } else {
                          pDev[ip].SCSI = 0;
                        }
                    }
                  if ( i >= 7)
                    {
                      if (strncmp("/dev/sg", buffer, 7) == 0)
                        {
                          dbprintf(("SCSI_OpenDevice : link points to %s\n", buffer));
                          pDev[ip].flags = 1;
                        }
                    }
                } else {/* S_ISLNK(pstat.st_mode) == 1 */
                  dbprintf(("No link %s\n", pDev[ip].dev));
                  buffer = strdup(pDev[ip].dev);
                }
            } else {/* lstat(DeviceName, &pstat) != -1 */ 
              dbprintf(("can't stat device %s\n", pDev[ip].dev));
              return(0);
            }
        } else {
          buffer = strdup(pDev[ip].dev);
          pDev[ip].flags = 1;
        }
      
      if (pDev[ip].flags == 1)
        {
          openmode = O_RDWR;
        }
      
      if ((DeviceFD = open(buffer, openmode)) > 0)
        {
          pDev[ip].avail = 1;
          pDev[ip].devopen = 1;
          pDev[ip].fd = DeviceFD;
        } else {
          return(0);
        }
      
      if ( pDev[ip].flags == 1)
        {
          pDev[ip].SCSI = 1;
        }
      
      pDev[ip].dev = strdup(buffer);
      if (pDev[ip].SCSI == 1)
        {
          dbprintf(("SCSI_OpenDevice : use SG interface\n"));
          if ((timeout = ioctl(DeviceFD, SG_GET_TIMEOUT)) > 0) 
            {
              dbprintf(("SCSI_OpenDevice : current timeout %d\n", timeout));
              timeout = 60000;
              if (ioctl(DeviceFD, SG_SET_TIMEOUT, &timeout) == 0)
                {
                  dbprintf(("SCSI_OpenDevice : timeout set to %d\n", timeout));
                }
            }
          pDev[ip].inquiry = (SCSIInquiry_T *)malloc(INQUIRY_SIZE);
          if (SCSI_Inquiry(ip, pDev[ip].inquiry, INQUIRY_SIZE) == 0)
            {
              if (pDev[ip].inquiry->type == TYPE_TAPE || pDev[ip].inquiry->type == TYPE_CHANGER)
                {
                  for (i=0;i < 16;i++)
                    pDev[ip].ident[i] = pDev[ip].inquiry->prod_ident[i];
                  for (i=15; i >= 0 && !isalnum(pDev[ip].ident[i]); i--)
                    {
                      pDev[ip].ident[i] = '\0';
                    }
                  pDev[ip].SCSI = 1;
                  PrintInquiry(pDev[ip].inquiry);
                  return(1);
                } else {
                  close(DeviceFD);
                  free(pDev[ip].inquiry);
                  return(0);
                }
            } else {
              pDev[ip].SCSI = 0;
              close(DeviceFD);
              free(pDev[ip].inquiry);
              pDev[ip].inquiry = NULL;
              return(1);
            }
        }
    } else {
      if (pDev[ip].flags == 1)
        {
          openmode = O_RDWR;
        } else {
          openmode = O_RDONLY;
        }
      if ((DeviceFD = open(pDev[ip].dev, openmode)) > 0)
        {
          pDev[ip].devopen = 1;
          pDev[ip].fd = DeviceFD;
          if (pDev[ip].flags = 1)
            {
              if ((timeout = ioctl(DeviceFD, SG_GET_TIMEOUT)) > 0) 
                {
                  dbprintf(("SCSI_OpenDevice : current timeout %d\n", timeout));
                  timeout = 60000;
                  if (ioctl(DeviceFD, SG_SET_TIMEOUT, &timeout) == 0)
                    {
                      dbprintf(("SCSI_OpenDevice : timeout set to %d\n", timeout));
                    }
                }
            }
          return(1);
        } else {
          return(0);
        }
    }
  return(0);
}

#define SCSI_OFF sizeof(struct sg_header)
int SCSI_ExecuteCommand(int DeviceFD,
                        Direction_T Direction,
                        CDB_T CDB,
                        int CDB_Length,
                        void *DataBuffer,
                        int DataBufferLength,
                        char *pRequestSense,
                        int RequestSenseLength)
{
  extern OpenFiles_T *pDev;
  struct sg_header *psg_header;
  char *buffer;
  int osize;
  int status;

  if (pDev[DeviceFD].avail == 0)
    {
      return(-1);
    }

  if (pDev[DeviceFD].devopen == 0)
    {
      SCSI_OpenDevice(DeviceFD);
    }
  
  if (SCSI_OFF + CDB_Length + DataBufferLength > 4096) 
    {
      SCSI_CloseDevice(DeviceFD);
      return(-1);
    }

  buffer = (char *)malloc(SCSI_OFF + CDB_Length + DataBufferLength);
  memset(buffer, 0, SCSI_OFF + CDB_Length + DataBufferLength);
  memcpy(buffer + SCSI_OFF, CDB, CDB_Length);
  
  psg_header = (struct sg_header *)buffer;
  if (CDB_Length >= 12)
    {
      psg_header->twelve_byte = 1;
    } else {
      psg_header->twelve_byte = 0;
    }
  psg_header->result = 0;
  psg_header->reply_len = SCSI_OFF + DataBufferLength;
  
  switch (Direction)
    {
    case Input:
      osize = 0;
      break;
    case Output:
      osize = DataBufferLength;
      break;
    }
  
  DecodeSCSI(CDB, "SCSI_ExecuteCommand : ");
  
  status = write(pDev[DeviceFD].fd, buffer, SCSI_OFF + CDB_Length + osize);
  if ( status < 0 || status != SCSI_OFF + CDB_Length + osize ||
       psg_header->result ) 
    {
      dbprintf(("SCSI_ExecuteCommand error send \n"));
      SCSI_CloseDevice(DeviceFD);
      return(-1);
    }
  
  memset(buffer, 0, SCSI_OFF + DataBufferLength);
  status = read(pDev[DeviceFD].fd, buffer, SCSI_OFF + DataBufferLength);
  memset(pRequestSense, 0, RequestSenseLength);
  memcpy(pRequestSense, psg_header->sense_buffer, 16);
  
  if ( status < 0 || status != SCSI_OFF + DataBufferLength || 
       psg_header->result ) 
    { 
      dbprintf(("SCSI_ExecuteCommand error read \n"));
      dbprintf(("Status %d (%d) %2X\n", status, SCSI_OFF + DataBufferLength, psg_header->result ));
      SCSI_CloseDevice(DeviceFD);
      return(-1);
    }

  if (DataBufferLength)
    {
       memcpy(DataBuffer, buffer + SCSI_OFF, DataBufferLength);
    }

  free(buffer);
  SCSI_CloseDevice(DeviceFD);
  return(0);
}

#else

static inline int min(int x, int y)
{
  return (x < y ? x : y);
}


static inline int max(int x, int y)
{
  return (x > y ? x : y);
}

int SCSI_OpenDevice(int ip)
{
  extern OpenFiles_T *pDev;
  int DeviceFD;
  int i;

  if (pDev[ip].inqdone == 0)
    {
      pDev[ip].inqdone = 1;
      if ((DeviceFD = open(pDev[ip].dev, O_RDWR)) > 0)
        {
          pDev[ip].avail = 1;
          pDev[ip].fd = DeviceFD;
          pDev[ip].SCSI = 0;
          pDev[ip].inquiry = (SCSIInquiry_T *)malloc(INQUIRY_SIZE);
          dbprintf(("SCSI_OpenDevice : use ioctl interface\n"));
          if (SCSI_Inquiry(ip, pDev[ip].inquiry, INQUIRY_SIZE) == 0)
            {
              if (pDev[ip].inquiry->type == TYPE_TAPE || pDev[ip].inquiry->type == TYPE_CHANGER)
                {
                  for (i=0;i < 16 && pDev[ip].inquiry->prod_ident[i] != ' ';i++)
                    pDev[ip].ident[i] = pDev[ip].inquiry->prod_ident[i];
                  pDev[ip].ident[i] = '\0';
                  pDev[ip].SCSI = 1;
                  PrintInquiry(pDev[ip].inquiry);
                  return(1);
                } else {
                  free(pDev[ip].inquiry);
                  close(DeviceFD);
                  return(0);
                }
            } else {
              close(DeviceFD);
              free(pDev[ip].inquiry);
              pDev[ip].inquiry = NULL;
              return(1);
            }
        }
      return(1); 
    } else {
      if ((DeviceFD = open(pDev[ip].dev, O_RDWR)) > 0)
        {
          pDev[ip].fd = DeviceFD;
          pDev[ip].devopen = 1;
          return(1);
        } else {
          pDev[ip].devopen = 0;
          return(0);
        }
    }
}

int SCSI_ExecuteCommand(int DeviceFD,
                        Direction_T Direction,
                        CDB_T CDB,
                        int CDB_Length,
                        void *DataBuffer,
                        int DataBufferLength,
                        char *pRequestSense,
                        int RequestSenseLength)
{
  extern OpenFiles_T *pDev;
  unsigned char *Command;
  int Zero = 0, Result;
 
  if (pDev[DeviceFD].avail == 0)
    {
      return(-1);
    }

  if (pDev[DeviceFD].devopen == 0)
    {
      SCSI_OpenDevice(DeviceFD);
    }

  memset(pRequestSense, 0, RequestSenseLength);
  switch (Direction)
    {
    case Input:
      Command = (unsigned char *)
        malloc(8 + max(DataBufferLength, RequestSenseLength));
      memcpy(&Command[0], &Zero, 4);
      memcpy(&Command[4], &DataBufferLength, 4);
      memcpy(&Command[8], CDB, CDB_Length);
      break;
    case Output:
      Command = (unsigned char *)
        malloc(8 + max(CDB_Length + DataBufferLength, RequestSenseLength));
      memcpy(&Command[0], &DataBufferLength, 4);
      memcpy(&Command[4], &Zero, 4);
      memcpy(&Command[8], CDB, CDB_Length);
      memcpy(&Command[8 + CDB_Length], DataBuffer, DataBufferLength);
      break;
    }
  
  DecodeSCSI(CDB, "SCSI_ExecuteCommand : ");
  
  Result = ioctl(pDev[DeviceFD].fd, SCSI_IOCTL_SEND_COMMAND, Command);
  if (Result != 0)
    memcpy(pRequestSense, &Command[8], RequestSenseLength);
  else if (Direction == Input)
    memcpy(DataBuffer, &Command[8], DataBufferLength);
  free(Command);
  SCSI_Close(DeviceFD);
  return Result;
}
#endif

int Tape_Eject ( int DeviceFD)
{
  extern OpenFiles_T *pDev;
  struct mtop mtop;

  if (pDev[DeviceFD].devopen == 0)
    {
      SCSI_OpenDevice(DeviceFD);
    }
  
  mtop.mt_op = MTUNLOAD;
  mtop.mt_count = 1;
  ioctl(pDev[DeviceFD].fd, MTIOCTOP, &mtop);
  SCSI_CloseDevice(DeviceFD);
  return(0);
}

int Tape_Status( int DeviceFD)
{
  extern OpenFiles_T *pDev;
  struct mtget mtget;
  int ret = 0;

  if (pDev[DeviceFD].devopen == 0)
    {
      SCSI_OpenDevice(DeviceFD);
    }

  if (ioctl(pDev[DeviceFD].fd , MTIOCGET, &mtget) != 0)
  {
     dbprintf(("Tape_Status error ioctl %d\n",errno));
     SCSI_CloseDevice(DeviceFD);
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

  SCSI_CloseDevice(DeviceFD);
  return(ret); 
}

#endif
/*
 * Local variables:
 * indent-tabs-mode: nil
 * c-file-style: gnu
 * End:
 */
