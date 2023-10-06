#ifndef __VPL_ENCODE_MODULE_HPP__
#define __VPL_ENCODE_MODULE_HPP__

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vpl/mfx.h>
#include <opencv2/opencv.hpp>

class VplEncodeModule
{
public:
    /**
     * @brief 构造函数，初始化和申请内存
     * 
     * @param file_path 输出文件路径
     */
    VplEncodeModule(std::string file_path, int imageWight, int imageHeight);
    /**
     * @brief 析构函数，释放内存
     * 
     */
    ~VplEncodeModule();

    /**
     * @brief 向编码队列里增加一帧
     * 
     * @param image 输入图像，要求大小和构造函数中相同，不能为空图
     */
    void push(cv::Mat image);

private:
    int sts = 0; // MFX_ERR_NONE=0, 其他报错为负数 https://spec.oneapi.io/versions/latest/elements/oneVPL/source/API_ref/VPL_enums.html?highlight=mfx_err_none#mfxstatus
    bool useHardware = false;

    mfxLoader loader = NULL; // loader handle
    mfxSession session = NULL; // 任务
    mfxVideoParam encodeParam = {0};    // encode 参数
    mfxVideoParam vppParam = {0};       // vpp 参数
    mfxU16 nSurfNumVPPIn = 0;           // VPP 推荐输入surface loop大小
    mfxU16 nSurfNumVPPOut = 0;          // VPP 推荐输出surface loop大小
    mfxU8 *vppInBuf = NULL;             // vpp 输入内存，用于存储图像
    mfxU8 *vppOutBuf = NULL;            // vpp 输出内存，用于存储图像，兼用于encode输入
    mfxFrameSurface1 *vppInSurfacePool = NULL;  // vpp输入内存池，用于存储SurfacePool信息
    mfxFrameSurface1 *vppOutSurfacePool = NULL; // vpp输出内存池，用于存储SurfacePool信息，兼用于encode输入
    mfxBitstream bitstream;             // Encode输出bit流
    mfxSyncPoint syncp = {};            // 同步指针，用于同步编码的异步处理流程
    int accel_fd = 0;                   // 加速器 fd
    void *accelHandle = NULL;           // 加速器 handle

    bool isStillGoing = true;   // 标识是否继续编码
    bool noImage = false;       // 是否还有未编码的图像
    int nIndexVPPInSurf  = -1;  // 当前使用的surface在输入loop中的index
    int nIndexVPPOutSurf = -1;  // 当前使用的surface在输出loop中的index，兼为encode输入loop索引
    FILE* sink = NULL;          // 输出文件

    std::queue<cv::Mat> imageQueue; // 输入图像队列
    std::mutex imageQueueLock;

    bool start = false;

private:
    /**
     * @brief 主循环
     * 
     */
    void EncodeLoop();
    /**
     * @brief 查看Impl配置
     * 
     * @param loader loader handle
     * @param implnum impl索引编号
     */
    void ShowImplementationInfo(mfxLoader loader, mfxU32 implnum);
    /**
     * @brief 初始化加速器
     * 
     * @param session 
     * @param fd 
     * @return void* 
     */
    void *InitAcceleratorHandle(mfxSession session, int *fd);
    /**
     * @brief 设置Encode参数
     * 
     * @return mfxVideoParam 
     */
    mfxVideoParam SetEncodeParam(int w, int h);
    /**
     * @brief 设置VPP参数
     * 
     * @return mfxVideoParam 
     */
    mfxVideoParam SetVPPParam(int w, int h);
    /**
     * @brief 根据不同都FOURCC创建不同大小的内存空间，并构造surface loop
     * 
     * @param buf 内存空间指针
     * @param surfpool surface pool指针
     * @param frame_info 类型
     * @param surfnum surface loop中的surface数量
     * @return mfxStatus 
     */
    mfxStatus AllocateExternalSystemMemorySurfacePool(mfxU8 **buf,
                                                  mfxFrameSurface1 *surfpool,
                                                  mfxFrameInfo frame_info,
                                                  mfxU16 surfnum);
    
    /**
     * @brief 获取对应格式的surface大小
     * 
     * @param FourCC 格式
     * @param width 图像宽
     * @param height 图像高
     * @return mfxU32 
     */
    mfxU32 GetSurfaceSize(mfxU32 FourCC, mfxU32 width, mfxU32 height);
    /**
     * @brief 释放buffer和内存池
     * 
     * @param buf 
     * @param surfpool 
     */
    void FreeExternalSystemMemorySurfacePool(mfxU8 *buf, mfxFrameSurface1 *surfpool);
    /**
     * @brief 释放加速器
     * 
     * @param accelHandle 
     * @param fd 
     */
    void FreeAcceleratorHandle(void *accelHandle, int fd);
    /**
     * @brief 将队列中的一张图转surface
     * 
     * @param surface 
     * @return mfxStatus 
     */
    mfxStatus ReadFrame(mfxFrameSurface1* surface);
    /**
     * @brief 向文件中写bit流数据
     * 
     * @param bs bit流
     * @param f 目标文件
     */
    void WriteEncodedStream(mfxBitstream& bs, FILE* f);
    /**
     * @brief 从pool中找到空闲的surface的id
     * 
     * @param SurfacesPool 
     * @param nPoolSize 
     * @return int id，如果没找到，返回MFX_ERR_NOT_FOUND
     */
    int GetFreeSurfaceIndex(mfxFrameSurface1 *SurfacesPool, mfxU16 nPoolSize);
};


#endif // __VPL_ENCODE_MODULE_HPP__