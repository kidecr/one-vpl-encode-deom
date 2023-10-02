#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vpl/mfx.h>
#include <opencv2/opencv.hpp>

// 错误检查
#define VERIFY(x, y)       \
    if (!(x)) {            \
        printf("%s\n", y); \
        isFailed = true;   \
        throw std::exception();          \
    }

// 计算VPL版本
#define VPLVERSION(major, minor) (major << 16 | minor)
// 取整到16和32
#define ALIGN16(value)           (((value + 15) >> 4) << 4)
#define ALIGN32(X)               (((mfxU32)((X) + 31)) & (~(mfxU32)31))

#define BITSTREAM_BUFFER_SIZE 2000000
#define OUTPUT_FILE                "out.h265"

void ShowImplementationInfo(mfxLoader loader, mfxU32 implnum);
mfxStatus ReadRawFrame_InternalMem(mfxFrameSurface1 * surface, std::vector<cv::Mat> &vecBGR);
mfxStatus ReadRawFrame(mfxFrameSurface1* surface, cv::Mat &RGB4);
void WriteEncodedStream(mfxBitstream & bs, FILE * f);
void *InitAcceleratorHandle(mfxSession session, int *fd);
void FreeAcceleratorHandle(void *accelHandle, int fd);
mfxStatus AllocateExternalSystemMemorySurfacePool(mfxU8 **buf,
                                                  mfxFrameSurface1 *surfpool,
                                                  mfxFrameInfo frame_info,
                                                  mfxU16 surfnum);
int GetFreeSurfaceIndex(mfxFrameSurface1 *SurfacesPool, mfxU16 nPoolSize); 
void FreeExternalSystemMemorySurfacePool(mfxU8 *dec_buf, mfxFrameSurface1 *surfpool);   
                                        

int main(int argc, char* argv[])
{
    cv::VideoCapture cap(0);
    cv::Mat initImage;
    cap >> initImage;
	// param
	bool useHardware = false;
	// 错误代码
	int sts = 0; // MFX_ERR_NONE=0, 其他报错为负数
	bool isFailed = false;
	// 1.先load
	mfxLoader loader = NULL;
	loader = MFXLoad();
	VERIFY(loader != NULL, "MFXLoad failed -- is implementation in path?");

	// 2.设置vpl配置参数（每个config需要单独配置，用同一个mfxConfig配置多次参数会相互覆盖；config设置在后台实现，因此config不用传给后边）
	// 2.1.设置编码方式：sw hw
	mfxConfig implConfig = MFXCreateConfig(loader); // 创建配置文件
	VERIFY(NULL != implConfig, "MFXCreateConfig failed");
	mfxVariant implValue = {0};							// 创建参数
	implValue.Type = MFX_VARIANT_TYPE_U32;			// 设置参数数据类型
	implValue.Data.U32 = useHardware ? MFX_IMPL_TYPE_HARDWARE : MFX_IMPL_TYPE_SOFTWARE;	//设置参数值
	sts = MFXSetConfigFilterProperty(implConfig, (mfxU8*)"mfxImplDescription.Impl", implValue);	// 设置参数，中间字符串怎么填看mfxImplDescription或 https://spec.oneapi.io/versions/latest/elements/oneVPL/source/programming_guide/VPL_prg_session.html#onevpl-dispatcher-configuration-properties
	VERIFY(MFX_ERR_NONE == sts, "MFXSetConfigFilterProperty failed for Impl");

	// 2.2.设置编码器
	mfxConfig codecConfig = MFXCreateConfig(loader);
	VERIFY(NULL != implConfig, "MFXCreateConfig failed");
	mfxVariant codecValue = {0};
	codecValue.Type = MFX_VARIANT_TYPE_U32;
	codecValue.Data.U32 = MFX_CODEC_AVC;		// 设置CODEC类型：MFX_CODEC_*，具体可以看CodecFormatFourCC
	sts = MFXSetConfigFilterProperty(codecConfig, (mfxU8*)"mfxImplDescription.mfxEncoderDescription.encoder.CodecID", codecValue);	// 设置参数
	VERIFY(MFX_ERR_NONE == sts, "MFXSetConfigFilterProperty failed for encoder CodecID");
	// 2.3.设置vpp Implementation must provide VPP scaling
    mfxConfig vppCodecConfig = MFXCreateConfig(loader);
    VERIFY(NULL != vppCodecConfig, "MFXCreateConfig failed")
    mfxVariant vppCodecValue = {0};
    vppCodecValue.Type     = MFX_VARIANT_TYPE_U32;
    vppCodecValue.Data.U32 = MFX_EXTBUFF_VPP_SCALING;
    sts = MFXSetConfigFilterProperty(vppCodecConfig, (mfxU8 *)"mfxImplDescription.mfxVPPDescription.filter.FilterFourCC", vppCodecValue);
    VERIFY(MFX_ERR_NONE == sts, "MFXSetConfigFilterProperty failed");
    // 2.3.设置API版本(可以不用)，关于vpl是如何搜索API版本的，https://spec.oneapi.io/versions/latest/elements/oneVPL/source/programming_guide/VPL_prg_session.html
	//mfxConfig apiVersionConfig = MFXCreateConfig(loader);
	//VERIFY(NULL != apiVersionConfig, "MFXCreateConfig failed")
	//mfxVariant apiVersionValue;
	//apiVersionValue.Type = MFX_VARIANT_TYPE_U32;
	//apiVersionValue.Data.U32 = VPLVERSION(2, 7);	// API版本设为2.7
	//sts = MFXSetConfigFilterProperty(apiVersionConfig, (mfxU8*)"mfxImplDescription.ApiVersion.Version", apiVersionValue);
	//VERIFY(MFX_ERR_NONE == sts, "MFXSetConfigFilterProperty failed for API version");
    // 2.4.打印一下最终设置结果
    ShowImplementationInfo(loader, 0);

	// 3.创建session
    // 一个loader可以创建多个session，一个session可以具有多条处理流，一个程序可以创建多个loader
    // 多loader和多处理流 https://spec.oneapi.io/versions/latest/elements/oneVPL/source/programming_guide/VPL_prg_session.html#examples-of-dispatcher-s-usage
    // 多session https://spec.oneapi.io/versions/latest/elements/oneVPL/source/programming_guide/VPL_prg_session.html#multiple-sessions
	mfxSession session = NULL;
	sts = MFXCreateSession(loader, 0, &session);
	VERIFY(MFX_ERR_NONE == sts, "Cannot create session -- no implementations meet selection criteria");
    // 创建一下加速器 Convenience function to initialize available accelerator(s)
    int accel_fd         = 0;
    void *accelHandle = InitAcceleratorHandle(session, &accel_fd);

    // 4.初始化编码器
    // 4.1.设置参数 
    mfxVideoParam encodeParams = {0}; // 参数解释 https://spec.oneapi.io/versions/latest/elements/oneVPL/source/API_ref/VPL_structs_cross_component.html?highlight=mfxvideoparam#mfxvideoparam
    encodeParams.mfx.CodecId = MFX_CODEC_HEVC;  // 编码器
    // encodeParams.mfx.CodecProfile = MFX_PROFILE_AVC_CONSTRAINED_BASELINE;   // 使用默认配置参数
    // encodeParams.mfx.CodecLevel = MFX_LEVEL_AVC_5;  // 使用的编码器级别
    encodeParams.mfx.TargetUsage = MFX_TARGETUSAGE_BALANCED; // 速度和质量的平衡度
    encodeParams.mfx.RateControlMethod = MFX_RATECONTROL_VBR; // 可变比特率控制算法
    encodeParams.mfx.TargetKbps = 4000; // kbps
    encodeParams.mfx.FrameInfo.FrameRateExtN = 30; // 帧率设置 帧率 = FrameRateExtN / FrameRateExtD
    encodeParams.mfx.FrameInfo.FrameRateExtD = 1;
    encodeParams.mfx.FrameInfo.FourCC = MFX_FOURCC_I420; 
    encodeParams.mfx.FrameInfo.ChromaFormat = MFX_CHROMAFORMAT_YUV420; // 颜色采样方法
    encodeParams.mfx.FrameInfo.CropW = initImage.cols;  // 原图宽（是ROI，可以比原图小，指定方法{X，Y，W，H})
    encodeParams.mfx.FrameInfo.CropH = initImage.rows; // 原图高
    encodeParams.mfx.FrameInfo.Width = ALIGN16(initImage.cols); // 目标宽 必须为16的倍数
    encodeParams.mfx.FrameInfo.Height = ALIGN16(initImage.rows);   // 目标高 对逐行帧，必须为16的倍数，否则为32的倍数
    encodeParams.IOPattern = MFX_IOPATTERN_IN_SYSTEM_MEMORY; // 函数的输入和输出存储器访问类型
    // fill in missing params
    sts = MFXVideoENCODE_Query(session, &encodeParams, &encodeParams);
    VERIFY(MFX_ERR_NONE == sts, "Encode query failed");
    // 创建vpp参数
    mfxVideoParam vppParam = {0};
    vppParam.IOPattern = MFX_IOPATTERN_IN_SYSTEM_MEMORY | MFX_IOPATTERN_OUT_SYSTEM_MEMORY;
    vppParam.vpp.In.FourCC = MFX_FOURCC_RGB4;
    vppParam.vpp.In.ChromaFormat  = MFX_CHROMAFORMAT_YUV420;
    vppParam.vpp.In.CropX         = 0;
    vppParam.vpp.In.CropY         = 0;
    vppParam.vpp.In.CropW         = initImage.cols;
    vppParam.vpp.In.CropH         = initImage.rows;
    vppParam.vpp.In.PicStruct     = MFX_PICSTRUCT_PROGRESSIVE;
    vppParam.vpp.In.FrameRateExtN = 30;
    vppParam.vpp.In.FrameRateExtD = 1;
    vppParam.vpp.In.Width = vppParam.vpp.Out.Width = ALIGN16(initImage.cols);
    vppParam.vpp.In.Height = vppParam.vpp.Out.Height = ALIGN16(initImage.rows);

    vppParam.vpp.Out.FourCC = MFX_FOURCC_I420;
    vppParam.vpp.Out.ChromaFormat  = MFX_CHROMAFORMAT_YUV420;
    vppParam.vpp.Out.CropX         = 0;
    vppParam.vpp.Out.CropY         = 0;
    vppParam.vpp.Out.CropW         = initImage.cols;
    vppParam.vpp.Out.CropH         = initImage.rows;
    vppParam.vpp.Out.PicStruct     = MFX_PICSTRUCT_PROGRESSIVE;
    vppParam.vpp.Out.FrameRateExtN = 30;
    vppParam.vpp.Out.FrameRateExtD = 1;
    vppParam.vpp.Out.Width = vppParam.vpp.Out.Width = ALIGN16(initImage.cols);
    vppParam.vpp.Out.Height = vppParam.vpp.Out.Height = ALIGN16(initImage.rows);

    // 4.2.创建编码器
    sts = MFXVideoENCODE_Init(session, &encodeParams);
    std::cout << sts << std::endl;
    VERIFY(MFX_ERR_NONE == sts, "Encode init failed");
    // 创建vpp
    sts = MFXVideoVPP_Init(session, &vppParam);
    std::cout << sts << std::endl;
    VERIFY(MFX_ERR_NONE == sts, "VPP init failed");
    // 4.3 申请内存
    // 4.3.1 申请VPP内存
    // 创建IO队列 Query number of required surfaces for VPP
    mfxFrameAllocRequest VPPRequest[2]  = {};
    sts = MFXVideoVPP_QueryIOSurf(session, &vppParam, VPPRequest);
    VERIFY(MFX_ERR_NONE == sts, "Error in QueryIOSurf");
    // 获取IN和OUT的推荐数量
    mfxU16 nSurfNumVPPIn  = VPPRequest[0].NumFrameSuggested; // vpp in
    mfxU16 nSurfNumVPPOut = VPPRequest[1].NumFrameSuggested; // vpp out
    // 申请In内存大小
    mfxU8 *vppInBuf         = NULL;
    mfxFrameSurface1 *vppInSurfacePool = (mfxFrameSurface1 *)calloc(sizeof(mfxFrameSurface1), nSurfNumVPPIn);

    sts = AllocateExternalSystemMemorySurfacePool(&vppInBuf,
                                                  vppInSurfacePool,
                                                  vppParam.vpp.In,
                                                  nSurfNumVPPIn);
    VERIFY(MFX_ERR_NONE == sts, "Error in external surface allocation for VPP in\n");
    // 申请Out内存大小
    mfxU8 *vppOutBuf        = NULL;
    mfxFrameSurface1 *vppOutSurfacePool = (mfxFrameSurface1 *)calloc(sizeof(mfxFrameSurface1), nSurfNumVPPOut);
    sts               = AllocateExternalSystemMemorySurfacePool(&vppOutBuf,
                                                  vppOutSurfacePool,
                                                  vppParam.vpp.Out,
                                                  nSurfNumVPPOut);
    VERIFY(MFX_ERR_NONE == sts, "Error in external surface allocation for VPP out\n");
    // 4.3.2 申请Encode内存
    // 申请队列  Query number required surfaces for decoder
    mfxFrameAllocRequest encRequest = {};
    sts = MFXVideoENCODE_QueryIOSurf(session, &encodeParams, &encRequest);
    VERIFY(MFX_ERR_NONE == sts, "QueryIOSurf failed");
    // 申请输出流大小 Prepare output bitstream
    mfxBitstream bitstream;
    bitstream.MaxLength = BITSTREAM_BUFFER_SIZE;
    bitstream.Data      = (mfxU8 *)malloc(bitstream.MaxLength * sizeof(mfxU8));
    VERIFY(bitstream.Data != NULL, "calloc bitstream failed");
    // External (application) allocation of decode surfaces
    mfxU8 *encOutBuf = NULL;
    mfxFrameSurface1 *encSurfPool = (mfxFrameSurface1 *)calloc(sizeof(mfxFrameSurface1), encRequest.NumFrameSuggested);
    sts = AllocateExternalSystemMemorySurfacePool(&encOutBuf,
                                                  encSurfPool,
                                                  encodeParams.mfx.FrameInfo,
                                                  encRequest.NumFrameSuggested);
    VERIFY(MFX_ERR_NONE == sts, "Error in external surface allocation\n");

    // 5.开始编码
    mfxFrameSurface1* encSurfaceIn = NULL; // surface是一块内存区域
    mfxSyncPoint syncp = {};    // 同步指针，用于同步编码的异步处理流程
    bool isStillGoing = true;
    bool isDraining = false; // 是否凑够一帧，没凑够为false，凑够为true
    int nIndexVPPInSurf  = 0;
    int nIndexVPPOutSurf = 0;
    int nIndex = -1;
    mfxU32 framenum = 0;
    // 5.1.输出文件
#ifdef _MSC_VER
    FILE* sink;
    fopen_s(&sink, OUTPUT_FILE, "wb");
#else
    FILE* sink = fopen(OUTPUT_FILE, "wb");
#endif 
    VERIFY(sink, "Could not create output file");
    // 5.2.开始循环
    printf("Encode start!");
    while (isStillGoing) {
        cv::Mat image;
        cap >> image;
        cv::cvtColor(image, image, cv::COLOR_BGR2BGRA);
        // Load a new frame if not draining
        if (isDraining == false) {
            // 先把图读到vpp里，转I420
            nIndexVPPInSurf = GetFreeSurfaceIndex(vppInSurfacePool, nSurfNumVPPIn); // Find free input frame surface
            // nIndex       = GetFreeSurfaceIndex(encSurfPool, encRequest.NumFrameSuggested);
            // encSurfaceIn = &encSurfPool[nIndex];
            sts = ReadRawFrame(&vppInSurfacePool[nIndexVPPInSurf], image);
            // sts = ReadRawFrame(encSurfaceIn, image);
            if (sts != MFX_ERR_NONE)
                isDraining = true;
        }
        // 先取得一个vpp out surface，存放vpp输出结果
        nIndexVPPOutSurf = GetFreeSurfaceIndex(vppOutSurfacePool,
                                               nSurfNumVPPOut); // Find free output frame surface
        encSurfaceIn = &vppOutSurfacePool[nIndexVPPOutSurf];
        sts = MFXVideoVPP_RunFrameVPPAsync( session,
                                            (isDraining == true) ? NULL : &vppInSurfacePool[nIndexVPPInSurf],
                                            encSurfaceIn, //&vppOutSurfacePool[nIndexVPPOutSurf],
                                            NULL,
                                            &syncp);

        sts = MFXVideoENCODE_EncodeFrameAsync(session,
                                              NULL,
                                              (isDraining == true) ? NULL : encSurfaceIn,
                                              &bitstream,
                                              &syncp);

        switch (sts) {
            case MFX_ERR_NONE:
                // MFX_ERR_NONE and syncp indicate output is available
                if (syncp) {
                    printf("hear!");
                    // Encode output is not available on CPU until sync operation completes
                    sts = MFXVideoCORE_SyncOperation(session, syncp, 100 * 1000);
                    VERIFY(MFX_ERR_NONE == sts, "MFXVideoCORE_SyncOperation error");

                    WriteEncodedStream(bitstream, sink);
                    framenum++;
                }
                break;
            case MFX_ERR_NOT_ENOUGH_BUFFER:
                // This example deliberatly uses a large output buffer with immediate write to disk
                // for simplicity.
                // Handle when frame size exceeds available buffer here
                break;
            case MFX_ERR_MORE_DATA:
                // The function requires more data to generate any output
                if (isDraining)
                    isStillGoing = false;
                break;
            case MFX_ERR_DEVICE_LOST:
                // For non-CPU implementations,
                // Cleanup if device is lost
                break;
            case MFX_WRN_DEVICE_BUSY:
                // For non-CPU implementations,
                // Wait a few milliseconds then try again
                break;
            default:
                printf("unknown status %d\n", sts);
                isStillGoing = false;
                break;
        }
    }
    
    if (session) {
        MFXVideoENCODE_Close(session);
        MFXVideoVPP_Close(session);
        MFXClose(session);
    }

    if (vppInBuf || vppInSurfacePool) {
        FreeExternalSystemMemorySurfacePool(vppInBuf, vppInSurfacePool);
    }

    if (vppOutBuf || vppOutSurfacePool) {
        FreeExternalSystemMemorySurfacePool(vppOutBuf, vppOutSurfacePool);
    }

    if (bitstream.Data)
        free(bitstream.Data);

    if (encSurfPool || encOutBuf) {
        FreeExternalSystemMemorySurfacePool(encOutBuf, encSurfPool);
    }

    if(sink)
        fclose(sink);

    FreeAcceleratorHandle(accelHandle, accel_fd);
    accelHandle = NULL;
    accel_fd    = 0;

    if (loader)
        MFXUnload(loader);
    if (isFailed) {
        return -1;
    }
    return 0;
}


// 查看Impl最终配置
void ShowImplementationInfo(mfxLoader loader, mfxU32 implnum) {
    mfxImplDescription* idesc = nullptr;
    mfxStatus sts;

    // Loads info about implementation at specified list location
    sts = MFXEnumImplementations(loader, implnum, MFX_IMPLCAPS_IMPLDESCSTRUCTURE, (mfxHDL*)&idesc);
    if (!idesc || (sts != MFX_ERR_NONE)) {
        printf("cannot get mfxHDL, MFXEnumImplementations return %d\n", sts);
        return;
    }

    printf("Implementation details:\n");
    printf("  ApiVersion:           %hu.%hu  \n", idesc->ApiVersion.Major, idesc->ApiVersion.Minor);
    printf("  Implementation type:  %s\n", (idesc->Impl == MFX_IMPL_TYPE_SOFTWARE) ? "SW" : "HW");
    printf("  AccelerationMode via: ");
    switch (idesc->AccelerationMode) {
    case MFX_ACCEL_MODE_NA:
        printf("NA \n");
        break;
    case MFX_ACCEL_MODE_VIA_D3D9:
        printf("D3D9\n");
        break;
    case MFX_ACCEL_MODE_VIA_D3D11:
        printf("D3D11\n");
        break;
    case MFX_ACCEL_MODE_VIA_VAAPI:
        printf("VAAPI\n");
        break;
    case MFX_ACCEL_MODE_VIA_VAAPI_DRM_MODESET:
        printf("VAAPI_DRM_MODESET\n");
        break;
    case MFX_ACCEL_MODE_VIA_VAAPI_GLX:
        printf("VAAPI_GLX\n");
        break;
    case MFX_ACCEL_MODE_VIA_VAAPI_X11:
        printf("VAAPI_X11\n");
        break;
    case MFX_ACCEL_MODE_VIA_VAAPI_WAYLAND:
        printf("VAAPI_WAYLAND\n");
        break;
    case MFX_ACCEL_MODE_VIA_HDDLUNITE:
        printf("HDDLUNITE\n");
        break;
    default:
        printf("unknown\n");
        break;
    }
    MFXDispReleaseImplDescription(loader, idesc);

    // Show implementation path, added in 2.4 API
    mfxHDL implPath = nullptr;
    sts = MFXEnumImplementations(loader, implnum, MFX_IMPLCAPS_IMPLPATH, &implPath);
    if (!implPath || (sts != MFX_ERR_NONE))
        return;

    printf("  Path: %s\n\n", reinterpret_cast<mfxChar*>(implPath));
    MFXDispReleaseImplDescription(loader, implPath);
}

// 读一帧
mfxStatus ReadRawFrame(mfxFrameSurface1* surface, cv::Mat &RGB4) {
    mfxU16 w, h, i, pitch;
    size_t bytes_read;
    mfxU8* ptr;
    mfxFrameInfo* info = &surface->Info;
    mfxFrameData* data = &surface->Data;
 
    w = info->Width;
    h = info->Height;
    switch (info->FourCC) {
    case MFX_FOURCC_RGB4:
        pitch = data->Pitch;
        for (i = 0; i < h; i++) {
            memcpy(data->B + i * pitch, RGB4.data + i * RGB4.step, 1 * pitch);
            // bytes_read = memcpy(data->B + i * pitch, 1, pitch, f);
            // if (pitch != bytes_read)
            //     return MFX_ERR_MORE_DATA;
        }
        break;
    default:
        printf("Unsupported FourCC code, skip LoadRawFrame\n");
        break;
    }

    return MFX_ERR_NONE;
}
// 读一帧
mfxStatus ReadRawFrame(mfxFrameSurface1* surface, std::vector<cv::Mat> &vecBGR) {
    mfxU16 w, h, i, pitch;
    size_t bytes_read;
    mfxU8* ptr;
    mfxFrameInfo* info = &surface->Info;
    mfxFrameData* data = &surface->Data;
 
    w = info->Width;
    h = info->Height;

    switch (info->FourCC) {
    case MFX_FOURCC_RGB4:
        data->B = vecBGR[0].data;
        data->G = vecBGR[1].data;
        data->R = vecBGR[2].data;
        break;
    default:
        printf("Unsupported FourCC code, skip LoadRawFrame\n");
        break;
    }

    return MFX_ERR_NONE;
}
mfxStatus ReadRawFrame_InternalMem(mfxFrameSurface1* surface, std::vector<cv::Mat> &vecBGR) {
    bool is_more_data = false;

    // Map makes surface writable by CPU for all implementations
    mfxStatus sts = surface->FrameInterface->Map(surface, MFX_MAP_WRITE); // 开启读取权限，可以把内存写入surface->Data
    if (sts != MFX_ERR_NONE) {
        printf("mfxFrameSurfaceInterface->Map failed (%d)\n", sts);
        return sts;
    }

    sts = ReadRawFrame(surface, vecBGR);
    if (sts != MFX_ERR_NONE) {
        if (sts == MFX_ERR_MORE_DATA)
            is_more_data = true;
        else
            return sts;
    }

    // Unmap/release returns local device access for all implementations
    sts = surface->FrameInterface->Unmap(surface);  // 关闭读写权限
    if (sts != MFX_ERR_NONE) {
        printf("mfxFrameSurfaceInterface->Unmap failed (%d)\n", sts);
        return sts;
    }

    return (is_more_data == true) ? MFX_ERR_MORE_DATA : MFX_ERR_NONE;
}


// Write encoded stream to file
void WriteEncodedStream(mfxBitstream& bs, FILE* f) {
    fwrite(bs.Data + bs.DataOffset, 1, bs.DataLength, f);
    bs.DataLength = 0;
    return;
}


void *InitAcceleratorHandle(mfxSession session, int *fd) {
    mfxIMPL impl;
    mfxStatus sts = MFXQueryIMPL(session, &impl);
    if (sts != MFX_ERR_NONE)
        return NULL;

#ifdef LIBVA_SUPPORT
    if ((impl & MFX_IMPL_VIA_VAAPI) == MFX_IMPL_VIA_VAAPI) {
        if (!fd)
            return NULL;
        VADisplay va_dpy = NULL;
        // initialize VAAPI context and set session handle (req in Linux)
        *fd = open("/dev/dri/renderD128", O_RDWR);
        if (*fd >= 0) {
            va_dpy = vaGetDisplayDRM(*fd);
            if (va_dpy) {
                int major_version = 0, minor_version = 0;
                if (VA_STATUS_SUCCESS == vaInitialize(va_dpy, &major_version, &minor_version)) {
                    MFXVideoCORE_SetHandle(session,
                                           static_cast<mfxHandleType>(MFX_HANDLE_VA_DISPLAY),
                                           va_dpy);
                }
            }
        }
        return va_dpy;
    }
#endif

    return NULL;
}

void FreeAcceleratorHandle(void *accelHandle, int fd) {
#ifdef LIBVA_SUPPORT
    if (accelHandle) {
        vaTerminate((VADisplay)accelHandle);
    }
    if (fd) {
        close(fd);
    }
#endif
}

void FreeExternalSystemMemorySurfacePool(mfxU8 *dec_buf, mfxFrameSurface1 *surfpool) {
    if (dec_buf)
        free(dec_buf);

    if (surfpool)
        free(surfpool);
}

mfxU32 GetSurfaceSize(mfxU32 FourCC, mfxU32 width, mfxU32 height) {
    mfxU32 nbytes = 0;

    switch (FourCC) {
        case MFX_FOURCC_I420:
        case MFX_FOURCC_NV12:
            nbytes = width * height + (width >> 1) * (height >> 1) + (width >> 1) * (height >> 1);
            break;
        case MFX_FOURCC_I010:
        case MFX_FOURCC_P010:
            nbytes = width * height + (width >> 1) * (height >> 1) + (width >> 1) * (height >> 1);
            nbytes *= 2;
            break;
        case MFX_FOURCC_RGB4:
            nbytes = width * height * 4;
            break;
        default:
            break;
    }

    return nbytes;
}

mfxStatus AllocateExternalSystemMemorySurfacePool(mfxU8 **buf,
                                                  mfxFrameSurface1 *surfpool,
                                                  mfxFrameInfo frame_info,
                                                  mfxU16 surfnum) {
    // initialize surface pool (I420, RGB4 format)
    mfxU32 surfaceSize = GetSurfaceSize(frame_info.FourCC, frame_info.Width, frame_info.Height);
    if (!surfaceSize)
        return MFX_ERR_MEMORY_ALLOC;

    size_t framePoolBufSize = static_cast<size_t>(surfaceSize) * surfnum;
    *buf                    = reinterpret_cast<mfxU8 *>(calloc(framePoolBufSize, 1));

    mfxU16 surfW;
    mfxU16 surfH = frame_info.Height;

    if (frame_info.FourCC == MFX_FOURCC_RGB4) {
        surfW = frame_info.Width * 4;

        for (mfxU32 i = 0; i < surfnum; i++) {
            surfpool[i]            = { 0 };
            surfpool[i].Info       = frame_info;
            size_t buf_offset      = static_cast<size_t>(i) * surfaceSize;
            surfpool[i].Data.B     = *buf + buf_offset;
            surfpool[i].Data.G     = surfpool[i].Data.B + 1;
            surfpool[i].Data.R     = surfpool[i].Data.B + 2;
            surfpool[i].Data.A     = surfpool[i].Data.B + 3;
            surfpool[i].Data.Pitch = surfW;
        }
    }
    else {
        surfW = (frame_info.FourCC == MFX_FOURCC_P010) ? frame_info.Width * 2 : frame_info.Width;

        for (mfxU32 i = 0; i < surfnum; i++) {
            surfpool[i]            = { 0 };
            surfpool[i].Info       = frame_info;
            size_t buf_offset      = static_cast<size_t>(i) * surfaceSize;
            surfpool[i].Data.Y     = *buf + buf_offset;
            surfpool[i].Data.U     = *buf + buf_offset + (surfW * surfH);
            surfpool[i].Data.V     = surfpool[i].Data.U + ((surfW / 2) * (surfH / 2));
            surfpool[i].Data.Pitch = surfW;
        }
    }

    return MFX_ERR_NONE;
}

int GetFreeSurfaceIndex(mfxFrameSurface1 *SurfacesPool, mfxU16 nPoolSize) {
    for (mfxU16 i = 0; i < nPoolSize; i++) {
        if (0 == SurfacesPool[i].Data.Locked)
            return i;
    }
    return MFX_ERR_NOT_FOUND;
}

