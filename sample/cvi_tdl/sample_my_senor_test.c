#include "middleware_utils.h"
#include <cvi_comm.h> 
#include <sample_comm.h>
#include <core/utils/vpss_helper.h>

#include <stdio.h>
#include <signal.h>
#include <pthread.h>

#include <stdlib.h>
#include <unistd.h>
#include <stddef.h>
#include <stdio.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/fb.h>
#include <time.h>

#define VPSS_GRP 0
#define UNUSED __attribute__((unused))

char *fbp;
static volatile bool bExit = false;

static void showVpssGrpAttr(VPSS_GRP_ATTR_S stVpssGrpAttr);
UNUSED static void lcd_show_rgb565(char *dstAddr, VIDEO_FRAME_INFO_S stFrame);
UNUSED static void lcd_show_rgb565_384_288(char *dstAddr, VIDEO_FRAME_INFO_S stFrame);

CVI_S32 Sensor_Info_Get( SAMPLE_VI_CONFIG_S *stViConfig, PIC_SIZE_E *enPicSize, SIZE_S *stSensorSize){
    CVI_S32 s32Ret = CVI_SUCCESS;

    // init senor
    s32Ret = SAMPLE_TDL_Get_VI_Config(stViConfig);//获取输入数据
    if (s32Ret != CVI_SUCCESS || stViConfig->s32WorkingViNum <= 0) {
        printf("Failed to get senor infomation from ini file (/mnt/data/sensor_cfg.ini).\n");
        return s32Ret;
    }
    
    // Get VI size
    s32Ret = SAMPLE_COMM_VI_GetSizeBySensor( stViConfig->astViInfo[0].stSnsInfo.enSnsType, enPicSize);
    if (s32Ret != CVI_SUCCESS) {
        printf("Cannot get senor size\n");
        return s32Ret;
    }
    
    s32Ret = SAMPLE_COMM_SYS_GetPicSize( *enPicSize, stSensorSize);
    if (s32Ret != CVI_SUCCESS) {
        printf("Cannot get senor size\n");
        return s32Ret;
    }

    return s32Ret;
}
static int lcd_init(){
  int fbfd = open("/dev/fb0", O_RDWR);
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
static void *getDstImg() {
    printf("Enter get dst img thread\n");
    VIDEO_FRAME_INFO_S stFrame;
    CVI_S32 s32Ret;

    lcd_init();
    clock_t time[5];
    while (bExit == false) {
        time[0] = clock();//----------------------------------------------------------------
        s32Ret = CVI_VPSS_GetChnFrame(VPSS_GRP, VPSS_CHN1, &stFrame, 2000);
            if (s32Ret != CVI_SUCCESS) {
            printf("CVI_VPSS_GetChnFrame chn1 failed with %#x\n", s32Ret);
            break;
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
        if (s32Ret != CVI_SUCCESS) {
            goto error;
        }

        time[1] = clock();//----------------------------------------------------------------
        lcd_show_rgb565_384_288(fbp,stFrame);
        time[2] = clock();//----------------------------------------------------------------
        printf("time:%f, show time:%f, precent:%.2f\n", (double)(time[2] - time[0]) / CLOCKS_PER_SEC, (double)(time[2] - time[1]) / CLOCKS_PER_SEC, (double)(time[2] - time[1])/(time[2] - time[0]));

    error:
        // CVI_VPSS_ReleaseChnFrame( VPSS_GRP, VPSS_CHN0, &stFrame);
        CVI_VPSS_ReleaseChnFrame( VPSS_GRP, VPSS_CHN1, &stFrame);
        // CVI_TDL_FreeImage(&frame_img);
        if (s32Ret != CVI_SUCCESS) {
            bExit = true;
        }
    }
  printf("Exit get dst img thread\n");
  pthread_exit(NULL);
}
static void *getSrcImg() {
    printf("Enter get src img thread\n");
    VIDEO_FRAME_INFO_S stFrame;
    CVI_S32 s32Ret;
    while (bExit == false) {
        s32Ret = CVI_VPSS_GetChnFrame(VPSS_GRP, VPSS_CHN0, &stFrame, 2000);
            if (s32Ret != CVI_SUCCESS) {
            printf("CVI_VPSS_GetChnFrame chn0 failed with %#x\n", s32Ret);
            break;
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
        if (s32Ret != CVI_SUCCESS) {
            goto error;
        }

    error:
        // CVI_VPSS_ReleaseChnFrame( VPSS_GRP, VPSS_CHN0, &stFrame);
        CVI_VPSS_ReleaseChnFrame( VPSS_GRP, VPSS_CHN0, &stFrame);
        // CVI_TDL_FreeImage(&frame_img);
        if (s32Ret != CVI_SUCCESS) {
            bExit = true;
        }
    }
  printf("Exit get src img thread\n");
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
UNUSED static void lcd_show_rgb565_384_288(char *dstAddr, VIDEO_FRAME_INFO_S stFrame){
  size_t image_size = stFrame.stVFrame.u32Length[0] + stFrame.stVFrame.u32Length[1] +
                      stFrame.stVFrame.u32Length[2];
  stFrame.stVFrame.pu8VirAddr[0] =
      (uint8_t *)CVI_SYS_Mmap(stFrame.stVFrame.u64PhyAddr[0], image_size);
  stFrame.stVFrame.pu8VirAddr[1] =
      stFrame.stVFrame.pu8VirAddr[0] + stFrame.stVFrame.u32Length[0];
  stFrame.stVFrame.pu8VirAddr[2] =
      stFrame.stVFrame.pu8VirAddr[1] + stFrame.stVFrame.u32Length[1];
      
  UNUSED unsigned short color_rgb565;
  UNUSED unsigned char *u8_rgb888_data = stFrame.stVFrame.pu8VirAddr[0];
  UNUSED uint16_t * fbp_16 = (uint16_t *) dstAddr;
    /* 384 * 288 */
  long index = 0;
  long lcd_index = 0;
  for (uint16_t iy = 0; iy < 288; iy ++) {
    for (uint16_t ix = 0; ix < 384; ix ++) {
        color_rgb565 = ((u8_rgb888_data[index]&0xf8)<<8)|((u8_rgb888_data[index + 1]&0xfc)<<3)|((u8_rgb888_data[index + 2]&0xf8)>>3);
        fbp_16[lcd_index] = color_rgb565;
        lcd_index++;
        index += 3;
    }
    lcd_index += (480-384);
  }

  CVI_SYS_Munmap((void *)stFrame.stVFrame.pu8VirAddr[0], image_size);
  stFrame.stVFrame.pu8VirAddr[0] = NULL;
  stFrame.stVFrame.pu8VirAddr[1] = NULL;
  stFrame.stVFrame.pu8VirAddr[2] = NULL;

}
UNUSED static void lcd_show_rgb565(char *dstAddr, VIDEO_FRAME_INFO_S stFrame){
  size_t image_size = stFrame.stVFrame.u32Length[0] + stFrame.stVFrame.u32Length[1] +
                      stFrame.stVFrame.u32Length[2];
  stFrame.stVFrame.pu8VirAddr[0] =
      (uint8_t *)CVI_SYS_Mmap(stFrame.stVFrame.u64PhyAddr[0], image_size);
  stFrame.stVFrame.pu8VirAddr[1] =
      stFrame.stVFrame.pu8VirAddr[0] + stFrame.stVFrame.u32Length[0];
  stFrame.stVFrame.pu8VirAddr[2] =
      stFrame.stVFrame.pu8VirAddr[1] + stFrame.stVFrame.u32Length[1];

  int dst_width = 480;
  UNUSED int err = 640*3 - dst_width*3;

  UNUSED unsigned short color_rgb565;
  UNUSED unsigned char *u8_rgb888_data = stFrame.stVFrame.pu8VirAddr[0];
  UNUSED uint16_t * fbp_16 = (uint16_t *) dstAddr;

  static int y_offset = 25;
  static int src_offset = 45;

  long index = src_offset * 640 * 3;
  long lcd_index = y_offset* dst_width;
  for (uint16_t iy = src_offset; iy < 480 - src_offset; iy ++) {
    if(iy%4 != 2){
        for (uint16_t ix = 0; ix < 640; ix ++) {
            if(ix%4 != 2){
                color_rgb565 = ((u8_rgb888_data[index]&0xf8)<<8)+((u8_rgb888_data[index + 1]&0xfc)<<3)+((u8_rgb888_data[index + 2]&0xf8)>>3);
                fbp_16[lcd_index] = color_rgb565;
                lcd_index++;
            }
            index += 3;
        }
    }else{
        index += 640*3;
    }
  }
  printf("lcd index cnt:%ld, index cnt:%ld\n", lcd_index, index);

  CVI_SYS_Munmap((void *)stFrame.stVFrame.pu8VirAddr[0], image_size);
  stFrame.stVFrame.pu8VirAddr[0] = NULL;
  stFrame.stVFrame.pu8VirAddr[1] = NULL;
  stFrame.stVFrame.pu8VirAddr[2] = NULL;
}

int main(int argc, char *argv[]){

    signal(SIGINT, SampleHandleSig);
    signal(SIGTERM, SampleHandleSig);

    CVI_S32 s32Ret;

    MMF_VERSION_S stVersion;
    CVI_SYS_GetVersion(&stVersion);
    printf("MMF Version:%s\n", stVersion.version);

    SAMPLE_VI_CONFIG_S stViConfig;
    PIC_SIZE_E enPicSize;
    SIZE_S stSensorSize;
    SIZE_S dstSize = { 
        .u32Width = 384,
        .u32Height = 288
    };

    s32Ret = Sensor_Info_Get( &stViConfig, &enPicSize, &stSensorSize);
    if (s32Ret != CVI_SUCCESS) {
        printf("sensor info get failed with %#x!\n" ,s32Ret);
        return -1;
    }else{
        printf("sensor info get success!\n");
    }
    CVI_VI_SetDevNum(stViConfig.s32WorkingViNum);

    VB_CONFIG_S stVbConf;
    CVI_U32 u32BlkSize;
    CVI_U32 u32TotalBlkSize = 0;
    memset( &stVbConf, 0, sizeof(VB_CONFIG_S));

    stVbConf.u32MaxPoolCnt = 2;
    u32BlkSize = COMMON_GetPicBufferSize(stSensorSize.u32Width, stSensorSize.u32Height, PIXEL_FORMAT_NV21, DATA_BITWIDTH_8,
                                        COMPRESS_MODE_NONE, DEFAULT_ALIGN);
    stVbConf.astCommPool[0].u32BlkSize = u32BlkSize;
    stVbConf.astCommPool[0].u32BlkCnt = 3;
    u32TotalBlkSize += (stVbConf.astCommPool[0].u32BlkSize * stVbConf.astCommPool[0].u32BlkCnt);

    u32BlkSize = COMMON_GetPicBufferSize(dstSize.u32Width, dstSize.u32Height, PIXEL_FORMAT_RGB_888, DATA_BITWIDTH_8,
                                        COMPRESS_MODE_NONE, DEFAULT_ALIGN);
    stVbConf.astCommPool[1].u32BlkSize = u32BlkSize;
    stVbConf.astCommPool[1].u32BlkCnt = 1;
    u32TotalBlkSize += (stVbConf.astCommPool[1].u32BlkSize * stVbConf.astCommPool[1].u32BlkCnt);

    printf("Total memory of VB pool: %u bytes\n", u32TotalBlkSize);

#ifndef _MIDDLEWARE_V3_
    s32Ret = SAMPLE_COMM_SYS_Init(&stVbConf);
    if (s32Ret != CVI_SUCCESS){
        printf("system init failed with %#x!\n", s32Ret);
        return -1;
    }else{
        printf("system init success!\n");
    }
#endif
    
    printf("Initialize VI\n");
    VI_VPSS_MODE_S stVIVPSSMode;
    // stVIVPSSMode.aenMode[0] = VI_OFFLINE_VPSS_ONLINE;
    stVIVPSSMode.aenMode[0] = VI_ONLINE_VPSS_ONLINE;
    CVI_SYS_SetVIVPSSMode(&stVIVPSSMode);

    s32Ret = SAMPLE_PLAT_VI_INIT(&stViConfig);
    if (s32Ret != CVI_SUCCESS) {
        printf("vi init failed. s32Ret: 0x%x !\n", s32Ret);
        goto vi_start_error;
    }

    printf("Initialize VPSS\n");
#ifndef CV186X
    VPSS_MODE_S stVPSSMode = {
        .aenInput[0] = VPSS_INPUT_MEM   ,
        .enMode = VPSS_MODE_DUAL        ,
        .ViPipe[0] = 0                  ,
        .aenInput[1] = VPSS_INPUT_ISP   ,
        .ViPipe[1] = 0
    };
    CVI_SYS_SetVPSSModeEx(&stVPSSMode);
#endif
    VPSS_GRP_ATTR_S stVpssGrpAttr       ;
    VPSS_CHN_ATTR_S astVpssChnAttr[VPSS_MAX_PHY_CHN_NUM]   ;
    {
        // Assign device 1 to VPSS Grp0, because device1 has 3 outputs in dual mode.
        VPSS_GRP_DEFAULT_HELPER2(&stVpssGrpAttr, stSensorSize.u32Width, stSensorSize.u32Height, PIXEL_FORMAT_NV21, 1);
        VPSS_CHN_DEFAULT_HELPER(&astVpssChnAttr[0], stSensorSize.u32Width, stSensorSize.u32Height, PIXEL_FORMAT_NV21, true);
        VPSS_CHN_DEFAULT_HELPER(&astVpssChnAttr[1], dstSize.u32Width, dstSize.u32Height, PIXEL_FORMAT_RGB_888, true);
    }
    showVpssGrpAttr(stVpssGrpAttr);
    CVI_BOOL abChnEnable[VPSS_MAX_PHY_CHN_NUM + 1] = { true, true, 0, 0};
    {
        CVI_VPSS_DestroyGrp(VPSS_GRP);
        s32Ret = SAMPLE_COMM_VPSS_Init(VPSS_GRP, abChnEnable, &stVpssGrpAttr, astVpssChnAttr);
        if (s32Ret != CVI_SUCCESS) {
            printf("init vpss group failed. s32Ret: 0x%x !\n", s32Ret);
            goto vpss_start_error;
        }

        s32Ret = SAMPLE_COMM_VPSS_Start(VPSS_GRP, abChnEnable, &stVpssGrpAttr, astVpssChnAttr);
        if (s32Ret != CVI_SUCCESS) {
            printf("start vpss group failed. s32Ret: 0x%x !\n", s32Ret);
            goto vpss_start_error;
        }

        s32Ret = SAMPLE_COMM_VI_Bind_VPSS( 0, VPSS_CHN0, VPSS_GRP);
        if (s32Ret != CVI_SUCCESS) {
            printf("vi bind vpss failed. s32Ret: 0x%x !\n", s32Ret);
            goto vpss_start_error;
        }

    }

    { 
        CVI_U32 iVBPoolIndex = 0;
         // Attach VB to VPSS
        printf("Attach VBPool(%u) to VPSS Grp(%u) Chn(%u)\n", iVBPoolIndex, VPSS_GRP, VPSS_CHN0);
            s32Ret = CVI_VPSS_AttachVbPool(VPSS_GRP, VPSS_CHN0, (VB_POOL)iVBPoolIndex);
            if (s32Ret != CVI_SUCCESS) {
                printf("Cannot attach VBPool(%u) to VPSS Grp(%u) Chn(%u): ret=%x\n", iVBPoolIndex, VPSS_GRP, iVBPoolIndex, s32Ret);
                goto vpss_start_error;
            }
        
        iVBPoolIndex++;
        printf("Attach VBPool(%u) to VPSS Grp(%u) Chn(%u)\n", iVBPoolIndex, VPSS_GRP, VPSS_CHN1);
        s32Ret = CVI_VPSS_AttachVbPool(VPSS_GRP, VPSS_CHN1, (VB_POOL)iVBPoolIndex);
        if (s32Ret != CVI_SUCCESS) {
            printf("Cannot attach VBPool(%u) to VPSS Grp(%u) Chn(%u): ret=%x\n", iVBPoolIndex, VPSS_GRP, iVBPoolIndex, s32Ret);
            goto vpss_start_error;
        }
    }

    pthread_t getDstImgThread, getSrcImgThread;
    pthread_create(&getDstImgThread, NULL, getDstImg, NULL);
    pthread_create(&getSrcImgThread, NULL, getSrcImg, NULL);

    pthread_join(getDstImgThread, NULL);
    pthread_join(getSrcImgThread, NULL);
    
    SAMPLE_COMM_VI_DestroyIsp(&stViConfig);
    SAMPLE_COMM_VI_DestroyVi(&stViConfig);
    SAMPLE_COMM_VPSS_Stop(VPSS_GRP, abChnEnable);

    printf("sys exit\n");
    CVI_SYS_Exit();
    CVI_VB_Exit();

    return 0;

vpss_start_error:
    SAMPLE_COMM_VI_DestroyIsp(&stViConfig);
    SAMPLE_COMM_VI_DestroyVi(&stViConfig);
    SAMPLE_COMM_VPSS_Stop(VPSS_GRP, abChnEnable);

vi_start_error:
#ifndef _MIDDLEWARE_V3_
    printf("vi start error\n");
    SAMPLE_COMM_SYS_Exit();
#endif

    return -1;

}

static void showVpssGrpAttr(VPSS_GRP_ATTR_S stVpssGrpAttr){
  printf("[showVpssGrpAttr] %d, %d, %d, %d, %d, %d\n", 
          stVpssGrpAttr.enPixelFormat, stVpssGrpAttr.stFrameRate.s32SrcFrameRate, stVpssGrpAttr.stFrameRate.s32DstFrameRate,
          stVpssGrpAttr.u32MaxH, stVpssGrpAttr.u32MaxW, stVpssGrpAttr.u8VpssDev);
}