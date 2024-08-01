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
#include <sys/time.h>
#include <time.h>

    #include "libavutil/adler32.h"
    #include "libavcodec/avcodec.h"
    #include "libavcodec/bsf.h"
    #include "libavformat/avformat.h"
    #include "libavutil/imgutils.h"
    #include "libavutil/timestamp.h"
    #include "libswscale/swscale.h"

#include <termios.h>

#define VPSS_GRP0 0
#define VDEC_CHN0 0
#define VDEC_STREAM_MODE VIDEO_MODE_FRAME
#define VDEC_EN_TYPE PT_H264
#define VDEC_PIXEL_FORMAT PIXEL_FORMAT_NV21
// #define VDEC_PIXEL_FORMAT PIXEL_FORMAT_YUV_PLANAR_420
#define _UNUSED __attribute__((unused))

#define VIDEO_FORWARD_SECONDS 10
#define VIDEO_BACK_SECONDS 10

#define LCD_BPP 24

#define IGN_SIGSEGV

#define FLASH_LINE do{printf("\033[A\033[K");}while(0);

typedef enum{
    VIDEO_PLAYING = 0,
    VIDEO_PAUSE ,
    VIDEO_BACK  ,
    VIDEO_FORWARD
} video_play_status_enum;

typedef struct video_status_ctl{
    bool en;
    video_play_status_enum status;
    int pts_offset;
}video_status_ctl_typedef;

video_status_ctl_typedef video_status_ctl = {
    .en = false,
    .status = VIDEO_PLAYING,
    .pts_offset = 0
};

struct phy2virAddrkey_s{
    unsigned char* phy;
    unsigned char* vir;
    size_t size;
};
static struct phy2virAddrkey_s phy2vir_arr[5];

struct timer_s{
    int val;
    timer_t id;
    struct sigevent sev;
    struct itimerspec its;
    void (*handler)(union sigval val);
};
static struct timer_s timer_recv_obj;
static struct timer_s timer_send_obj;

struct h264_frame_pkt_s{
    uint8_t* buf;
    size_t size;
    int64_t pts;
    bool en;
#define H264_FRAME_BUF_MAX_SIZE 1024 * 256
};
struct h264_frame_pkt_s h264_frame_send_recv_obj;

pthread_mutex_t video_status_ctl_mutex = PTHREAD_MUTEX_INITIALIZER;

pthread_mutex_t video_wait_thread_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t video_wait_thread_cond = PTHREAD_COND_INITIALIZER;

pthread_mutex_t video_cmd_thread_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t video_cmd_thread_cond = PTHREAD_COND_INITIALIZER;

pthread_mutex_t video_frame_send_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t video_frame_send_cond = PTHREAD_COND_INITIALIZER;

static volatile bool bStopCtl = false;
static volatile bool bWaitCmdDownCtl = false;
static volatile bool bPauseThread = false;
static volatile bool bReleaseSendFrame = false;
static volatile bool bSendFrame = false;
static char *fbp;
_UNUSED static char fbp_tmp[480*320*3];
static volatile int pts_timebase;

_UNUSED static void lcd_show_rgb888(char *dstAddr, VIDEO_FRAME_INFO_S stFrame);
static void timer_frame_send_handler(union sigval val){
    static long long cnt = 0;
    cnt++;
    // printf("cnt:%lld ",cnt);

    pthread_mutex_lock(&video_frame_send_mutex);
    if(!h264_frame_send_recv_obj.en){
        pthread_mutex_unlock(&video_frame_send_mutex);
        return;
    } 

    static VDEC_STREAM_S stStream;
    
    stStream.u32Len     = h264_frame_send_recv_obj.size ;
    stStream.pu8Addr    = h264_frame_send_recv_obj.buf ;
    stStream.u64PTS     = h264_frame_send_recv_obj.pts  ;

    stStream.bDisplay       = true          ;
    stStream.bEndOfFrame    = true          ;
    stStream.bEndOfStream   = false         ;

    CVI_VDEC_SendStream(VDEC_CHN0,&stStream,100);

    h264_frame_send_recv_obj.en = false;
    bSendFrame = false;

    pthread_cond_broadcast(&video_frame_send_cond);

    pthread_mutex_unlock(&video_frame_send_mutex);
}
static void timer_frame_recv_handler(union sigval val){
    static long long cnt = 0;
    cnt++;
    // printf("cnt:%lld ",cnt);

    static VIDEO_FRAME_INFO_S stFrame;
    static CVI_S32 s32Ret;
    if(bStopCtl) return;
    if(bPauseThread) return;

    s32Ret = CVI_VPSS_GetChnFrame(VPSS_GRP0, VPSS_CHN1, &stFrame, 2000);
    if(s32Ret == 0xc006800e) return ;
    if(s32Ret != CVI_SUCCESS){
        bStopCtl = true;
        CVI_VPSS_ReleaseChnFrame( VPSS_GRP0, VPSS_CHN1, &stFrame);
        return ;
    }

    FLASH_LINE;
    lcd_show_rgb888(fbp,stFrame);

    printf("cnt:%lld get pts:%ld, pts2time:%.2f, phyaddr:0x%lx\n", 
        cnt, stFrame.stVFrame.u64PTS, 
        (float)stFrame.stVFrame.u64PTS/pts_timebase,
        stFrame.stVFrame.u64PhyAddr[0]);

    CVI_VPSS_ReleaseChnFrame( VPSS_GRP0, VPSS_CHN1, &stFrame);
}
static void h264_frame_pkt_init(struct h264_frame_pkt_s* obj){
    obj->buf = (uint8_t*)malloc(H264_FRAME_BUF_MAX_SIZE);
    obj->en = false;
}
static void thread_wait_cmd_down(pthread_mutex_t* pmutex, pthread_cond_t* pcond, bool *_cond)
{
    pthread_mutex_lock(pmutex);
    while (*_cond) {
        pthread_cond_wait(pcond, pmutex);
    }
    pthread_mutex_unlock(pmutex);
}
static void set_video_status_ctl_val(bool en, video_play_status_enum status, int pts_offset, video_status_ctl_typedef* _ctl_status)
{
    _ctl_status->en = en;
    _ctl_status->pts_offset = pts_offset;
    _ctl_status->status = status;
}
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
static _UNUSED unsigned char* get_virAddr_by_phyAddr(struct phy2virAddrkey_s *arr, unsigned char* phy, size_t size, int n){
    int i;
    for( i = 0; i < n;i++){
        if(!arr[i].phy)
            break;
        if(arr[i].phy == phy){
            // printf("%d 0x%p->0x%p ",i,arr[i].phy,arr[i].vir);
            return arr[i].vir;
        }
    }
    // printf("\n"); 
    if(i == n) return NULL;

    arr[i].phy = phy;
    arr[i].vir = (uint8_t *)CVI_SYS_Mmap(phy, size);
    arr[i].size = size;
    return arr[i].vir;
}
static _UNUSED void destory_virAddr(struct phy2virAddrkey_s *arr, int n){
    for(int i = 0; i < n;i++){
        if(arr[i].phy)
            CVI_SYS_Munmap((void *)arr[i].vir, arr[i].size);
    }
}
static _UNUSED void timer_init(struct timer_s *obj, int val)
{
    memset(&obj->sev, 0, sizeof(struct sigevent));

    obj->sev.sigev_value.sival_int = 111;
    obj->sev.sigev_notify = SIGEV_THREAD;
    obj->sev.sigev_notify_function = obj->handler; 
    
    if (timer_create(CLOCK_REALTIME, &obj->sev, &obj->id) == -1) {
        perror("timer_create");
        exit(EXIT_FAILURE);
    }
}
static _UNUSED void timer_set_time(struct timer_s *obj, long delay_nsec, long interval_nsec)
{
    long delay_sec = delay_nsec/1000000000;
    delay_nsec =delay_nsec%1000000000;

    long interval_sec = interval_nsec/1000000000;
    interval_nsec =interval_nsec%1000000000;

    // 设置定时器属性
    obj->its.it_value.tv_sec = delay_sec;  // 首次触发时间（秒）
    obj->its.it_value.tv_nsec = delay_nsec; // 首次触发时间（纳秒）
    obj->its.it_interval.tv_sec = interval_sec; // 间隔时间（秒）
    obj->its.it_interval.tv_nsec = interval_nsec; // 间隔时间（纳秒）

    if (timer_settime(obj->id, 0, &obj->its, NULL) == -1) {
        perror("timer_settime");
        exit(EXIT_FAILURE);
    }
}
static _UNUSED void timer_destory(struct timer_s *obj)
{
    if (timer_delete(obj->id) == -1) {
        perror("timer_delete");
        exit(EXIT_FAILURE);
    }
}
static void SampleHandleSig(CVI_S32 signo) {
  signal(SIGINT, SIG_IGN);
  signal(SIGTERM, SIG_IGN);
#ifdef IGN_SIGSEGV
  signal(SIGSEGV, SIG_IGN);
#endif
  printf("handle signal, signo: %d\n", signo);
  if (SIGINT == signo || SIGTERM == signo 
#ifdef IGN_SIGSEGV
  || SIGSEGV == signo
#endif
  ) {
    printf("info signo:%d\n",signo);
    bStopCtl = true;

    bPauseThread = false;
    pthread_mutex_lock(&video_wait_thread_mutex);
    pthread_cond_broadcast(&video_wait_thread_cond);
    pthread_mutex_unlock(&video_wait_thread_mutex);
}
}
static CVI_S32 setVdecChnAttr(VDEC_CHN_ATTR_S *pstChnAttr,VDEC_CHN VdecChn,SIZE_S srcSize){
	VDEC_CHN_PARAM_S stChnParam;

	pstChnAttr->enType = VDEC_EN_TYPE		;
	pstChnAttr->enMode = VDEC_STREAM_MODE	;
	pstChnAttr->u32PicHeight = srcSize.u32Height			;
	pstChnAttr->u32PicWidth = srcSize.u32Width  			;
	pstChnAttr->u32StreamBufSize = ALIGN(pstChnAttr->u32PicHeight * pstChnAttr->u32PicWidth, 0x4000);
	printf("u32StreamBufSize = 0x%X\n", pstChnAttr->u32StreamBufSize);
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

    // VIDEO_DISPLAY_MODE_E VIDEO_DISPLAY_MODE;
    // CVI_VDEC_GetDisplayMode( VdecChn, &VIDEO_DISPLAY_MODE);
    // printf("VIDEO_DISPLAY_MODE %d\n",VIDEO_DISPLAY_MODE);
	
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
    stVbConf.astCommPool[1].u32BlkCnt = 5;

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
	vdecThreadParm->s32MinBufSize 	= 1024*50     		;
	vdecThreadParm->s32StreamMode 	= VDEC_STREAM_MODE	;
	vdecThreadParm->eThreadCtrl 	= THREAD_CTRL_PAUSE	;
	vdecThreadParm->s32MilliSec 	= -1				;//阻塞模式
	vdecThreadParm->u64PtsInit		= 0					;
    vdecThreadParm->s32IntervalTime = 33*1000           ;
	return CVI_SUCCESS;
}
_UNUSED static SIZE_S getVideoWH(char *filePath){
    SIZE_S size = {
        .u32Height = 0,
        .u32Width = 0
    };
    
    AVCodecParameters *origin_par = NULL;
    AVFormatContext *fmt_ctx = NULL;
    int result, video_stream;

    result = avformat_open_input(&fmt_ctx, filePath, NULL, NULL);
    if (result < 0) {
        av_log(NULL, AV_LOG_ERROR, "Can't open file\n");
        goto get_video_info_err;
    }

    result = avformat_find_stream_info(fmt_ctx, NULL);
    if (result < 0) {
        av_log(NULL, AV_LOG_ERROR, "Can't get stream info\n");
        goto get_video_info_err;
    }

    video_stream = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (video_stream < 0) {
        av_log(NULL, AV_LOG_ERROR, "Can't find video stream in input file\n");
        goto get_video_info_err;
    }

    origin_par = fmt_ctx->streams[video_stream]->codecpar;

    size.u32Height = origin_par->height;
    size.u32Width = origin_par->width;

    avformat_close_input(&fmt_ctx);

    return size;

get_video_info_err:
    return size;

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
_UNUSED static CVI_VOID startGetdecThread(pthread_t *pVdecThread, void *fun())
{
	struct sched_param param;
	pthread_attr_t attr;

	param.sched_priority = 40;
	pthread_attr_init(&attr);
	pthread_attr_setschedpolicy(&attr, SCHED_RR);
	pthread_attr_setschedparam(&attr, &param);
	pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
	pthread_create(pVdecThread, &attr, fun, NULL);
}
_UNUSED static void lcd_show_rgb888(char *dstAddr, VIDEO_FRAME_INFO_S stFrame){
    size_t image_size = stFrame.stVFrame.u32Length[0] + stFrame.stVFrame.u32Length[1] +
                        stFrame.stVFrame.u32Length[2];
      
    unsigned char *u8_rgb888_data = get_virAddr_by_phyAddr(phy2vir_arr,stFrame.stVFrame.u64PhyAddr[0],image_size,5);
    int width = (stFrame.stVFrame.u32Width > 480)?480:stFrame.stVFrame.u32Width;    
    int height = (stFrame.stVFrame.u32Height > 320)?320:stFrame.stVFrame.u32Height;    
    int data_size = width*3;
    int lcd_pitch = 480*3;

    for (int i = 0; i < height; i++) {
        memcpy( dstAddr, u8_rgb888_data, data_size);
        dstAddr += lcd_pitch;
        u8_rgb888_data += data_size;
    }


}
_UNUSED static void *getDstImg() {
    printf("Enter get dst img thread\n");
    VIDEO_FRAME_INFO_S stFrame;
    CVI_S32 s32Ret;

    lcd_init();
    _UNUSED static clock_t time_begin, time_end;
    memset(phy2vir_arr,0,sizeof(struct phy2virAddrkey_s)*3);
    while (bStopCtl == false) {
        thread_wait_cmd_down(&video_wait_thread_mutex,&video_wait_thread_cond,&bPauseThread);

        s32Ret = CVI_VPSS_GetChnFrame(VPSS_GRP0, VPSS_CHN1, &stFrame, 500);
        if(!time_begin) time_begin = (clock_t)stFrame.stVFrame.u64PTS*CLOCKS_PER_SEC/pts_timebase;
		time_end = clock();
        if (s32Ret == 0xc006800e) {
            // printf("CVI_VPSS_GetChnFrame chn1 failed with %#x\n", s32Ret);
            // printf("wait 100ms\n");
			usleep(100);
            // break;
			continue;
		}else if (s32Ret != CVI_SUCCESS) {
            printf("CVI_VPSS_GetChnFrame chn1 failed with %#x\n", s32Ret);
            break;
        }

        printf("get pts = %ld, pts2time:%.2f, time:%.2f, phyaddr:0x%lx\n", 
            stFrame.stVFrame.u64PTS, 
            (float)stFrame.stVFrame.u64PTS/pts_timebase,
            (double)(time_end - time_begin) / CLOCKS_PER_SEC,
            stFrame.stVFrame.u64PhyAddr[0]);
		// 将当前光标往上移动一行
		// printf("\033[A");
		//删除光标后面的内容
		// printf("\033[K");
    
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

            lcd_show_rgb888(fbp,stFrame);

    error:
        // CVI_VPSS_ReleaseChnFrame( VPSS_GRP0, VPSS_CHN0, &stFrame);
        CVI_VPSS_ReleaseChnFrame( VPSS_GRP0, VPSS_CHN1, &stFrame);
        // CVI_TDL_FreeImage(&frame_img);
        if (s32Ret != CVI_SUCCESS) {
            bStopCtl = true;
        }
    }
    destory_virAddr(phy2vir_arr,5);
    printf("Exit get dst img thread\n");
    pthread_exit(NULL);
}
_UNUSED static void *getSrcImg() {
    printf("Enter get src img thread\n");
    VIDEO_FRAME_INFO_S stFrame;
    CVI_S32 s32Ret;
    while (bStopCtl == false) {
        thread_wait_cmd_down(&video_wait_thread_mutex,&video_wait_thread_cond,&bPauseThread);

        s32Ret = CVI_VPSS_GetChnFrame(VPSS_GRP0, VPSS_CHN0, &stFrame, 2000);
		if (s32Ret == 0xc006800e) {
            // printf("CVI_VPSS_GetChnFrame chn0 failed with %#x\n", s32Ret);
            // printf("wait 100ms\n");
			usleep(100);
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
_UNUSED static void *getDecImg() {
	// lcd_init();
    printf("Enter get dec img thread\n");
    VIDEO_FRAME_INFO_S stFrame;
    CVI_S32 s32Ret;
    static int cnt = 0;
    _UNUSED clock_t time,time_tmp;
    time = clock();

    int send_en = 0;
    while (bStopCtl == false) {
        thread_wait_cmd_down(&video_wait_thread_mutex,&video_wait_thread_cond,&bPauseThread);

		s32Ret = CVI_VDEC_GetFrame( VDEC_CHN0, &stFrame, -1);
		if (s32Ret == 0xc0058041) {
            // printf("CVI_DEC_GetChnFrame chn1 failed with %#x\n", s32Ret);
            // printf("wait next frame \n");
			usleep(10);
			continue;
		}else if (s32Ret != CVI_SUCCESS) {
            printf("CVI_DEC_GetChnFrame chn0 failed with %#x\n", s32Ret);
            break;
        }
        // printf("pixel format:%d ",stFrame.stVFrame.enPixelFormat);
        if(send_en == 0 && stFrame.stVFrame.enPixelFormat == VDEC_PIXEL_FORMAT){
            // time_tmp = clock();
            // while(time_tmp - time <= 333){
            //     time_tmp = clock();
            //     usleep(100);
            // }
            // time = time_tmp;
    // sendFrame:
            s32Ret = CVI_VPSS_SendFrame( VPSS_GRP0, &stFrame, -1);//手动发送解码帧
            // s32Ret = CVI_SUCCESS;
            if (s32Ret == 0xc0068012) {//busy
                // CVI_VDEC_ReleaseFrame( VDEC_CHN0, &stFrame);
                // usleep(100);
                // goto sendFrame;
            }else if (s32Ret != CVI_SUCCESS) {
                printf("CVI_VPSS_SendFrame to VPSS0 failed with %#x\n", s32Ret);
                CVI_VDEC_ReleaseFrame( VDEC_CHN0, &stFrame);
                break;
            }
        }
        // send_en = (send_en+1)%2;

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
    if(bStopCtl == false) bStopCtl=true;
	pthread_exit(NULL);
}
_UNUSED static void *sendFrame(CVI_VOID *pArgs){

    // CVI_S32 s32Ret;
//     int _sendStream2Dec(uint32_t size, unsigned char * data, uint64_t pts){
//         static CVI_S32 s32Ret;
//         static VDEC_STREAM_S stStream;
//         stStream.u32Len     = size ;
//         // if(stStream.u32Len > 1024*10) 
//         // printf("pkt size:%d\n",size);
//         stStream.pu8Addr    = data ;
//         stStream.u64PTS     = pts  ;

//         stStream.bDisplay       = true          ;
//         stStream.bEndOfFrame    = true          ;
//         stStream.bEndOfStream   = false         ;
// SendAgain:
//         s32Ret = CVI_VDEC_SendStream(VDEC_CHN0,&stStream,-1);
//         if(s32Ret != 0){
//             usleep(100);
//             goto SendAgain;
//         }
//         return 1;
//     }

    void _sendStream2timerHandler(uint32_t size, unsigned char * data, uint64_t pts){
        pthread_mutex_lock(&video_frame_send_mutex);

        memcpy(h264_frame_send_recv_obj.buf,data,size);
        h264_frame_send_recv_obj.size = size;
        h264_frame_send_recv_obj.pts = pts;

        h264_frame_send_recv_obj.en = true;
        bSendFrame = true;

        pthread_mutex_unlock(&video_frame_send_mutex);
    }

	VDEC_THREAD_PARAM_S *pstVdecThreadParam = (VDEC_THREAD_PARAM_S *)pArgs;

    const AVCodec *codec = NULL;
    AVCodecContext *ctx= NULL;
    AVCodecParameters *origin_par = NULL;
    // struct SwsContext * my_SwsContext;
    // uint8_t *byte_buffer = NULL;
    AVPacket *pkt;
    AVFormatContext *fmt_ctx = NULL;
    int video_stream;
    int byte_buffer_size;
    int i = 0;
    int result;
    static int cnt = 0;

    int video_type = 0;
    if(1){
        int file_name_n = strlen(pstVdecThreadParam->cFileName);
        if(strncmp(&pstVdecThreadParam->cFileName[file_name_n - 3],"mp4",3) == 0)
            video_type = 1;
        //mp3
    }

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
    // byte_buffer = (uint8_t*)fbp;
    // byte_buffer = av_malloc(byte_buffer_size);
    AVRational frame_rate = fmt_ctx->streams[video_stream]->avg_frame_rate;

    printf("w:%d h:%d byte_buffer_size:%d\n",ctx->width,ctx->height,byte_buffer_size);
    float fq = (float)frame_rate.den / frame_rate.num;
    long interval_nsec = (long)(1000000000*fq);

    timer_set_time(&timer_recv_obj,1000000,interval_nsec);
    timer_set_time(&timer_send_obj,1000,interval_nsec);

    printf("timer_set %ldus\n",(long)(1000000*fq));
    printf("Frame rate: %d/%d %.3f\n", frame_rate.num, frame_rate.den, fq);
    // if (!byte_buffer) {
    //     av_log(NULL, AV_LOG_ERROR, "Can't allocate buffer\n");
    //     pthread_exit(NULL);
    // }

    printf("#tb %d: %d/%d\n", video_stream, fmt_ctx->streams[video_stream]->time_base.num, fmt_ctx->streams[video_stream]->time_base.den);
    pts_timebase = fmt_ctx->streams[video_stream]->time_base.den;
    i = 0;


    AVBSFContext * h264bsfc;
    uint8_t *h264_fr_buf = NULL;  // 连续的内存空间
    size_t h264_fr_buf_size = H264_FRAME_BUF_MAX_SIZE ;  // 初始分配1MB，实际大小可能需要调整
    size_t pos = 0;  // 跟踪当前写入位置
    const AVBitStreamFilter * filter;
    if(video_type){
        filter = av_bsf_get_by_name("h264_mp4toannexb");
        av_bsf_alloc(filter, &h264bsfc);
        avcodec_parameters_copy(h264bsfc->par_in, fmt_ctx->streams[video_stream]->codecpar);
        
        av_bsf_init(h264bsfc);
        h264_fr_buf = (uint8_t *)av_malloc(h264_fr_buf_size);
    }

    // AVBitStreamFilterContext* h264bsfc =  av_bitstream_filter_init("h264_mp4toannexb"); 

    result = 0;
    // int i_clock = 0;

    video_status_ctl_typedef _video_status_ctl;
    video_play_status_enum video_status = VIDEO_PLAYING;
    bool unBlockingFlag = false;
    int64_t pts;

    static VDEC_CHN_STATUS_S vdecChnStatus;
    while (result >= 0 && !bStopCtl) {

        unBlockingFlag = false;
        _video_status_ctl.en = false;

        pthread_mutex_lock(&video_status_ctl_mutex);

        if(bWaitCmdDownCtl){
            memcpy( &_video_status_ctl, &video_status_ctl, sizeof(video_status_ctl_typedef));
        }

        if(_video_status_ctl.en && bWaitCmdDownCtl)
        {
            int ret;
            bPauseThread = true;
            switch (_video_status_ctl.status)
            {
            case VIDEO_PLAYING:
                if(video_status != VIDEO_PLAYING){
                    unBlockingFlag = true;
                }
                video_status = VIDEO_PLAYING;
                break;
            case VIDEO_PAUSE:
                video_status = VIDEO_PAUSE;
                break;
            case VIDEO_BACK:
                unBlockingFlag = true;
                ret = av_seek_frame(fmt_ctx, video_stream , (int64_t)(pts - _video_status_ctl.pts_offset * pts_timebase), AVSEEK_FLAG_BACKWARD );
                if(ret < 0){
                    printf("av_seek_frame failed!!\ntarget:%.2f\n",(float)pts/pts_timebase - _video_status_ctl.pts_offset);
                }
                break;
            case VIDEO_FORWARD:
                unBlockingFlag = true;
                ret = av_seek_frame(fmt_ctx, video_stream , (int64_t)(pts + _video_status_ctl.pts_offset * pts_timebase), AVSEEK_FLAG_BACKWARD );
                if(ret < 0){
                    printf("av_seek_frame failed!!\ntarget:%.2f\n",(float)pts/pts_timebase + _video_status_ctl.pts_offset);
                }
                break;
            default:
                break;
            }

            if(unBlockingFlag){
                bPauseThread = false;
                pthread_mutex_lock(&video_wait_thread_mutex);
                pthread_cond_broadcast(&video_wait_thread_cond);
                pthread_mutex_unlock(&video_wait_thread_mutex);
            }

            if(bWaitCmdDownCtl){
                bWaitCmdDownCtl = false;
                // pthread_mutex_lock(&video_cmd_thread_mutex);
                pthread_cond_broadcast(&video_cmd_thread_cond);
                // pthread_mutex_unlock(&video_cmd_thread_mutex);
            }

        }

        pthread_mutex_unlock(&video_status_ctl_mutex);

        if(video_status == VIDEO_PAUSE) continue;

        result = av_read_frame(fmt_ctx, pkt);
        if (result >= 0 && pkt->stream_index != video_stream) {
            av_packet_unref(pkt);
            continue;
        }
        if (result < 0){
            printf("av_read_frame done!\n");
            goto finish;
        }
        else {
            if (pkt->pts == AV_NOPTS_VALUE)
                pkt->pts = pkt->dts = i;
            cnt++;
        }

        if(video_type){
            pts = pkt->pts;
            // int pts = i;
            pos = 0;
            result = av_bsf_send_packet(h264bsfc, pkt);
            if(result < 0){
                printf("av_bsf_send_packet error\n");
                goto finish;
            } 
            while (av_bsf_receive_packet(h264bsfc, pkt) == 0)
            {
                size_t packet_size = pkt->size;
                if (pos + packet_size > h264_fr_buf_size){
                    printf("packet size %ld\n",pos + packet_size);
                    printf("packet size out of h264_fr_buf_size!!!\n");
                    av_packet_unref(pkt);
                    goto finish;
                }
                memcpy(h264_fr_buf + pos, pkt->data, packet_size);
                pos += packet_size;  // 更新写入位置
                av_packet_unref(pkt);
            }
            // printf("<pts:%d>",pts);
            _sendStream2timerHandler( pos, h264_fr_buf, pts);
        }
        else {
            _sendStream2timerHandler( pkt->size,pkt->data,pkt->pts);
            av_packet_unref(pkt);
        }

        thread_wait_cmd_down(&video_frame_send_mutex,&video_frame_send_cond,&bSendFrame);

        // usleep(165);

        int ret = CVI_VDEC_QueryStatus(VDEC_CHN0,&vdecChnStatus);
        if(ret == 0 && 0){
            printf("enType:%d, Bytes:%d, Frames:%d, Pic:%d, b:%d, RF:%d, DSF:%d, pts:%.2f\n",
                vdecChnStatus.enType, vdecChnStatus.u32LeftStreamBytes, vdecChnStatus.u32LeftStreamFrames,
                vdecChnStatus.u32LeftPics, vdecChnStatus.bStartRecvStream, vdecChnStatus.u32RecvStreamFrames, 
                vdecChnStatus.u32DecodeStreamFrames, (float)pts/16000);
        }

        if (result < 0) {
            av_log(NULL, AV_LOG_ERROR, "Error submitting a packet for decoding\n");
            goto finish;
        }

        i++;
    }

finish:
    bStopCtl = true;
    while(!bReleaseSendFrame){
        usleep(100000);
    };
	printf("Exit send stream img thread\n");
    av_packet_free(&pkt);
    avformat_close_input(&fmt_ctx);
    avcodec_free_context(&ctx);
    // av_freep(&byte_buffer);
    // sws_freeContext(my_SwsContext);
    pthread_exit(NULL);
}
_UNUSED static void *input_ctrl()
{
    struct termios oldt, newt;
    char cmd[4];
    int bytes_read;

    bool set_video_status_flag = false;
    video_status_ctl_typedef _video_status_ctl;

    tcgetattr(STDIN_FILENO, &oldt);
    newt = oldt;
    newt.c_lflag &= ~(ICANON | ECHO);
    // newt.c_lflag &= ~(ICANON);
    newt.c_cc[VTIME] = 0;
    newt.c_cc[VMIN] = 1;
    tcsetattr(STDIN_FILENO, TCSANOW, &newt);

    printf("enter input ctl thread\n");

    while (bStopCtl == false) {
        thread_wait_cmd_down(&video_cmd_thread_mutex,&video_cmd_thread_cond,&bWaitCmdDownCtl);
        set_video_status_flag = false;

        bytes_read = read(STDIN_FILENO, cmd, 4);
        if (bytes_read == -1) {
            perror("read");
            break;
        }

        printf("recv:");
        for(int i = 0; i< bytes_read; i++)
            printf("%x",cmd[i]);
        printf("\n");

        _video_status_ctl.en = false;

        if(bytes_read == 1)
            switch (cmd[0]) {
                case 'p':
                    set_video_status_flag = true;
                    if(_video_status_ctl.status == VIDEO_PAUSE)
                        set_video_status_ctl_val(true,VIDEO_PLAYING,0,&_video_status_ctl);
                    else
                        set_video_status_ctl_val(true,VIDEO_PAUSE,0,&_video_status_ctl);
                    printf("pause cmd:%d\n",_video_status_ctl.status);
                    break;
                case 'q':
                    // 退出循环
                    bStopCtl = true;
                    goto end;
                default:
                    // 其他按键
                    break;
            }
        else if(bytes_read == 3 && strncmp(cmd,"\x1b[",2) == 0)
            switch (cmd[2]) {
                case 'A':
                    printf("Up arrow pressed\n");
                    break;
                case 'B':
                    printf("Down arrow pressed\n");
                    break;
                case 'C':
                    printf("VIDEO FORWARD %ds!!\n",VIDEO_FORWARD_SECONDS);
                    set_video_status_flag = true;
                    set_video_status_ctl_val(true,VIDEO_FORWARD,VIDEO_FORWARD_SECONDS,&_video_status_ctl);
                    break;
                case 'D':
                    printf("VIDEO BACK %ds!!\n",VIDEO_BACK_SECONDS);
                    set_video_status_flag = true;
                    set_video_status_ctl_val(true,VIDEO_BACK,VIDEO_BACK_SECONDS,&_video_status_ctl);
                    break;
                default:
                    // 其他按键
                    break;
            }
        
        if(set_video_status_flag){
            set_video_status_flag = false;
            pthread_mutex_lock(&video_status_ctl_mutex);

            memcpy(&video_status_ctl,&_video_status_ctl,sizeof(video_status_ctl_typedef));
            bWaitCmdDownCtl = true;

            pthread_mutex_unlock(&video_status_ctl_mutex);
        }

    }

end:
    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
    return NULL;
}

int main(int argc, char *argv[]){

    signal(SIGINT, SampleHandleSig);
    signal(SIGTERM, SampleHandleSig);
#ifdef IGN_SIGSEGV
    signal(SIGSEGV, SampleHandleSig);
#endif

    h264_frame_pkt_init(&h264_frame_send_recv_obj);

    timer_recv_obj.handler = timer_frame_recv_handler;
    timer_send_obj.handler = timer_frame_send_handler;
    timer_init(&timer_recv_obj,101);
    timer_init(&timer_send_obj,102);

	if(argc == 1) return 0;

    _UNUSED CVI_S32 s32Ret;
	{
		MMF_VERSION_S stVersion;
		CVI_SYS_GetVersion(&stVersion);
		printf("MMF Version:%s\n", stVersion.version);
	}
    SIZE_S srcSize = getVideoWH(argv[1]);
    if(srcSize.u32Width == 0 || srcSize.u32Height == 0) return 0;
    // SIZE_S dstSize = { 
    //     .u32Width = 384,
    //     .u32Height = 288
    //     // .u32Height = 216
    // };
    SIZE_S dstSize = { 
        .u32Width = 448,
        .u32Height = 252
    };
    CVI_BOOL abChnEnable[VPSS_MAX_PHY_CHN_NUM + 1] = { 0, true, 0, 0};

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

    lcd_init();
	if(s32Ret == CVI_FAILURE) goto vpss_start_error;

	vdecThreadParm.eThreadCtrl = THREAD_CTRL_START;
	_UNUSED pthread_t getDstImgThread, getSrcImgThread, sendStreamThread, getDecImgThread;
    _UNUSED pthread_t inputCtrlThread;
	// SAMPLE_COMM_VDEC_StartSendStream( &vdecThreadParm, &sendStreamThread);
    startVdecFrameSendThread(&sendStreamThread,&vdecThreadParm);
    // pthread_create(&getDstImgThread, NULL, getDstImg, NULL);
    // startGetdecThread(&getDstImgThread,getDstImg);
    // pthread_create(&getSrcImgThread, NULL, getSrcImg, NULL);
    pthread_create(&getDecImgThread, NULL, getDecImg, NULL);
    pthread_create(&inputCtrlThread, NULL, input_ctrl, NULL);

	printf("vdec get pic start!\n");
	// SAMPLE_COMM_VDEC_StartGetPic( &vdecThreadParm, &getDecPicThread);

    // pthread_join(getDstImgThread, NULL);
    system("date");
    pthread_join(getDecImgThread, NULL);
    bReleaseSendFrame = true;
    pthread_join(sendStreamThread, NULL);
    printf("exit all thread down\n");
    system("date");
    // pthread_join(getDecImgThread, NULL);
	// SAMPLE_COMM_VDEC_StopGetPic( &vdecThreadParm, &getDecPicThread);

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