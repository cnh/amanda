#ifndef lint
static char rcsid[] = "$Id: scsi-linux.c,v 1.1.2.12 1999/03/04 20:47:49 th Exp $";
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

#include <scsi/scsi_ioctl.h>
#include <scsi/sg.h>
#include <sys/mtio.h>

#include <scsi-defs.h>



int SCSI_CloseDevice(int DeviceFD)
{
    int ret;
    
    ret = close(DeviceFD);
    return(ret);
}
#define LINUX_CHG
#ifdef LINUX_CHG
OpenFiles_T * SCSI_OpenDevice(char *DeviceName)
{
  int DeviceFD;
  int i;
  int timeout;
  OpenFiles_T *pwork;
  struct stat pstat;
  char *buffer;

  if ((DeviceFD = open(DeviceName, O_RDWR)) > 0)
    {
      pwork = (OpenFiles_T *)malloc(sizeof(OpenFiles_T));
      memset(pwork, 0, sizeof(OpenFiles_T));
     pwork->fd = DeviceFD;
      if (strncmp("/dev/sg", DeviceName, 7) != 0)
        {
          dbprintf(("SCSI_OpenDevice : checking if %s is a sg device\n", DeviceName));
          if (lstat(DeviceName, &pstat) != -1)
            {
              if (S_ISLNK(pstat.st_mode) == 1)
                {
                  dbprintf(("SCSI_OpenDevice : is a link, checking destination\n"));
                  if ((buffer = (char *)malloc(512)) == NULL)
                    {
                      dbprintf(("SCSI_OpenDevice : malloc failed\n"));
                      return(NULL);
                    }
                  memset(buffer, 0, 512);
                  if (( i = readlink(DeviceName, buffer, 512)) == -1)
                    {
                      if (errno == ENAMETOOLONG )
                        {
                        } else {
                          pwork->SCSI = 0;
                        }
                    }
                  if ( i >= 7)
                    {
                      if (strncmp("/dev/sg", buffer, 7) == 0)
                        {
                          dbprintf(("SCSI_OpenDevice : link points to %s\n", buffer));
                          pwork->SCSI = 1;
                        } else {
                          dbprintf(("SCSI_OpenDevice : link %s does not point to /dev/sg device\n",
                                    DeviceName));
                          pwork->SCSI = 0;
                        }
                    } else {
                      dbprintf(("SCSI_OpenDevice : link %s does not point to /dev/sg device\n",
                                DeviceName));
                    }
                  dbprintf(("SCSI_OpenDevice : link points to %s\n", buffer));
                } else {
                  dbprintf(("SCSI_OpenDevice : %s is neither a link nor an sg device\n", DeviceName));
                  pwork->SCSI = 0;
                }
            } else {
              dbprintf(("SCSI_OpenDevice : lstat on %s failed\n", DeviceName));
              pwork->SCSI=0;
            }
        } else {
          pwork->SCSI = 1;
        }
      
      pwork->dev = strdup(DeviceName);
      if (pwork->SCSI == 1)
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
          pwork->inquiry = (SCSIInquiry_T *)malloc(INQUIRY_SIZE);
          if (SCSI_Inquiry(DeviceFD, pwork->inquiry, INQUIRY_SIZE) == 0)
            {
              if (pwork->inquiry->type == TYPE_TAPE || pwork->inquiry->type == TYPE_CHANGER)
                {
                  for (i=0;i < 16;i++)
                    pwork->ident[i] = pwork->inquiry->prod_ident[i];
                  for (i=15; i >= 0 && !isalnum(pwork->inquiry->prod_ident[i]); i--)
                    {
                      pwork->ident[i] = '\0';
                    }
                  pwork->SCSI = 1;
                  PrintInquiry(pwork->inquiry);
                  return(pwork);
                } else {
                  free(pwork->inquiry);
                  free(pwork);
                  close(DeviceFD);
                  return(NULL);
                }
            } else {
              pwork->SCSI = 0;
              free(pwork->inquiry);
              pwork->inquiry = NULL;
              return(pwork);
            }
        }
      return(pwork);
    }
  return(NULL); 
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
 struct sg_header *psg_header;
  char *buffer;
  int osize;
  int status;

  if (SCSI_OFF + CDB_Length + DataBufferLength > 4096) 
    {
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

  status = write(DeviceFD, buffer, SCSI_OFF + CDB_Length + osize);
  if ( status < 0 || status != SCSI_OFF + CDB_Length + osize ||
    psg_header->result ) 
    {
      dbprintf(("SCSI_ExecuteCommand error send \n"));
      return(-1);
    }

  memset(buffer, 0, SCSI_OFF + DataBufferLength);
  status = read(DeviceFD, buffer, SCSI_OFF + DataBufferLength);
  memset(pRequestSense, 0, RequestSenseLength);
  memcpy(psg_header->sense_buffer, pRequestSense, 16);

  if ( status < 0 || status != SCSI_OFF + DataBufferLength || 
    psg_header->result ) 
    { 
      dbprintf(("SCSI_ExecuteCommand error read \n"));
      dbprintf(("Status %d (%d) %2X\n", status, SCSI_OFF + DataBufferLength, psg_header->result ));
      return(-1);
    }

  if (DataBufferLength)
    {
       memcpy(DataBuffer, buffer + SCSI_OFF, DataBufferLength);
    }

  free(buffer);

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

OpenFiles_T * SCSI_OpenDevice(char *DeviceName)
{
  int DeviceFD;
  int i;
  OpenFiles_T *pwork;

  if ((DeviceFD = open(DeviceName, O_RDWR)) > 0)
    {
      pwork = (OpenFiles_T *)malloc(sizeof(OpenFiles_T));
      pwork->fd = DeviceFD;
      pwork->SCSI = 0;
      pwork->dev = strdup(DeviceName);
      pwork->inquiry = (SCSIInquiry_T *)malloc(INQUIRY_SIZE);
      dbprintf(("SCSI_OpenDevice : use ioctl interface\n"));
      if (SCSI_Inquiry(DeviceFD, pwork->inquiry, INQUIRY_SIZE) == 0)
        {
          if (pwork->inquiry->type == TYPE_TAPE || pwork->inquiry->type == TYPE_CHANGER)
            {
              for (i=0;i < 16 && pwork->inquiry->prod_ident[i] != ' ';i++)
                pwork->ident[i] = pwork->inquiry->prod_ident[i];
              pwork->ident[i] = '\0';
              pwork->SCSI = 1;
              PrintInquiry(pwork->inquiry);
              return(pwork);
            } else {
              free(pwork->inquiry);
                  free(pwork);
                  close(DeviceFD);
                  return(NULL);
            }
            } else {
              free(pwork->inquiry);
              pwork->inquiry = NULL;
              return(pwork);
            }
      
    }
  return(pwork); 
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
    unsigned char *Command;
    int Zero = 0, Result;
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

    Result = ioctl(DeviceFD, SCSI_IOCTL_SEND_COMMAND, Command);
    if (Result != 0)
        memcpy(pRequestSense, &Command[8], RequestSenseLength);
    else if (Direction == Input)
        memcpy(DataBuffer, &Command[8], DataBufferLength);
    free(Command);
    return Result;
}
#endif

int Tape_Eject ( int DeviceFD)
{
  struct mtop mtop;

  mtop.mt_op = MTUNLOAD;
  mtop.mt_count = 1;
  ioctl(DeviceFD, MTIOCTOP, &mtop);
  return(0);
}

#endif
/*
 * Local variables:
 * indent-tabs-mode: nil
 * c-file-style: gnu
 * End:
 */
