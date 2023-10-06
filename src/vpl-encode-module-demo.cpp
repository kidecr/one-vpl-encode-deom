#include "vpl-encode-module.hpp"
#include <opencv2/opencv.hpp>


int main()
{
    cv::VideoCapture cap(0);
    if(!cap.isOpened())
        return -1;
    int h = cap.get(cv::CAP_PROP_FRAME_HEIGHT);
    int w = cap.get(cv::CAP_PROP_FRAME_WIDTH);
    printf("size : [%d, %d]\n", w, h);
    VplEncodeModule v("out.h265", w, h);

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