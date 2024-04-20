#include "middleware_utils.h"
#include <cvi_comm.h> 
#include <sample_comm.h>
#include <core/utils/vpss_helper.h>

#include <stdio.h>

static void showVpssGrpAttr(VPSS_GRP_ATTR_S stVpssGrpAttr){
  printf("[showVpssGrpAttr] %d, %d, %d, %d, %d, %d\n", 
          stVpssGrpAttr.enPixelFormat, stVpssGrpAttr.stFrameRate.s32SrcFrameRate, stVpssGrpAttr.stFrameRate.s32DstFrameRate,
          stVpssGrpAttr.u32MaxH, stVpssGrpAttr.u32MaxW, stVpssGrpAttr.u8VpssDev);
}

int main(){
    VPSS_GRP_ATTR_S stVpssGrpAttr       ;
    VPSS_CHN_ATTR_S astVpssChnAttr[VPSS_MAX_PHY_CHN_NUM]   ;
    CVI_BOOL abChnEnable[VPSS_MAX_PHY_CHN_NUM + 1] = { 0, 0, 0, 0};
    CVI_VPSS_DestroyGrp(0);
    VPSS_GRP_DEFAULT_HELPER2(&stVpssGrpAttr, 1920, 1080, PIXEL_FORMAT_NV21, 1);
    SAMPLE_COMM_VPSS_Init( 0, abChnEnable, &stVpssGrpAttr, astVpssChnAttr);
    showVpssGrpAttr(stVpssGrpAttr);
    // SAMPLE_COMM_VI_DestroyIsp(&stViConfig);
    // SAMPLE_COMM_VI_DestroyVi(&stViConfig);
    CVI_VPSS_DestroyGrp(0);
    return 0;
}