#ifndef lint
static char rcsid[] = "$Id: scsi-bsd.c,v 1.1.2.2 1998/12/22 05:12:06 oliva Exp $";
#endif
/*
 * Interface to execute SCSI commands on an SGI Workstation
 *
 * Copyright (c) 1998 T.Hepper th@icem.de
 */
#include <amanda.h>

#ifdef HAVE_BSD_LIKE_SCSI

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

#include <sys/scsiio.h>

#include <scsi-defs.h>

/*
 * Check if the device is already open,
 * if no open it and save it in the list 
 * of open files.
 */
OpenFiles_T * SCSI_OpenDevice(char *DeviceName)
{
  int DeviceFD;
  int i;
  OpenFiles_T *pwork;
  
  if ((DeviceFD = open(DeviceName, O_RDWR)) > 0)
    {
      pwork = (OpenFiles_T *)malloc(sizeof(OpenFiles_T));
      pwork->next = NULL;
      pwork->fd = DeviceFD;
      pwork->dev = strdup(DeviceName);
      pwork->inquiry = (SCSIInquiry_T *)malloc(sizeof(SCSIInquiry_T));
      Inquiry(DeviceFD, pwork->inquiry);
      for (i=0;i < 16 && pwork->inquiry->prod_ident[i] != ' ';i++)
        pwork->name[i] = pwork->inquiry->prod_ident[i];
      pwork->name[i] = '\0';
      pwork->SCSI = 1;
      return(pwork);
    }
  
  return(NULL); 
}

int SCSI_CloseDevice(int DeviceFD)
{
  int ret;
    
  ret = close(DeviceFD) ;
  return(ret);
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
  ExtendedRequestSense_T ExtendedRequestSense;
  scsireq_t ds;
  int Zero = 0, Result;
  int retries = 5;
  extern int errno;
  
  memset(&ds, 0, sizeof(scsireq_t));
  memset(pRequestSense, 0, RequestSenseLength);
  memset(&ExtendedRequestSense, 0 , sizeof(ExtendedRequestSense_T)); 
  
  ds.flags = SCCMD_ESCAPE; 
  /* Timeout */
  ds.timeout = 120000;
  /* Set the cmd */
  memcpy(ds.cmd, CDB, CDB_Length);
  ds.cmdlen = CDB_Length;
  /* Data buffer for results */
  ds.databuf = (caddr_t)DataBuffer;
  ds.datalen = DataBufferLength;
  /* Sense Buffer */
  /*
    ds.sense = (u_char)pRequestSense;
  */
  ds.senselen = RequestSenseLength;
    
  switch (Direction) 
    {
    case Input:
      ds.flags = ds.flags | SCCMD_READ;
      break;
    case Output:
      ds.flags = ds.flags | SCCMD_WRITE;
      break;
    }
    
  while (--retries > 0) {
    Result = ioctl(DeviceFD, SCIOCCOMMAND, &ds);
    memcpy(pRequestSense, ds.sense, RequestSenseLength);
    if (Result < 0)
      {
        dbprintf(("errno : %d\n",errno));
        return (-1);
      }
    dbprintf(("SCSI_ExecuteCommand(BSD) %02X STATUS(%02X) \n", CDB[0], ds.retsts));
    switch (ds.retsts)
      {
      case SCCMD_BUSY:                /*  BUSY */
        break;
      case SCCMD_OK:                /*  GOOD */
        return(SCCMD_OK);
        break;
      case SCCMD_SENSE:               /*  CHECK CONDITION */ 
        return(SCCMD_SENSE);
        break;
      default:
        continue;
      }
  }   
  return(ds.retsts);
}

#endif
/*
 * Local variables:
 * indent-tabs-mode: nil
 * c-file-style: gnu
 * End:
 */
