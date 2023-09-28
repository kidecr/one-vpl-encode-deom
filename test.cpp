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
void WriteEncodedStream(mfxBitstream & bs, FILE * f);

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
    sts = MFXVideoVPP_Init(session, &vppParam);
    std::cout << sts << std::endl;
    VERIFY(MFX_ERR_NONE == sts, "VPP init failed");
    sts = MFXVideoENCODE_Init(session, &encodeParams);
    std::cout << sts << std::endl;
    VERIFY(MFX_ERR_NONE == sts, "Encode init failed");
    // 创建vpp
    // 4.3 申请输出比特流，并开辟缓冲区
    mfxBitstream bitstream;
    bitstream.MaxLength = BITSTREAM_BUFFER_SIZE;
    bitstream.Data = (mfxU8*)calloc(bitstream.MaxLength, sizeof(mfxU8));
    VERIFY(bitstream.Data == NULL, "calloc bitstream failed");

    // 5.开始编码
    mfxFrameSurface1* encSurfaceIn = NULL; // surface是一块内存区域
    mfxSyncPoint syncp = {};    // 同步指针，用于同步编码的异步处理流程
    bool keepEncoding = true;
    bool isDraining = false; // 是否凑够一帧，没凑够为false，凑够为true
    // 5.1.输出文件
#ifdef _MSC_VER
    FILE* sink;
    fopen_s(&sink, OUTPUT_FILE, "wb");
#else
    FILE* sink = fopen(OUTPUT_FILE, "wb");
#endif 
    VERIFY(sink, "Could not create output file");
    // 5.2.开始循环
    while (keepEncoding)
    {
        cv::Mat image;
        std::vector<cv::Mat> vecBGR;
        cap >> image;
        // Load a new frame if not draining
        if (isDraining == false) {
            sts = MFXMemory_GetSurfaceForEncode(session, &encSurfaceIn);
            VERIFY(MFX_ERR_NONE == sts, "Could not get encode surface");

            if (encSurfaceIn == NULL) printf("enSurfaceIn is NULL");

            cv::split(image, vecBGR);
            sts = ReadRawFrame_InternalMem(encSurfaceIn, vecBGR);
            if (sts != MFX_ERR_NONE)
                isDraining = true;
        }

        sts = MFXVideoENCODE_EncodeFrameAsync(session,
            NULL,
            (isDraining == true) ? NULL : encSurfaceIn,
            &bitstream,
            &syncp);

        if (!isDraining) {
            mfxStatus sts_r = encSurfaceIn->FrameInterface->Release(encSurfaceIn);
            VERIFY(MFX_ERR_NONE == sts_r, "mfxFrameSurfaceInterface->Release failed");
        }
        switch (sts) {
        case MFX_ERR_NONE:
            // MFX_ERR_NONE and syncp indicate output is available
            if (syncp) {
                // Encode output is not available on CPU until sync operation
                // completes
                sts = MFXVideoCORE_SyncOperation(session, syncp, 100);
                VERIFY(MFX_ERR_NONE == sts, "MFXVideoCORE_SyncOperation error");

                WriteEncodedStream(bitstream, sink);
            }
            break;
        case MFX_ERR_NOT_ENOUGH_BUFFER:
            // This example deliberatly uses a large output buffer with immediate
            // write to disk for simplicity. Handle when frame size exceeds
            // available buffer here
            break;
        case MFX_ERR_MORE_DATA:
            // The function requires more data to generate any output
            if (isDraining == true)
                keepEncoding = false;
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
            keepEncoding = false;
            break;
        }
    }
    if (session)
        MFXClose(session);
    if (loader)
        MFXUnload(loader);
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