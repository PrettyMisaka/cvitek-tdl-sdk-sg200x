#define LOG_TAG "SampleFD"
#define LOG_LEVEL LOG_LEVEL_INFO

#include "middleware_utils.h"
#include "sample_utils.h"
#include "vi_vo_utils.h"

#include <core/utils/vpss_helper.h>
#include <cvi_comm.h>
#include <rtsp.h>
#include <sample_comm.h>
#include "cvi_tdl.h"

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <stdlib.h>
#include <unistd.h>
#include <stddef.h>
#include <stdio.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/fb.h>
#include <time.h>
// #define MY_VI_PIXEL_FORMAT PIXEL_FORMAT_RGB_888
#define MY_VI_PIXEL_FORMAT PIXEL_FORMAT_NV21

#define UNUSED __attribute__((unused))

static volatile bool bExit = false;

static cvtdl_face_t g_stFaceMeta = {0};

static uint32_t g_size = 0;

static int fbfd = 0;
char *fbp = 0;

static int lcd_init();
UNUSED static void showVpssGrpAttr(VPSS_GRP_ATTR_S stVpssGrpAttr);
UNUSED static void yuvScale2rgb565(char *dstAddr, VIDEO_FRAME_INFO_S stFrame);
UNUSED static void yuv2rgb565(char *dstAddr, VIDEO_FRAME_INFO_S stVOFrame, int x, int y);
UNUSED static void lcd_show_rgb565(char *dstAddr, VIDEO_FRAME_INFO_S stFrame, int x, int y);

static short __s_r_1370705v[256] = {0};
static short __s_b_1732446u[256] = {0};
static short __s_g_337633u[256] = {0};
static short __s_g_698001v[256] = {0};

static bool __isInitialized = false;
// static VIDEO_FRAME_INFO_S _stFrame;
 
/* 初始化YUV转RGB转换对照表 */
static bool initYUV2RGBTabel()
{
    for (int i = 0; i < 256; i++) {
        __s_r_1370705v[i] = (1.370705 * (i-128));
        __s_b_1732446u[i] = (1.732446 * (i-128));
        __s_g_337633u[i] = (0.337633 * (i-128));
        __s_g_698001v[i] = (0.698001 * (i-128));
    }
    return true;
}

UNUSED unsigned short YUV2RGB(unsigned char y, unsigned char u, unsigned char v)
{
    /* 只初始化一次，用于初始化YUV转RGB对照表 */
    if(__isInitialized == false) __isInitialized = initYUV2RGBTabel();
 
    int r = (int)y + __s_r_1370705v[v];
    int g = (int)y - __s_g_337633u[u] - __s_g_698001v[v];
    int b = (int)y + __s_b_1732446u[u];
 
    unsigned char _r = (r < 0 ? 0 : (r > 255 ? 255 : r));      /* R */
    unsigned char _g = (g < 0 ? 0 : (g > 255 ? 255 : g));      /* G */
    unsigned char _b = (b < 0 ? 0 : (b > 255 ? 255 : b));      /* B */

    // return ((_r>>3)<<11)+((_g>>2)<<5)+(_b>>3);
    return ((_r&0xf8)<<8)+((_g&0xfc)<<3)+(_b>>3);
}

MUTEXAUTOLOCK_INIT(ResultMutex);
MUTEXAUTOLOCK_INIT(LCDFlashMutex);

typedef struct {
  SAMPLE_TDL_MW_CONTEXT *pstMWContext;
  cvitdl_service_handle_t stServiceHandle;
} SAMPLE_TDL_VENC_THREAD_ARG_S;

void *run_venc(void *args) {
  printf("Enter encoder thread\n");
  VIDEO_FRAME_INFO_S stFrame;
  // cvtdl_image_t frame_img;
  CVI_S32 s32Ret;

  SAMPLE_TDL_VENC_THREAD_ARG_S *pstArgs = (SAMPLE_TDL_VENC_THREAD_ARG_S *)args;
  cvtdl_face_t stFaceMeta = {0};

  clock_t time[5];

  while (bExit == false) {
    time[0] = clock();//----------------------------------------------------------------
    s32Ret = CVI_VPSS_GetChnFrame(0, 0, &stFrame, 2000);
    if (s32Ret != CVI_SUCCESS) {
      printf("CVI_VPSS_GetChnFrame chn0 failed with %#x\n", s32Ret);
      break;
    }

    {
      MutexAutoLock(ResultMutex, lock);
      CVI_TDL_CopyFaceMeta(&g_stFaceMeta, &stFaceMeta);
    }

    s32Ret = CVI_TDL_Service_FaceDrawRect(pstArgs->stServiceHandle, &stFaceMeta, &stFrame, false,
                                          CVI_TDL_Service_GetDefaultBrush());
    if (s32Ret != CVI_TDL_SUCCESS) {
      CVI_VPSS_ReleaseChnFrame(0, 0, &stFrame);
      printf("Draw fame fail!, ret=%x\n", s32Ret);
      goto error;
    }

    static bool info_flag = true;
    if(info_flag){
      VIDEO_FRAME_S *pstVFrame = &stFrame.stVFrame;

      printf("enc:Width:%d, Height:%d, PixelFormat:%d, BayerFormat:%d, VideoFormat:%d \
              CompressMode:%d DynamicRange:%d FrameFlag:%d\n",
        pstVFrame->u32Width, pstVFrame->u32Height, pstVFrame->enPixelFormat, pstVFrame->enBayerFormat, pstVFrame->enVideoFormat,
        pstVFrame->enCompressMode, pstVFrame->enDynamicRange, pstVFrame->u32FrameFlag);
      printf("u32Stride:%d,%d,%d u64PhyAddr:%ld,%ld,%ld u32Length:%d,%d,%d pu8VirAddr:%p,%p,%p u64PhyAddr:%p,%p,%p\n",
              pstVFrame->u32Stride[0], pstVFrame->u32Stride[1], pstVFrame->u32Stride[2],
              pstVFrame->u64PhyAddr[0], pstVFrame->u64PhyAddr[1], pstVFrame->u64PhyAddr[2],
              pstVFrame->u32Length[0], pstVFrame->u32Length[1], pstVFrame->u32Length[2],
              pstVFrame->pu8VirAddr[0],pstVFrame->pu8VirAddr[1],pstVFrame->pu8VirAddr[2],
              (uint8_t*)pstVFrame->u64PhyAddr[0], (uint8_t*)pstVFrame->u64PhyAddr[1], (uint8_t*)pstVFrame->u64PhyAddr[2]
              );
      info_flag = false;
    }

    // lcd_show_rgb565( fbp, stFrame, 1080, 600);
    // yuv2rgb565( fbp, stFrame, 1080, 600);
    time[1] = clock();//----------------------------------------------------------------
    yuvScale2rgb565( fbp, stFrame);
    time[2] = clock();//----------------------------------------------------------------

    printf("time=%f,%f,%.2f\n", (double)(time[2] - time[0]) / CLOCKS_PER_SEC, (double)(time[2] - time[1]) / CLOCKS_PER_SEC, (double)(time[2] - time[1])/(time[2] - time[0]));
    // s32Ret = SAMPLE_TDL_Send_Frame_RTSP(&stFrame, pstArgs->pstMWContext);
    // if (s32Ret != CVI_SUCCESS) {
    //   CVI_VPSS_ReleaseChnFrame(0, 0, &stFrame);
    //   printf("Send Output Frame NG, ret=%x\n", s32Ret);
    //   goto error;
    // }

  error:
    CVI_TDL_Free(&stFaceMeta);
    CVI_VPSS_ReleaseChnFrame(0, 0, &stFrame);
    // CVI_TDL_FreeImage(&frame_img);
    if (s32Ret != CVI_SUCCESS) {
      bExit = true;
    }
  }
  printf("Exit encoder thread\n");
  pthread_exit(NULL);
}

void *run_tdl_thread(void *pHandle) {
  printf("Enter TDL thread\n");
  cvitdl_handle_t pstTDLHandle = (cvitdl_handle_t)pHandle;

  VIDEO_FRAME_INFO_S stFrame;
  cvtdl_face_t stFaceMeta = {0};

  CVI_S32 s32Ret;
  while (bExit == false) {
    s32Ret = CVI_VPSS_GetChnFrame(0, VPSS_CHN1, &stFrame, 2000);

    if (s32Ret != CVI_SUCCESS) {
      printf("CVI_VPSS_GetChnFrame failed with %#x\n", s32Ret);
      goto get_frame_failed;
    }

    s32Ret = CVI_TDL_ScrFDFace(pstTDLHandle, &stFrame, &stFaceMeta);
    if (s32Ret != CVI_TDL_SUCCESS) {
      printf("inference failed!, ret=%x\n", s32Ret);
      goto inf_error;
    }

    if (stFaceMeta.size != g_size) {
        printf("face count: %d\n", stFaceMeta.size);
        g_size = stFaceMeta.size;
        // goto get_frame_failed;
    }

    {
      MutexAutoLock(ResultMutex, lock);
      CVI_TDL_CopyFaceMeta(&stFaceMeta, &g_stFaceMeta);
    }

  inf_error:
    CVI_VPSS_ReleaseChnFrame(0, 1, &stFrame);
  get_frame_failed:
    CVI_TDL_Free(&stFaceMeta);
    if (s32Ret != CVI_SUCCESS) {
      bExit = true;
    }
  }

  printf("Exit TDL thread\n");
  pthread_exit(NULL);
}

static void SampleHandleSig(CVI_S32 signo) {
  signal(SIGINT, SIG_IGN);
  signal(SIGTERM, SIG_IGN);
  printf("handle signal, signo: %d\n", signo);
  if (SIGINT == signo || SIGTERM == signo) {
    bExit = true;
  }
}

UNUSED static void yuvScale2rgb565(char *dstAddr, VIDEO_FRAME_INFO_S stFrame){
  size_t image_size = stFrame.stVFrame.u32Length[0] + stFrame.stVFrame.u32Length[1] +
                      stFrame.stVFrame.u32Length[2];
  stFrame.stVFrame.pu8VirAddr[0] =
      (uint8_t *)CVI_SYS_Mmap(stFrame.stVFrame.u64PhyAddr[0], image_size);
  stFrame.stVFrame.pu8VirAddr[1] =
      stFrame.stVFrame.pu8VirAddr[0] + stFrame.stVFrame.u32Length[0];
  stFrame.stVFrame.pu8VirAddr[2] =
      stFrame.stVFrame.pu8VirAddr[1] + stFrame.stVFrame.u32Length[1];
  static long src_width = 1920;
  static long dst_height = 270;
  static long dst_width = 480;
  static unsigned short Y,U,V;
  static int y_offset = 25;

  unsigned char *pY, *pUV;
  pY = (unsigned char *)stFrame.stVFrame.pu8VirAddr[0];
  pUV = (unsigned char *)stFrame.stVFrame.pu8VirAddr[1];
  uint16_t * fbp_16 = (uint16_t *) dstAddr;

  static long uv_index0, uv_index1, y_index0, y_index1;
  y_index0 = 0;
  y_index1 = src_width*4;
  uv_index0 = 0;
  uv_index1 = src_width ;
  for (uint16_t iy = 0; iy < dst_height; iy ++) {
    for (uint16_t ix = 0; ix < dst_width; ix ++) {
      Y = U = V = 0;
      Y += ((*(pY + y_index0)    )   );
      // Y += ((*(pY + y_index0 + 3))   );
      // Y += ((*(pY + y_index1)    )   );
      Y += ((*(pY + y_index1 + 3))   );

      V += ((*(pUV + uv_index0)    ) );
      // V += ((*(pUV + uv_index0 + 2)) );
      // V += ((*(pUV + uv_index1)    ) );
      V += ((*(pUV + uv_index1 + 2)) );

      U += ((*(pUV + uv_index0 + 1)) );
      // U += ((*(pUV + uv_index0 + 3)) );
      // U += ((*(pUV + uv_index1 + 1)) );
      U += ((*(pUV + uv_index1 + 3)) );

      Y /= 2; U /=2; V /= 2;

      *(fbp_16 + (iy + y_offset) * dst_width + ix ) = YUV2RGB((uint8_t)Y,(uint8_t)U,(uint8_t)V);

      y_index0 += 4;
      y_index1 += 4;
      uv_index0 += 4;
      uv_index1 += 4;
    }
    y_index0 += src_width;
    y_index1 += src_width;
    uv_index0 += src_width/2;
    uv_index1 += src_width/2;
  }

  CVI_SYS_Munmap((void *)stFrame.stVFrame.pu8VirAddr[0], image_size);
  stFrame.stVFrame.pu8VirAddr[0] = NULL;
  stFrame.stVFrame.pu8VirAddr[1] = NULL;
  stFrame.stVFrame.pu8VirAddr[2] = NULL;
}

UNUSED static void yuv2rgb565(char *dstAddr, VIDEO_FRAME_INFO_S stFrame, int x, int y){
  // return ;
  size_t image_size = stFrame.stVFrame.u32Length[0] + stFrame.stVFrame.u32Length[1] +
                      stFrame.stVFrame.u32Length[2];
  stFrame.stVFrame.pu8VirAddr[0] =
      (uint8_t *)CVI_SYS_Mmap(stFrame.stVFrame.u64PhyAddr[0], image_size);
  stFrame.stVFrame.pu8VirAddr[1] =
      stFrame.stVFrame.pu8VirAddr[0] + stFrame.stVFrame.u32Length[0];
  stFrame.stVFrame.pu8VirAddr[2] =
      stFrame.stVFrame.pu8VirAddr[1] + stFrame.stVFrame.u32Length[1];
  // long src_height = srcImg->height;
  long src_width = 1920;
  long dst_height = 320;
  long dst_width = 480;

  unsigned char Y00,Y01,Y10,Y11,U,V;
  // char R,G,B;
  int x_begin = x, y_begin = y, x_end = dst_width/2,y_end = dst_height/2;//
  long index, y_index;

  uint16_t * fbp_16 = (uint16_t *) dstAddr;
  unsigned char *pY, *pUV;
  pY = (unsigned char *)stFrame.stVFrame.pu8VirAddr[0];
  pUV = (unsigned char *)stFrame.stVFrame.pu8VirAddr[1];
  fbp_16[0] = 0xffff;

  if(x%2 != 0) x_begin--;
  if(y%2 != 0) y_begin--;

  // index = x_begin/2 + y_begin * src_width/4;
  y_index = x_begin + y_begin * src_width ;
  index = y_begin * src_width/4 + x_begin/2;

  uint16_t _x, _y;
  for (uint16_t iy = 0; iy < y_end; iy ++) {
    for (uint16_t ix = 0; ix < x_end; ix ++) {
      Y00 = *(pY + y_index);
      Y01 = *(pY + y_index + 1);
      Y10 = *(pY + y_index + src_width    );
      Y11 = *(pY + y_index + src_width + 1);
      V = *(pUV + index) ;
      U = *(pUV + index + 1) ;
      // R = Y + 1.402* (V - 128); //R
      // G = Y - 0.34413 * (U - 128) - 0.71414 * (V - 128); //G
      // B = Y + 1.772 * (U - 128); //B
      // R = (int)( 1.402 * V); //R
      // G = (int)(- 0.34413 * U - 0.71414 * V); //G
      // B = (int)( 1.772 * U); //B
      _x = ix * 2;
      _y = iy * 2;
      // color_rgb565 = ((*(u8_rgb888_data)>>3)<<11)+((*(u8_rgb888_data+2)>>2)<<5)+(*(u8_rgb888_data+2)>>3);
      *(fbp_16 + _y * dst_width + _x ) = YUV2RGB(Y00,U,V);
      *(fbp_16 + _y * dst_width + _x  + 1) = YUV2RGB(Y01,U,V);
      *(fbp_16 + (_y + 1) * dst_width + _x ) = YUV2RGB(Y10,U,V);
      *(fbp_16 + (_y + 1) * dst_width + _x + 1) = YUV2RGB(Y11,U,V);
      index += 2;
      y_index += 2;
      // printf("%ld,%ld,",index,y_index);
      // if(Y00!=0 || Y01!=0 || Y10!=0 || Y11!=0 || U!= 0|| V!= 0)
      // printf("%x%x%x%x%x%x,%ld,%ld,",Y00,Y01,Y10,Y11,U,V,index,y_index);
    }
      index += (src_width/2 - x_end);
      y_index += (src_width - x_end*2);
      // printf("/n");
  }

  CVI_SYS_Munmap((void *)stFrame.stVFrame.pu8VirAddr[0], image_size);
  stFrame.stVFrame.pu8VirAddr[0] = NULL;
  stFrame.stVFrame.pu8VirAddr[1] = NULL;
  stFrame.stVFrame.pu8VirAddr[2] = NULL;
}

UNUSED static void lcd_show_rgb565(char *dstAddr, VIDEO_FRAME_INFO_S stFrame, int x, int y){
  return ;
  size_t image_size = stFrame.stVFrame.u32Length[0] + stFrame.stVFrame.u32Length[1] +
                      stFrame.stVFrame.u32Length[2];
  stFrame.stVFrame.pu8VirAddr[0] =
      (uint8_t *)CVI_SYS_Mmap(stFrame.stVFrame.u64PhyAddr[0], image_size);
  stFrame.stVFrame.pu8VirAddr[1] =
      stFrame.stVFrame.pu8VirAddr[0] + stFrame.stVFrame.u32Length[0];
  stFrame.stVFrame.pu8VirAddr[2] =
      stFrame.stVFrame.pu8VirAddr[1] + stFrame.stVFrame.u32Length[1];

  // long src_height = srcImg->height;
  // long src_width = 1920;
  // long dst_height = 320;
  long dst_width = 480;

  unsigned char color_rgb565;
  unsigned char *u8_rgb888_data = stFrame.stVFrame.pu8VirAddr[0];
  uint16_t * fbp_16 = (uint16_t *) dstAddr;

  long index = x + y * 1920;
  for (uint16_t iy = 0; iy < 320; iy ++) {
    for (uint16_t ix = 0; ix < 480; ix ++) {
      color_rgb565 = ((u8_rgb888_data[index]&&0xf8)<<8)+((u8_rgb888_data[index + 1]&&0xfc)<<3)+(u8_rgb888_data[index + 2]>>3);
      *(fbp_16 + iy * dst_width + ix ) = color_rgb565;
      index += 3;
      // printf("%2x",color_rgb565);
    }
      // printf("\n");
      index += (1920-480)*3;
  }

  CVI_SYS_Munmap((void *)stFrame.stVFrame.pu8VirAddr[0], image_size);
  stFrame.stVFrame.pu8VirAddr[0] = NULL;
  stFrame.stVFrame.pu8VirAddr[1] = NULL;
  stFrame.stVFrame.pu8VirAddr[2] = NULL;
}
static int lcd_init(){
  fbfd = open("/dev/fb0", O_RDWR);
  if(fbfd == -1) {
      perror("Error: cannot open framebuffer device\n");
      return -1;
  }
  printf("The framebuffer device was opened successfully\n");
  static struct fb_var_screeninfo vinfo;
  static struct fb_fix_screeninfo finfo;
  // Get fixed screen information
  if(ioctl(fbfd, FBIOGET_FSCREENINFO, &finfo) == -1) {
      perror("Error reading fixed information");
      return -1;
  }

  // Get variable screen information
  if(ioctl(fbfd, FBIOGET_VSCREENINFO, &vinfo) == -1) {
      perror("Error reading variable information");
      return -1;
  }
  printf("%dx%d, %dbpp\n", vinfo.xres, vinfo.yres, vinfo.bits_per_pixel);
  int screensize = vinfo.xres*vinfo.yres*vinfo.bits_per_pixel/8;
  fbp = (char *)mmap(0, screensize, PROT_READ | PROT_WRITE, MAP_SHARED, fbfd, 0);
  if((intptr_t)fbp == -1) {
      perror("Error: failed to map framebuffer device to memory");
      return -1;
  }
  return 0;
}

int main(int argc, char *argv[]) {
  if (argc != 2) {
    printf(
        "\nUsage: %s RETINA_MODEL_PATH.\n\n"
        "\tRETINA_MODEL_PATH, path to retinaface model.\n",
        argv[0]);
    return CVI_TDL_FAILURE;
  }

  signal(SIGINT, SampleHandleSig);
  signal(SIGTERM, SampleHandleSig);

  SAMPLE_TDL_MW_CONFIG_S stMWConfig = {0};

  CVI_S32 s32Ret = SAMPLE_TDL_Get_VI_Config(&stMWConfig.stViConfig);//获取输入数据
  if (s32Ret != CVI_SUCCESS || stMWConfig.stViConfig.s32WorkingViNum <= 0) {
    printf("Failed to get senor infomation from ini file (/mnt/data/sensor_cfg.ini).\n");
    return -1;
  }// init senor

  // Get VI size
  PIC_SIZE_E enPicSize;
  s32Ret = SAMPLE_COMM_VI_GetSizeBySensor(stMWConfig.stViConfig.astViInfo[0].stSnsInfo.enSnsType,
                                          &enPicSize);
  if (s32Ret != CVI_SUCCESS) {
    printf("Cannot get senor size\n");
    return -1;
  }

  SIZE_S stSensorSize;
  s32Ret = SAMPLE_COMM_SYS_GetPicSize(enPicSize, &stSensorSize);
  if (s32Ret != CVI_SUCCESS) {
    printf("Cannot get senor size\n");
    return -1;
  }

  // Setup frame size of video encoder to 1080p
  SIZE_S stVencSize = {
      .u32Width = 1920,
      .u32Height = 1080,
  };

  stMWConfig.stVBPoolConfig.u32VBPoolCount = 3;

  // VBPool 0 for VPSS Grp0 Chn0
  // stMWConfig.stVBPoolConfig.astVBPoolSetup[0].enFormat = PIXEL_FORMAT_RGB_888;
  stMWConfig.stVBPoolConfig.astVBPoolSetup[0].enFormat = MY_VI_PIXEL_FORMAT;
  stMWConfig.stVBPoolConfig.astVBPoolSetup[0].u32BlkCount = 3;
  stMWConfig.stVBPoolConfig.astVBPoolSetup[0].u32Height = stSensorSize.u32Height;
  stMWConfig.stVBPoolConfig.astVBPoolSetup[0].u32Width = stSensorSize.u32Width;
  stMWConfig.stVBPoolConfig.astVBPoolSetup[0].bBind = true;
  stMWConfig.stVBPoolConfig.astVBPoolSetup[0].u32VpssChnBinding = VPSS_CHN0;
  stMWConfig.stVBPoolConfig.astVBPoolSetup[0].u32VpssGrpBinding = (VPSS_GRP)0;

  // VBPool 1 for VPSS Grp0 Chn1
  stMWConfig.stVBPoolConfig.astVBPoolSetup[1].enFormat = MY_VI_PIXEL_FORMAT;
  // stMWConfig.stVBPoolConfig.astVBPoolSetup[1].enFormat = PIXEL_FORMAT_RGB_888;
  stMWConfig.stVBPoolConfig.astVBPoolSetup[1].u32BlkCount = 3;
  stMWConfig.stVBPoolConfig.astVBPoolSetup[1].u32Height = stVencSize.u32Height;
  stMWConfig.stVBPoolConfig.astVBPoolSetup[1].u32Width = stVencSize.u32Width;
  // stMWConfig.stVBPoolConfig.astVBPoolSetup[1].u32Height = 320;
  // stMWConfig.stVBPoolConfig.astVBPoolSetup[1].u32Width = 480;
  stMWConfig.stVBPoolConfig.astVBPoolSetup[1].bBind = true;
  stMWConfig.stVBPoolConfig.astVBPoolSetup[1].u32VpssChnBinding = VPSS_CHN1;
  stMWConfig.stVBPoolConfig.astVBPoolSetup[1].u32VpssGrpBinding = (VPSS_GRP)0;

  // VBPool 2 for TDL preprocessing
  stMWConfig.stVBPoolConfig.astVBPoolSetup[2].enFormat = PIXEL_FORMAT_BGR_888_PLANAR;
  stMWConfig.stVBPoolConfig.astVBPoolSetup[2].u32BlkCount = 1;
  stMWConfig.stVBPoolConfig.astVBPoolSetup[2].u32Height = 720;
  stMWConfig.stVBPoolConfig.astVBPoolSetup[2].u32Width = 1280;
  stMWConfig.stVBPoolConfig.astVBPoolSetup[2].bBind = false;

  // Setup VPSS Grp0
  stMWConfig.stVPSSPoolConfig.u32VpssGrpCount = 1;
#ifndef CV186X
  stMWConfig.stVPSSPoolConfig.stVpssMode.aenInput[0] = VPSS_INPUT_MEM;
  stMWConfig.stVPSSPoolConfig.stVpssMode.enMode = VPSS_MODE_DUAL;
  stMWConfig.stVPSSPoolConfig.stVpssMode.ViPipe[0] = 0;
  stMWConfig.stVPSSPoolConfig.stVpssMode.aenInput[1] = VPSS_INPUT_ISP;
  stMWConfig.stVPSSPoolConfig.stVpssMode.ViPipe[1] = 0;
#endif

  SAMPLE_TDL_VPSS_CONFIG_S *pstVpssConfig = &stMWConfig.stVPSSPoolConfig.astVpssConfig[0];
  pstVpssConfig->bBindVI = true;

  // Assign device 1 to VPSS Grp0, because device1 has 3 outputs in dual mode.
  VPSS_GRP_DEFAULT_HELPER2(&pstVpssConfig->stVpssGrpAttr, stSensorSize.u32Width,
                           stSensorSize.u32Height, MY_VI_PIXEL_FORMAT, 1);
  pstVpssConfig->u32ChnCount = 2;
  // pstVpssConfig->u32ChnCount = 3;
  pstVpssConfig->u32ChnBindVI = 0;
  VPSS_CHN_DEFAULT_HELPER(&pstVpssConfig->astVpssChnAttr[0], stVencSize.u32Width,
                          stVencSize.u32Height, MY_VI_PIXEL_FORMAT, true);
  // VPSS_CHN_DEFAULT_HELPER(&pstVpssConfig->astVpssChnAttr[0], stVencSize.u32Width,
  //                         stVencSize.u32Height, PIXEL_FORMAT_RGB_888, true);
  VPSS_CHN_DEFAULT_HELPER(&pstVpssConfig->astVpssChnAttr[1], stVencSize.u32Width,
                          stVencSize.u32Height, MY_VI_PIXEL_FORMAT, true);
  //
  //
  // VPSS_CHN_DEFAULT_HELPER(&pstVpssConfig->astVpssChnAttr[2], 480,
  //                         320, MY_VI_PIXEL_FORMAT, true);

  // Get default VENC configurations
  SAMPLE_TDL_Get_Input_Config(&stMWConfig.stVencConfig.stChnInputCfg);
  stMWConfig.stVencConfig.u32FrameWidth = stVencSize.u32Width;
  stMWConfig.stVencConfig.u32FrameHeight = stVencSize.u32Height;

  // Get default RTSP configurations
  SAMPLE_TDL_Get_RTSP_Config(&stMWConfig.stRTSPConfig.stRTSPConfig);

  SAMPLE_TDL_MW_CONTEXT stMWContext = {0};
  showVpssGrpAttr(pstVpssConfig->stVpssGrpAttr);
  s32Ret = SAMPLE_TDL_Init_WM(&stMWConfig, &stMWContext);
  if (s32Ret != CVI_SUCCESS) {
    printf("init middleware failed! ret=%x\n", s32Ret);
    return -1;
  }

  cvitdl_handle_t stTDLHandle = NULL;

  // Create TDL handle and assign VPSS Grp1 Device 0 to TDL SDK
  GOTO_IF_FAILED(CVI_TDL_CreateHandle2(&stTDLHandle, 1, 0), s32Ret, create_tdl_fail);

  GOTO_IF_FAILED(CVI_TDL_SetVBPool(stTDLHandle, 0, 2), s32Ret, create_service_fail);

  CVI_TDL_SetVpssTimeout(stTDLHandle, 1000);

  cvitdl_service_handle_t stServiceHandle = NULL;
  GOTO_IF_FAILED(CVI_TDL_Service_CreateHandle(&stServiceHandle, stTDLHandle), s32Ret,
                 create_service_fail);

  GOTO_IF_FAILED(CVI_TDL_OpenModel(stTDLHandle, CVI_TDL_SUPPORTED_MODEL_SCRFDFACE, argv[1]), s32Ret,
                 setup_tdl_fail);

  if(lcd_init() == -1) return 0;
  pthread_t stVencThread, stTDLThread;
  SAMPLE_TDL_VENC_THREAD_ARG_S args = {
      .pstMWContext = &stMWContext,
      .stServiceHandle = stServiceHandle,
  };

  pthread_create(&stVencThread, NULL, run_venc, &args);
  // pthread_create(&stVencThread, NULL, run_venc, NULL);
  pthread_create(&stTDLThread, NULL, run_tdl_thread, stTDLHandle);

  pthread_join(stVencThread, NULL);
  pthread_join(stTDLThread, NULL);

setup_tdl_fail:
  CVI_TDL_Service_DestroyHandle(stServiceHandle);
create_service_fail:
  CVI_TDL_DestroyHandle(stTDLHandle);
create_tdl_fail:
  SAMPLE_TDL_Destroy_MW(&stMWContext);

  return 0;
}

UNUSED static void showVpssGrpAttr(VPSS_GRP_ATTR_S stVpssGrpAttr){
  printf("[showVpssGrpAttr] %d, %d, %d, %d, %d, %d\n", 
          stVpssGrpAttr.enPixelFormat, stVpssGrpAttr.stFrameRate.s32SrcFrameRate, stVpssGrpAttr.stFrameRate.s32DstFrameRate,
          stVpssGrpAttr.u32MaxH, stVpssGrpAttr.u32MaxW, stVpssGrpAttr.u8VpssDev);
}

