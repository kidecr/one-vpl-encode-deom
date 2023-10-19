#include "vpl-encode-module.hpp"
#include <opencv2/opencv.hpp>


int main(int argc, char* argv[])
{
    std::string outputfilename = "out.h265";
    std::string devicename = "/dev/video0";
    if(argc > 1)
        devicename = std::string(argv[1]);
    if(argc > 2)
        outputfilename = std::string(argv[2]);

    cv::VideoCapture cap(devicename);
    if(!cap.isOpened())
        return -1;
    int h = cap.get(cv::CAP_PROP_FRAME_HEIGHT);
    int w = cap.get(cv::CAP_PROP_FRAME_WIDTH);
    printf("size : [%d, %d]\n", w, h);
    VplEncodeModule v(outputfilename, w, h);

    while (true)
    {
        cv::Mat image;
        cap >> image;
        v.push(image);

        cv::imshow("image", image);
        if(cv::waitKey(10) == 'q')
            break;
    }
    
}