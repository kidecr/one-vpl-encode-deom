#include "vpl-encode-module.hpp"
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <thread>
#include <queue>

// 错误检查
#define VERIFY(x, y)       \
    if (!(x)) {            \
        printf("%s\n", y); \
        throw std::exception();          \
    }

// 计算VPL版本
#define VPLVERSION(major, minor)    (major << 16 | minor)
// 取整到16和32
#define ALIGN16(value)              (((value + 15) >> 4) << 4)
#define ALIGN32(X)                  (((mfxU32)((X) + 31)) & (~(mfxU32)31))
// 设置输出流大小
#define BITSTREAM_BUFFER_SIZE       2000000

VplEncodeModule::VplEncodeModule(std::string file_path, int imageWight, int imageHeight)
{
    // 1.先load
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
    VERIFY(NULL != vppCodecConfig, "MFXCreateConfig failed");
    mfxVariant vppCodecValue = {0};
    vppCodecValue.Type     = MFX_VARIANT_TYPE_U32;
    vppCodecValue.Data.U32 = MFX_EXTBUFF_VPP_SCALING;
    sts = MFXSetConfigFilterProperty(vppCodecConfig, (mfxU8 *)"mfxImplDescription.mfxVPPDescription.filter.FilterFourCC", vppCodecValue);
    VERIFY(MFX_ERR_NONE == sts, "MFXSetConfigFilterProperty failed");
    // 2.4.设置API版本(可以不用)，关于vpl是如何搜索API版本的，https://spec.oneapi.io/versions/latest/elements/oneVPL/source/programming_guide/VPL_prg_session.html
	//mfxConfig apiVersionConfig = MFXCreateConfig(loader);
	//VERIFY(NULL != apiVersionConfig, "MFXCreateConfig failed")
	//mfxVariant apiVersionValue;
	//apiVersionValue.Type = MFX_VARIANT_TYPE_U32;
	//apiVersionValue.Data.U32 = VPLVERSION(2, 7);	// API版本设为2.7
	//sts = MFXSetConfigFilterProperty(apiVersionConfig, (mfxU8*)"mfxImplDescription.ApiVersion.Version", apiVersionValue);
	//VERIFY(MFX_ERR_NONE == sts, "MFXSetConfigFilterProperty failed for API version");
    // 2.5.打印一下最终设置结果
    ShowImplementationInfo(loader, 0);

    // 3.创建session
    // 一个loader可以创建多个session，一个session可以具有多条处理流，一个程序可以创建多个loader
    // 多loader和多处理流 https://spec.oneapi.io/versions/latest/elements/oneVPL/source/programming_guide/VPL_prg_session.html#examples-of-dispatcher-s-usage
    // 多session https://spec.oneapi.io/versions/latest/elements/oneVPL/source/programming_guide/VPL_prg_session.html#multiple-sessions
	sts = MFXCreateSession(loader, 0, &session);
	VERIFY(MFX_ERR_NONE == sts, "Cannot create session -- no implementations meet selection criteria");
    // 3.1 创建一下加速器 Convenience function to initialize available accelerator(s)
    accelHandle = InitAcceleratorHandle(session, &accel_fd);

    // 4.初始化编码器和VPP
    // 4.1.设置参数 
    encodeParam = SetEncodeParam(imageWight, imageHeight);
    vppParam = SetVPPParam(imageWight, imageHeight);
    // 4.2.填补和矫正不和里参数
    sts = MFXVideoENCODE_Query(session, &encodeParam, &encodeParam);
    VERIFY(MFX_ERR_NONE == sts, "Encode query failed");
    // 4.3.创建编码器
    sts = MFXVideoENCODE_Init(session, &encodeParam);
    VERIFY(MFX_ERR_NONE == sts, "Encode init failed");
    // 4.4.创建vpp
    sts = MFXVideoVPP_Init(session, &vppParam);
    VERIFY(MFX_ERR_NONE == sts, "VPP init failed");

    // 5.申请内存
    // 5.1.申请VPP内存
    // 5.1.1.创建IO队列 Query number of required surfaces for VPP
    mfxFrameAllocRequest VPPRequest[2]  = {};
    sts = MFXVideoVPP_QueryIOSurf(session, &vppParam, VPPRequest);
    VERIFY(MFX_ERR_NONE == sts, "Error in QueryIOSurf");
    // 5.1.2.获取IN和OUT的推荐数量
    nSurfNumVPPIn  = VPPRequest[0].NumFrameSuggested; // vpp in
    nSurfNumVPPOut = VPPRequest[1].NumFrameSuggested; // vpp out
    // 5.1.3.申请In内存大小
    vppInSurfacePool = (mfxFrameSurface1 *)calloc(sizeof(mfxFrameSurface1), nSurfNumVPPIn);

    sts = AllocateExternalSystemMemorySurfacePool(&vppInBuf,
                                                  vppInSurfacePool,
                                                  vppParam.vpp.In,
                                                  nSurfNumVPPIn);
    VERIFY(MFX_ERR_NONE == sts, "Error in external surface allocation for VPP in\n");
    // 5.1.4.申请Out内存大小
    vppOutSurfacePool = (mfxFrameSurface1 *)calloc(sizeof(mfxFrameSurface1), nSurfNumVPPOut);
    sts               = AllocateExternalSystemMemorySurfacePool(&vppOutBuf,
                                                  vppOutSurfacePool,
                                                  vppParam.vpp.Out,
                                                  nSurfNumVPPOut);
    VERIFY(MFX_ERR_NONE == sts, "Error in external surface allocation for VPP out\n");
    // 5.2.申请Encode内存
    // 5.2.1.申请队列  Query number required surfaces for decoder
    mfxFrameAllocRequest encRequest = {0};
    sts = MFXVideoENCODE_QueryIOSurf(session, &encodeParam, &encRequest);
    VERIFY(MFX_ERR_NONE == sts, "QueryIOSurf failed");
    // 5.2.2.申请输出流大小 Prepare output bitstream
    bitstream.MaxLength = BITSTREAM_BUFFER_SIZE;
    bitstream.Data      = (mfxU8 *)malloc(bitstream.MaxLength * sizeof(mfxU8));
    VERIFY(bitstream.Data != NULL, "calloc bitstream failed");
    // // 5.2.3.申请输入surface pool，（用不上了，直接用VPP的输出代替）External (application) allocation of decode surfaces
    // mfxU8 *encOutBuf = NULL;
    // mfxFrameSurface1 *encSurfPool = (mfxFrameSurface1 *)calloc(sizeof(mfxFrameSurface1), encRequest.NumFrameSuggested);
    // sts = AllocateExternalSystemMemorySurfacePool(&encOutBuf,
    //                                               encSurfPool,
    //                                               encodeParam.mfx.FrameInfo,
    //                                               encRequest.NumFrameSuggested);
    // VERIFY(MFX_ERR_NONE == sts, "Error in external surface allocation\n");
    // 6.创建并打开输出文件
    sink = fopen(file_path.c_str(), "wb");
    VERIFY(sink != NULL, "open output file failed");
}

mfxVideoParam VplEncodeModule::SetEncodeParam(int w, int h)
{
    mfxVideoParam encodeParam = {0}; // 参数解释 https://spec.oneapi.io/versions/latest/elements/oneVPL/source/API_ref/VPL_structs_cross_component.html?highlight=mfxvideoparam#mfxvideoparam
    encodeParam.mfx.CodecId = MFX_CODEC_HEVC;  // 编码器
    // encodeParam.mfx.CodecProfile = MFX_PROFILE_AVC_CONSTRAINED_BASELINE;   // 使用默认配置参数
    // encodeParam.mfx.CodecLevel = MFX_LEVEL_AVC_5;  // 使用的编码器级别
    encodeParam.mfx.TargetUsage = MFX_TARGETUSAGE_BALANCED; // 速度和质量的平衡度
    encodeParam.mfx.RateControlMethod = MFX_RATECONTROL_VBR; // 可变比特率控制算法
    encodeParam.mfx.TargetKbps = 4000; // kbps
    encodeParam.mfx.FrameInfo.FrameRateExtN = 30; // 帧率设置 帧率 = FrameRateExtN / FrameRateExtD
    encodeParam.mfx.FrameInfo.FrameRateExtD = 1;
    encodeParam.mfx.FrameInfo.FourCC = MFX_FOURCC_I420; 
    encodeParam.mfx.FrameInfo.ChromaFormat = MFX_CHROMAFORMAT_YUV420; // 颜色采样方法
    encodeParam.mfx.FrameInfo.CropW = w;  // 原图宽（是ROI，可以比原图小，指定方法{X，Y，W，H})
    encodeParam.mfx.FrameInfo.CropH = h; // 原图高
    encodeParam.mfx.FrameInfo.Width = ALIGN16(w); // 目标宽 必须为16的倍数
    encodeParam.mfx.FrameInfo.Height = ALIGN16(h);   // 目标高 对逐行帧，必须为16的倍数，否则为32的倍数
    encodeParam.IOPattern = MFX_IOPATTERN_IN_SYSTEM_MEMORY; // 函数的输入和输出存储器访问类型

    return encodeParam;
}

mfxVideoParam VplEncodeModule::SetVPPParam(int w, int h)
{
    mfxVideoParam vppParam = {0}; // 必须用0初始化，防止有些参数出现未知值
    vppParam.IOPattern = MFX_IOPATTERN_IN_SYSTEM_MEMORY | MFX_IOPATTERN_OUT_SYSTEM_MEMORY;
    vppParam.vpp.In.FourCC = MFX_FOURCC_RGB4;
    vppParam.vpp.In.ChromaFormat  = MFX_CHROMAFORMAT_YUV420;
    vppParam.vpp.In.CropX         = 0;
    vppParam.vpp.In.CropY         = 0;
    vppParam.vpp.In.CropW         = w;
    vppParam.vpp.In.CropH         = h;
    vppParam.vpp.In.PicStruct     = MFX_PICSTRUCT_PROGRESSIVE;
    vppParam.vpp.In.FrameRateExtN = 30;
    vppParam.vpp.In.FrameRateExtD = 1;
    vppParam.vpp.In.Width = vppParam.vpp.Out.Width = ALIGN16(w);
    vppParam.vpp.In.Height = vppParam.vpp.Out.Height = ALIGN16(h);

    vppParam.vpp.Out.FourCC = MFX_FOURCC_I420;
    vppParam.vpp.Out.ChromaFormat  = MFX_CHROMAFORMAT_YUV420;
    vppParam.vpp.Out.CropX         = 0;
    vppParam.vpp.Out.CropY         = 0;
    vppParam.vpp.Out.CropW         = w;
    vppParam.vpp.Out.CropH         = h;
    vppParam.vpp.Out.PicStruct     = MFX_PICSTRUCT_PROGRESSIVE;
    vppParam.vpp.Out.FrameRateExtN = 30;
    vppParam.vpp.Out.FrameRateExtD = 1;
    vppParam.vpp.Out.Width = vppParam.vpp.Out.Width = ALIGN16(w);
    vppParam.vpp.Out.Height = vppParam.vpp.Out.Height = ALIGN16(h);

    return vppParam;
}

void VplEncodeModule::push(cv::Mat image)
{
    cv::Mat input;
    if(image.elemSize() == 3)
        cv::cvtColor(image, input, cv::COLOR_BGR2BGRA);
    else if(image.elemSize() == 1)
        cv::cvtColor(image, input, cv::COLOR_GRAY2BGRA);
    else
        image.copyTo(input);
    std::lock_guard<std::mutex> lock(imageQueueLock);
    imageQueue.push(input);

    if(!start){
        std::thread t(&VplEncodeModule::EncodeLoop, this);
        t.detach();
        start = true;
    }
}

VplEncodeModule::~VplEncodeModule()
{
    isStillGoing = false;
    while (start) usleep(1e2); // 等待进程结束

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

    if(sink)
        fclose(sink);

    FreeAcceleratorHandle(accelHandle, accel_fd);

    if (loader)
        MFXUnload(loader);
}

void VplEncodeModule::EncodeLoop()
{
    while (isStillGoing) {
        timeval tv1;
        gettimeofday(&tv1, nullptr);
        // Load a new frame if not draining
        // 先把图读到vpp里，转I420
        while( (nIndexVPPInSurf = GetFreeSurfaceIndex(vppInSurfacePool, nSurfNumVPPIn)) < 0 ) usleep(1e3); // Find free input frame surface
        printf("get input free index %d\n", nIndexVPPInSurf);

        sts = ReadFrame(&vppInSurfacePool[nIndexVPPInSurf]);
        if(sts != MFX_ERR_NONE) {
            usleep(1e3);
            continue;
            printf("no image\n");
        }
        printf("have image %d\n", (int)!noImage);

        // 先取得一个vpp out surface，存放vpp输出结果
        while( (nIndexVPPOutSurf = GetFreeSurfaceIndex(vppOutSurfacePool, nSurfNumVPPOut)) < 0) usleep(1e3); // Find free output frame surface
        printf("get output free index %d\n", nIndexVPPOutSurf);

        sts = MFXVideoVPP_RunFrameVPPAsync( session,
                                            (noImage == true) ? NULL : &vppInSurfacePool[nIndexVPPInSurf],
                                            &vppOutSurfacePool[nIndexVPPOutSurf], //&vppOutSurfacePool[nIndexVPPOutSurf],
                                            NULL,
                                            &syncp);
        printf("VPP OK, sts %d\n", sts);
        switch (sts)
        {
        case MFX_ERR_NONE:
            // printf("VPP: MFX_ERR_NONE\n");
            break;
        case MFX_ERR_MORE_DATA:
            // printf("VPP : MFX_ERR_MORE_DATA\n");
            continue;
            // The function requires more data to generate any output
            break;
        default:
            break;
        }
        sts = MFXVideoENCODE_EncodeFrameAsync(session,
                                              NULL,
                                              (noImage == true) ? NULL : &vppOutSurfacePool[nIndexVPPOutSurf],
                                              &bitstream,
                                              &syncp);
        printf("Encode OK, sts %d\n", sts);
        switch (sts) {
            case MFX_ERR_NONE:
                // MFX_ERR_NONE and syncp indicate output is available
                if (syncp) {
                    // Encode output is not available on CPU until sync operation completes
                    sts = MFXVideoCORE_SyncOperation(session, syncp, 100 * 1000);
                    VERIFY(MFX_ERR_NONE == sts, "MFXVideoCORE_SyncOperation error");

                    WriteEncodedStream(bitstream, sink);
                    printf("write encode stream\n");
                    timeval tv2;
                    gettimeofday(&tv2, nullptr);
                    printf("%lf ms\n", (tv2.tv_sec - tv1.tv_sec) * 1e3 + (tv2.tv_usec - tv1.tv_usec) * 1e-3);
                }
                break;
            case MFX_ERR_NOT_ENOUGH_BUFFER:
                // printf("ENCODE : MFX_ERR_NOT_ENOUGH_BUFFER\n");
                // This example deliberatly uses a large output buffer with immediate write to disk
                // for simplicity.
                // Handle when frame size exceeds available buffer here
                break;
            case MFX_ERR_MORE_DATA:
                // printf("ENCODE : MFX_ERR_MORE_DATA\n");
                // The function requires more data to generate any output
                break;
            case MFX_ERR_DEVICE_LOST:
                // printf("ENCODE : MFX_ERR_DEVICE_LOST\n");
                // For non-CPU implementations,
                // Cleanup if device is lost
                break;
            case MFX_WRN_DEVICE_BUSY:
                // printf("ENCODE : MFX_WRN_DEVICE_BUSY\n");
                // For non-CPU implementations,
                // Wait a few milliseconds then try again
                break;
            case MFX_ERR_INCOMPATIBLE_VIDEO_PARAM:
                // printf("ENCODE : MFX_ERR_INCOMPATIBLE_VIDEO_PARAM\n");
                break;
            default:
                break;
        }
        printf("loop end\n");
        usleep(1e3);
    }
    start = false;
}

// 读一帧
mfxStatus VplEncodeModule::ReadFrame(mfxFrameSurface1* surface) {

    cv::Mat RGB4;
    {
        std::lock_guard<std::mutex> lock(imageQueueLock);
        if(imageQueue.empty()) {
            noImage = true;
            return MFX_ERR_UNKNOWN;
        }
        RGB4 = imageQueue.front();
        imageQueue.pop();
        noImage = false;
        printf("get one frame\n");
    }

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
        }
        break;
    default:
        printf("Unsupported FourCC code, skip LoadRawFrame\n");
        break;
    }

    return MFX_ERR_NONE;
}

int VplEncodeModule::GetFreeSurfaceIndex(mfxFrameSurface1 *SurfacesPool, mfxU16 nPoolSize) {
    for (mfxU16 i = 0; i < nPoolSize; i++) {
        if (0 == SurfacesPool[i].Data.Locked)
            return i;
    }
    return MFX_ERR_NOT_FOUND;
}

// Write encoded stream to file
void VplEncodeModule::WriteEncodedStream(mfxBitstream& bs, FILE* f) {
    fwrite(bs.Data + bs.DataOffset, 1, bs.DataLength, f);
    bs.DataLength = 0;
    return;
}

// 查看Impl最终配置
void VplEncodeModule::ShowImplementationInfo(mfxLoader loader, mfxU32 implnum) {
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

void* VplEncodeModule::InitAcceleratorHandle(mfxSession session, int *fd) {
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

mfxStatus VplEncodeModule::AllocateExternalSystemMemorySurfacePool(mfxU8 **buf,
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

mfxU32 VplEncodeModule::GetSurfaceSize(mfxU32 FourCC, mfxU32 width, mfxU32 height) {
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

void VplEncodeModule::FreeExternalSystemMemorySurfacePool(mfxU8 *buf, mfxFrameSurface1 *surfpool) {
    if (buf)
        free(buf);

    if (surfpool)
        free(surfpool);
}

void VplEncodeModule::FreeAcceleratorHandle(void *accelHandle, int fd) {
#ifdef LIBVA_SUPPORT
    if (accelHandle) {
        vaTerminate((VADisplay)accelHandle);
    }
    if (fd) {
        close(fd);
    }
#endif
}

