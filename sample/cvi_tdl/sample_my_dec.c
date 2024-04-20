#define LOG_TAG "SampleFD"
#define LOG_LEVEL LOG_LEVEL_TRACE

#include "middleware_utils.h"
#include "sample_utils.h"
#include "vi_vo_utils.h"

#include <core/utils/vpss_helper.h>
#include <cvi_comm.h>
#include <rtsp.h>
#include <sample_comm.h>
#include "cvi_tdl.h"
#include "cvi_vdec.h"

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <stddef.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <time.h>

    #include "libavutil/adler32.h"
    #include "libavcodec/avcodec.h"
    #include "libavformat/avformat.h"
    #include "libavutil/imgutils.h"
    #include "libavutil/timestamp.h"
    #include "libswscale/swscale.h"

#define VPSS_GRP0 0
#define VDEC_CHN0 0
#define VDEC_STREAM_MODE VIDEO_MODE_FRAME
#define VDEC_EN_TYPE PT_H264
#define VDEC_PIXEL_FORMAT PIXEL_FORMAT_NV21
// #define VDEC_PIXEL_FORMAT PIXEL_FORMAT_RGB_888
#define _UNUSED __attribute__((unused))

static volatile bool bStopCtl = false;
static char *fbp;

static _UNUSED int lcd_init(){
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
static void SampleHandleSig(CVI_S32 signo) {
  signal(SIGINT, SIG_IGN);
  signal(SIGTERM, SIG_IGN);
  printf("handle signal, signo: %d\n", signo);
  if (SIGINT == signo || SIGTERM == signo) {
    bStopCtl = true;
  }
}
static CVI_S32 setVdecChnAttr(VDEC_CHN_ATTR_S *pstChnAttr,VDEC_CHN VdecChn,SIZE_S srcSize){
	VDEC_CHN_PARAM_S stChnParam;

	pstChnAttr->enType = VDEC_EN_TYPE		;
	pstChnAttr->enMode = VDEC_STREAM_MODE	;
	pstChnAttr->u32PicHeight = srcSize.u32Height			;
	pstChnAttr->u32PicWidth = srcSize.u32Width  			;
	pstChnAttr->u32StreamBufSize = ALIGN(pstChnAttr->u32PicHeight * pstChnAttr->u32PicWidth, 0x4000);
	CVI_VDEC_MEM("u32StreamBufSize = 0x%X\n", pstChnAttr->u32StreamBufSize);
	pstChnAttr->u32FrameBufCnt = 3			;//参考帧+显示帧+1
	CVI_VDEC_TRACE("VdecChn = %d\n", VdecChn)	;

	CHECK_CHN_RET(CVI_VDEC_CreateChn(VdecChn, pstChnAttr), VdecChn, "CVI_VDEC_SetChnAttr");
	printf("CVI_VDEC_SetChnAttr success\n");
	CHECK_CHN_RET(CVI_VDEC_GetChnParam(VdecChn, &stChnParam), VdecChn, "CVI_VDEC_GetChnParam");
	printf("CVI_VDEC_GetChnParam success\n");

	stChnParam.enPixelFormat = VDEC_PIXEL_FORMAT;
	stChnParam.enType = VDEC_EN_TYPE				;
	stChnParam.u32DisplayFrameNum = 1		;
	CHECK_CHN_RET(CVI_VDEC_SetChnParam(VdecChn, &stChnParam), VdecChn, "CVI_MPI_VDEC_GetChnParam");
	printf("CVI_MPI_VDEC_GetChnParam success\n");
	CHECK_CHN_RET(CVI_VDEC_StartRecvStream(VdecChn), VdecChn, "CVI_MPI_VDEC_StartRecvStream");
	printf("CVI_MPI_VDEC_StartRecvStream success\n");
	
	return CVI_SUCCESS;
}
static CVI_S32 attachVdecVBPool(VB_POOL_CONFIG_S *stVbPoolCfg/*, VB_POOL *vbPoolId*/){
	CVI_S32 s32Ret = CVI_SUCCESS;
	SAMPLE_VDEC_BUF astSampleVdecBuf[VDEC_MAX_CHN_NUM];

    VDEC_MOD_PARAM_S stModParam;
    CVI_VDEC_GetModParam(&stModParam);
    stModParam.enVdecVBSource = VB_SOURCE_COMMON;
    CVI_VDEC_SetModParam(&stModParam);

	astSampleVdecBuf[0].u32PicBufSize =
				VDEC_GetPicBufferSize( VDEC_EN_TYPE, 1920, 1080,
						VDEC_PIXEL_FORMAT,
						DATA_BITWIDTH_8,
						COMPRESS_MODE_NONE);

	memset(stVbPoolCfg, 0, sizeof(VB_POOL_CONFIG_S));
	stVbPoolCfg->u32BlkSize	= astSampleVdecBuf[0].u32PicBufSize;
	stVbPoolCfg->u32BlkCnt	= 3;
	stVbPoolCfg->enRemapMode = VB_REMAP_MODE_NONE;
	// *vbPoolId  = CVI_VB_CreatePool(&stVbPoolCfg);
	// CVI_VDEC_TRACE("CVI_VDEC_VB_CreatePool : %d, u32BlkSize=0x%x, u32BlkCnt=%d\n",
	// 	*vbPoolId, stVbPoolCfg->u32BlkSize, stVbPoolCfg->u32BlkCnt);
	// if (*vbPoolId == VB_INVALID_POOLID) {
	// 	CVI_VDEC_ERR("CVI_VB_CreatePool Fail\n");
	// 	return CVI_FAILURE;
	// }

	return s32Ret;
}
static CVI_S32 VBPool_Init(SIZE_S chn0Size, SIZE_S chn1Size){
	CVI_S32 s32Ret;
	CVI_U32 u32BlkSize;
    VB_CONFIG_S stVbConf;
    memset( &stVbConf, 0, sizeof(VB_CONFIG_S));
    stVbConf.u32MaxPoolCnt = 3;
    u32BlkSize = COMMON_GetPicBufferSize(chn0Size.u32Width, chn0Size.u32Height, VDEC_PIXEL_FORMAT, DATA_BITWIDTH_8,
                                        COMPRESS_MODE_NONE, DEFAULT_ALIGN);
    stVbConf.astCommPool[0].u32BlkSize = u32BlkSize;
    stVbConf.astCommPool[0].u32BlkCnt = 3;

    u32BlkSize = COMMON_GetPicBufferSize(chn1Size.u32Width, chn1Size.u32Height, PIXEL_FORMAT_RGB_888, DATA_BITWIDTH_8,
                                        COMPRESS_MODE_NONE, DEFAULT_ALIGN);
    stVbConf.astCommPool[1].u32BlkSize = u32BlkSize;
    stVbConf.astCommPool[1].u32BlkCnt = 3;

    attachVdecVBPool(&stVbConf.astCommPool[2]);

    s32Ret = SAMPLE_COMM_SYS_Init(&stVbConf);
    if (s32Ret != CVI_SUCCESS){
        printf("system init failed with %#x!\n", s32Ret);
        return CVI_FAILURE;
    }else{
        printf("system init success!\n");
    }
	return CVI_SUCCESS;
}
static CVI_S32 setVpssGrp( VPSS_GRP VpssGrp, CVI_BOOL *abChnEnable, SIZE_S srcSize, SIZE_S dstSize){
	CVI_S32 s32Ret;
    VPSS_GRP_ATTR_S stVpssGrpAttr       ;
    memset(&stVpssGrpAttr,0,sizeof(VPSS_GRP_ATTR_S));
    VPSS_CHN_ATTR_S astVpssChnAttr[VPSS_MAX_PHY_CHN_NUM]   ;
	VPSS_GRP_DEFAULT_HELPER2(&stVpssGrpAttr, srcSize.u32Width, srcSize.u32Height, VDEC_PIXEL_FORMAT, 1);
	VPSS_CHN_DEFAULT_HELPER(&astVpssChnAttr[0], srcSize.u32Width, srcSize.u32Height, PIXEL_FORMAT_NV21, true);
	VPSS_CHN_DEFAULT_HELPER(&astVpssChnAttr[1], dstSize.u32Width, dstSize.u32Height, PIXEL_FORMAT_RGB_888, true);

	// astVpssChnAttr[0].stFrameRate.s32DstFrameRate = 30;
	// astVpssChnAttr[1].stFrameRate.s32DstFrameRate = 30;

	CVI_VPSS_DestroyGrp(VpssGrp);
	s32Ret = SAMPLE_COMM_VPSS_Init(VpssGrp, abChnEnable, &stVpssGrpAttr, astVpssChnAttr);
	if (s32Ret != CVI_SUCCESS) {
		printf("init vpss group failed. s32Ret: 0x%x !\n", s32Ret);
		return CVI_FAILURE;
		// goto vpss_start_error;
	}

	s32Ret = SAMPLE_COMM_VPSS_Start(VpssGrp, abChnEnable, &stVpssGrpAttr, astVpssChnAttr);
	if (s32Ret != CVI_SUCCESS) {
		printf("start vpss group failed. s32Ret: 0x%x !\n", s32Ret);
		return CVI_FAILURE;
		// goto vpss_start_error;
	}

	MMF_CHN_S stSrcChn = {
		.enModId = CVI_ID_VDEC,
		.s32DevId = 0,
		.s32ChnId = VDEC_CHN0
	};
	MMF_CHN_S stDestChn = {
		.enModId = CVI_ID_VPSS,
		.s32DevId = VpssGrp,
		.s32ChnId = 0
	};
	s32Ret = CVI_SYS_Bind(&stSrcChn, &stDestChn);
	if (s32Ret != CVI_SUCCESS) {
		printf("vpss group blind failed. s32Ret: 0x%x !\n", s32Ret);
		return CVI_FAILURE;
	}else{
        printf("vpss group blind success!\n");
    }
    // s32Ret = CVI_SYS_GetBindbyDest(&stDestChn,&stSrcChn);
	// if (s32Ret == CVI_SUCCESS) {
    //     printf("SYS BIND INFO:%d %d %d",stSrcChn.enModId,stSrcChn.s32DevId,stSrcChn.s32ChnId);
    // }

	return CVI_SUCCESS;
}
static CVI_S32 setVdecThreadParm(VDEC_THREAD_PARAM_S *vdecThreadParm, char *filePath, VDEC_CHN VdecChn){
	memset(vdecThreadParm,0,sizeof(VDEC_THREAD_PARAM_S));
	snprintf(vdecThreadParm->cFileName, sizeof(vdecThreadParm->cFileName), "%s", filePath);
	vdecThreadParm->s32ChnId 		= VdecChn			;
	vdecThreadParm->s32MinBufSize 	= 1024*20      		;
	vdecThreadParm->s32StreamMode 	= VDEC_STREAM_MODE	;
	vdecThreadParm->eThreadCtrl 	= THREAD_CTRL_PAUSE	;
	vdecThreadParm->s32MilliSec 	= -1				;//阻塞模式
	vdecThreadParm->u64PtsInit		= 0					;
    vdecThreadParm->s32IntervalTime = 33*1000           ;
	return CVI_SUCCESS;
}
_UNUSED static void *sendFrame();
_UNUSED static CVI_VOID startVdecFrameSendThread(pthread_t *pVdecThread,VDEC_THREAD_PARAM_S *pstVdecSend)
{
	struct sched_param param;
	pthread_attr_t attr;

	param.sched_priority = 80;
	pthread_attr_init(&attr);
	pthread_attr_setschedpolicy(&attr, SCHED_RR);
	pthread_attr_setschedparam(&attr, &param);
	pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
	pthread_create(pVdecThread, &attr, sendFrame, (CVI_VOID *)pstVdecSend);
    printf("enter send frame thread, path:%s\n",pstVdecSend->cFileName);
}
_UNUSED static void lcd_show_rgb565_384_288(char *dstAddr, VIDEO_FRAME_INFO_S stFrame){
  size_t image_size = stFrame.stVFrame.u32Length[0] + stFrame.stVFrame.u32Length[1] +
                      stFrame.stVFrame.u32Length[2];
  stFrame.stVFrame.pu8VirAddr[0] =
      (uint8_t *)CVI_SYS_Mmap(stFrame.stVFrame.u64PhyAddr[0], image_size);
  stFrame.stVFrame.pu8VirAddr[1] =
      stFrame.stVFrame.pu8VirAddr[0] + stFrame.stVFrame.u32Length[0];
  stFrame.stVFrame.pu8VirAddr[2] =
      stFrame.stVFrame.pu8VirAddr[1] + stFrame.stVFrame.u32Length[1];
      
  _UNUSED unsigned short color_rgb565;
  _UNUSED unsigned char *u8_rgb888_data = stFrame.stVFrame.pu8VirAddr[0];
  _UNUSED uint16_t * fbp_16 = (uint16_t *) dstAddr;
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
static void *getDstImg() {
    printf("Enter get dst img thread\n");
    VIDEO_FRAME_INFO_S stFrame;
    CVI_S32 s32Ret;

    lcd_init();
    clock_t time[5];
    while (bStopCtl == false) {
        time[0] = clock();//----------------------------------------------------------------
        s32Ret = CVI_VPSS_GetChnFrame(VPSS_GRP0, VPSS_CHN1, &stFrame, 2000);
		if (s32Ret == 0xc006800e) {
            // printf("CVI_VPSS_GetChnFrame chn1 failed with %#x\n", s32Ret);
            // printf("wait 100ms\n");
			usleep(1000);
            // break;
			continue;
		}else if (s32Ret != CVI_SUCCESS) {
            printf("CVI_VPSS_GetChnFrame chn1 failed with %#x\n", s32Ret);
            break;
        }
    
        static bool info_flag = true;
        if(info_flag){
            VIDEO_FRAME_S *pstVFrame = &stFrame.stVFrame;

            printf("CHN1:Width:%d, Height:%d, PixelFormat:%d, BayerFormat:%d, VideoFormat:%d \
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
        // CVI_VPSS_ReleaseChnFrame( VPSS_GRP0, VPSS_CHN0, &stFrame);
        CVI_VPSS_ReleaseChnFrame( VPSS_GRP0, VPSS_CHN1, &stFrame);
        // CVI_TDL_FreeImage(&frame_img);
        if (s32Ret != CVI_SUCCESS) {
            bStopCtl = true;
        }
    }
  printf("Exit get dst img thread\n");
  pthread_exit(NULL);
}
static void *getSrcImg() {
    printf("Enter get src img thread\n");
    VIDEO_FRAME_INFO_S stFrame;
    CVI_S32 s32Ret;
    while (bStopCtl == false) {
        s32Ret = CVI_VPSS_GetChnFrame(VPSS_GRP0, VPSS_CHN0, &stFrame, 2000);
		if (s32Ret == 0xc006800e) {
            // printf("CVI_VPSS_GetChnFrame chn0 failed with %#x\n", s32Ret);
            // printf("wait 100ms\n");
			usleep(1000);
            // break;
			continue;
		}else if (s32Ret != CVI_SUCCESS) {
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
        CVI_VPSS_ReleaseChnFrame( VPSS_GRP0, VPSS_CHN0, &stFrame);
        // CVI_TDL_FreeImage(&frame_img);
        if (s32Ret != CVI_SUCCESS) {
            bStopCtl = true;
        }
    }
  printf("Exit get src img thread\n");
  pthread_exit(NULL);
}
static _UNUSED void *getDecImg() {
	// lcd_init();
    printf("Enter get dec img thread\n");
    VIDEO_FRAME_INFO_S stFrame;
    CVI_S32 s32Ret;
    static int cnt = 0;
    clock_t time,time_tmp;
    time = clock();
    while (bStopCtl == false) {
		s32Ret = CVI_VDEC_GetFrame( VDEC_CHN0, &stFrame, -1);
		if (s32Ret == 0xc0058041) {
            // printf("CVI_DEC_GetChnFrame chn1 failed with %#x\n", s32Ret);
            // printf("wait next frame \n");
			usleep(1000);
			continue;
		}else if (s32Ret != CVI_SUCCESS) {
            printf("CVI_DEC_GetChnFrame chn0 failed with %#x\n", s32Ret);
            break;
        }
        if(1){
            time_tmp = clock();
            while(time_tmp - time <= 333){
                time_tmp = clock();
                usleep(100);
            }
            time = time_tmp;
    sendFrame:
            s32Ret = CVI_VPSS_SendFrame( VPSS_GRP0, &stFrame, -1);//手动发送解码帧
            // s32Ret = CVI_SUCCESS;
            if (s32Ret == 0xc0068012) {//busy
                usleep(100);
                goto sendFrame;
            }else if (s32Ret != CVI_SUCCESS) {
                printf("CVI_VPSS_SendFrame to VPSS0 failed with %#x\n", s32Ret);
                CVI_VDEC_ReleaseFrame( VDEC_CHN0, &stFrame);
                break;
            }
        }

        static bool info_flag = true;
        if(info_flag){
            VIDEO_FRAME_S *pstVFrame = &stFrame.stVFrame;

            printf("DEC:Width:%d, Height:%d, PixelFormat:%d, BayerFormat:%d, VideoFormat:%d \
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
        cnt++;
        // printf("dec frame:%d\n",cnt);
		s32Ret = CVI_VDEC_ReleaseFrame( VDEC_CHN0, &stFrame);
		if (s32Ret != CVI_SUCCESS) {
			CVI_VDEC_ERR("chn %d CVI_MPI_VDEC_ReleaseFrame fail for s32Ret=0x%x!\n",
					VDEC_CHN0, s32Ret);
		}
	}
	printf("Exit get dec img thread\n");
	pthread_exit(NULL);
}
_UNUSED static void *sendFrame(CVI_VOID *pArgs){

    // CVI_S32 s32Ret;
    int _sendStream2Dec(AVPacket *pkt,CVI_BOOL bEndOfStream){
        static CVI_S32 s32Ret;
        static VDEC_STREAM_S stStream;
        stStream.u32Len     = pkt->size ;
        stStream.pu8Addr    = pkt->data ;
        stStream.u64PTS     = pkt->pts  ;

        stStream.bDisplay       = true          ;
        stStream.bEndOfFrame    = true          ;
        stStream.bEndOfStream   = bEndOfStream  ;
        if(bEndOfStream){
            stStream.u32Len     = 0 ;
        }
SendAgain:
        s32Ret = CVI_VDEC_SendStream(VDEC_CHN0,&stStream,-1);
        if(s32Ret != CVI_SUCCESS){
			usleep(1000);//1ms
            goto SendAgain;
        }
        return 1;
    }

	VDEC_THREAD_PARAM_S *pstVdecThreadParam = (VDEC_THREAD_PARAM_S *)pArgs;

    const AVCodec *codec = NULL;
    AVCodecContext *ctx= NULL;
    AVCodecParameters *origin_par = NULL;
    // struct SwsContext * my_SwsContext;
    uint8_t *byte_buffer = NULL;
    AVPacket *pkt;
    AVFormatContext *fmt_ctx = NULL;
    int video_stream;
    int byte_buffer_size;
    int i = 0;
    int result;
    static int cnt = 0;

    result = avformat_open_input(&fmt_ctx, pstVdecThreadParam->cFileName, NULL, NULL);
    if (result < 0) {
        av_log(NULL, AV_LOG_ERROR, "Can't open file\n");
        pthread_exit(NULL);
    }

    result = avformat_find_stream_info(fmt_ctx, NULL);
    if (result < 0) {
        av_log(NULL, AV_LOG_ERROR, "Can't get stream info\n");
        pthread_exit(NULL);
    }

    video_stream = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (video_stream < 0) {
      av_log(NULL, AV_LOG_ERROR, "Can't find video stream in input file\n");
        pthread_exit(NULL);
    }

    origin_par = fmt_ctx->streams[video_stream]->codecpar;

    codec = avcodec_find_decoder(origin_par->codec_id);
    if (!codec) {
        av_log(NULL, AV_LOG_ERROR, "Can't find decoder\n");
        pthread_exit(NULL);
    }

    ctx = avcodec_alloc_context3(codec);
    if (!ctx) {
        av_log(NULL, AV_LOG_ERROR, "Can't allocate decoder context\n");
        return AVERROR(ENOMEM);
    }

    result = avcodec_parameters_to_context(ctx, origin_par);
    if (result) {
        av_log(NULL, AV_LOG_ERROR, "Can't copy decoder context\n");
        pthread_exit(NULL);
    }

    result = avcodec_open2(ctx, codec, NULL);
    if (result < 0) {
        av_log(ctx, AV_LOG_ERROR, "Can't open decoder\n");
        pthread_exit(NULL);
    }


    pkt = av_packet_alloc();
    if (!pkt) {
        av_log(NULL, AV_LOG_ERROR, "Cannot allocate packet\n");
        pthread_exit(NULL);
    }

    printf("pix_fmt:%d\n",ctx->pix_fmt);
    
    // byte_buffer_size = av_image_get_buffer_size(ctx->pix_fmt, ctx->width, ctx->height, 16);
    byte_buffer_size = av_image_get_buffer_size( AV_PIX_FMT_RGB565LE, 480, 320, 16);
    byte_buffer = (uint8_t*)fbp;
    // byte_buffer = av_malloc(byte_buffer_size);

    printf("w:%d h:%d byte_buffer_size:%d\n",ctx->width,ctx->height,byte_buffer_size);
    if (!byte_buffer) {
        av_log(NULL, AV_LOG_ERROR, "Can't allocate buffer\n");
        pthread_exit(NULL);
    }

    printf("#tb %d: %d/%d\n", video_stream, fmt_ctx->streams[video_stream]->time_base.num, fmt_ctx->streams[video_stream]->time_base.den);
    i = 0;

    result = 0;
    // int i_clock = 0;
    clock_t clock_arr[10];
    clock_t time, time_tmp;
    time = clock();
    while (result >= 0 && !bStopCtl) {
        clock_arr[0] = clock();//----------------------------------------------------------------------------------------------------------

        result = av_read_frame(fmt_ctx, pkt);
        if (result >= 0 && pkt->stream_index != video_stream) {
            av_packet_unref(pkt);
            continue;
        }
        clock_arr[1] = clock();//----------------------------------------------------------------------------------------------------------

        time_tmp = clock();
        while(time_tmp - time <= 20000){
            time_tmp = clock();
            usleep(100);
        }
        printf("time:%f cnt:%d\n",(double)(time_tmp - time)/CLOCKS_PER_SEC,cnt);
        time = time_tmp;
        if (result < 0){
            // result = _sendStream2Dec(pkt,true);
            goto finish;
        }
        else {
            if (pkt->pts == AV_NOPTS_VALUE)
                pkt->pts = pkt->dts = i;
            result = _sendStream2Dec(pkt,false);
            cnt++;
        }

        av_packet_unref(pkt);

        if (result < 0) {
            av_log(NULL, AV_LOG_ERROR, "Error submitting a packet for decoding\n");
            pthread_exit(NULL);
        }

        clock_arr[2] = clock();//----------------------------------------------------------------------------------------------------------

        clock_arr[3] = clock();//----------------------------------------------------------------------------------------------------------
        if(0)
        printf("time=%f,%f,%f\n", (double)(clock_arr[1] - clock_arr[0]) / CLOCKS_PER_SEC,
                                (double)(clock_arr[2] - clock_arr[1]) / CLOCKS_PER_SEC,
                                (double)(clock_arr[3] - clock_arr[2]) / CLOCKS_PER_SEC);
        i++;
    }

finish:
    av_packet_free(&pkt);
    avformat_close_input(&fmt_ctx);
    avcodec_free_context(&ctx);
    av_freep(&byte_buffer);
    // sws_freeContext(my_SwsContext);
    pthread_exit(NULL);
    bStopCtl = true;
}

int main(int argc, char *argv[]){

    signal(SIGINT, SampleHandleSig);
    signal(SIGTERM, SampleHandleSig);

	if(argc == 1) return 0;

    _UNUSED CVI_S32 s32Ret;
	{
		MMF_VERSION_S stVersion;
		CVI_SYS_GetVersion(&stVersion);
		printf("MMF Version:%s\n", stVersion.version);
	}
    SIZE_S srcSize = { 
        .u32Width = 1280,
        .u32Height = 720
    };
    SIZE_S dstSize = { 
        .u32Width = 384,
        .u32Height = 288
    };
    CVI_BOOL abChnEnable[VPSS_MAX_PHY_CHN_NUM + 1] = { true, true, 0, 0};

	VDEC_CHN_ATTR_S vdec_chn0_attr;
	VDEC_THREAD_PARAM_S vdecThreadParm;

	VBPool_Init(srcSize,dstSize);
	setVdecChnAttr(&vdec_chn0_attr,VDEC_CHN0,srcSize);
	setVdecThreadParm(&vdecThreadParm,argv[1],VDEC_CHN0);
	s32Ret = setVpssGrp(VPSS_GRP0,abChnEnable,srcSize,dstSize);

    CVI_U32 iVBPoolIndex = 0;
    printf("Attach VBPool(%u) to VPSS Grp(%u) Chn(%u)\n", iVBPoolIndex, VPSS_GRP0, VPSS_CHN0);
    s32Ret = CVI_VPSS_AttachVbPool(VPSS_GRP0, VPSS_CHN0, (VB_POOL)iVBPoolIndex);
    if (s32Ret != CVI_SUCCESS) {
        printf("Cannot attach VBPool(%u) to VPSS Grp(%u) Chn(%u): ret=%x\n", iVBPoolIndex, VPSS_GRP0, iVBPoolIndex, s32Ret);
        goto vpss_start_error;
    }
    iVBPoolIndex++;
    printf("Attach VBPool(%u) to VPSS Grp(%u) Chn(%u)\n", iVBPoolIndex, VPSS_GRP0, VPSS_CHN0);
    s32Ret = CVI_VPSS_AttachVbPool(VPSS_GRP0, VPSS_CHN1, (VB_POOL)iVBPoolIndex);
    if (s32Ret != CVI_SUCCESS) {
        printf("Cannot attach VBPool(%u) to VPSS Grp(%u) Chn(%u): ret=%x\n", iVBPoolIndex, VPSS_GRP0, iVBPoolIndex, s32Ret);
        goto vpss_start_error;
    }

    // VB_POOL_CONFIG_S stVbPoolCfg;
    // VB_POOL vdecVBPoolId;
	// attachVdecVBPool(&stVbPoolCfg,&vdecVBPoolId);
    // iVBPoolIndex++;
    // printf("Attach VBPool to VDEC Chn(%u)\n",VDEC_CHN0);
    // VDEC_CHN_POOL_S stVdecPool = {
    //     .hPicVbPool = (VB_POOL)iVBPoolIndex ,
    //     .hTmvVbPool = (VB_POOL)iVBPoolIndex + 1
    // };
	// s32Ret = CVI_VDEC_AttachVbPool(VDEC_CHN0, &stVdecPool);
    // if (s32Ret != CVI_SUCCESS) {
    //     printf("Cannot Attach VBPool to VDEC Chn(%u): ret=%x\n", VDEC_CHN0 ,s32Ret);
    //     goto vpss_start_error;
    // }

	if(s32Ret == CVI_FAILURE) goto vpss_start_error;

	vdecThreadParm.eThreadCtrl = THREAD_CTRL_START;
	_UNUSED pthread_t getDstImgThread, getSrcImgThread, sendStreamThread, getDecImgThread;
	// SAMPLE_COMM_VDEC_StartSendStream( &vdecThreadParm, &sendStreamThread);
    startVdecFrameSendThread(&sendStreamThread,&vdecThreadParm);
    pthread_create(&getDstImgThread, NULL, getDstImg, NULL);
    pthread_create(&getSrcImgThread, NULL, getSrcImg, NULL);
    pthread_create(&getDecImgThread, NULL, getDecImg, NULL);

	printf("vdec get pic start!\n");
	// SAMPLE_COMM_VDEC_StartGetPic( &vdecThreadParm, &getDecPicThread);

    pthread_join(getDstImgThread, NULL);
    pthread_join(getSrcImgThread, NULL);
    pthread_join(sendStreamThread, NULL);
    printf("exit all thread down\n");
    // pthread_join(getDecImgThread, NULL);
	// SAMPLE_COMM_VDEC_StopGetPic( &vdecThreadParm, &getDecPicThread);
	
	vdecThreadParm.eThreadCtrl = THREAD_CTRL_STOP;
	SAMPLE_COMM_VDEC_Stop(VDEC_CHN0);

    SAMPLE_COMM_VPSS_Stop(VPSS_GRP0, abChnEnable);
	printf("stop vpss grp %d!",VPSS_GRP0);

    CVI_SYS_Exit();
    CVI_VB_Exit();

	return 0;

vpss_start_error:
    SAMPLE_COMM_VPSS_Stop(VPSS_GRP0, abChnEnable);

    CVI_SYS_Exit();
    CVI_VB_Exit();

	return -1;
}