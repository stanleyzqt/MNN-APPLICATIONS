#include <iostream>
#include <stdio.h>
#include "mssd.h"
#include <time.h>

int main(void)
{
    std::string image_name = "./image.jpg";
    std::string model_name = "./face_det_quant.mnn";
    int forward = MNN_FORWARD_CPU;
    // int forward = MNN_FORWARD_OPENCL;
    int precision = 0;

    // read image 
    cv::Mat raw_image    = cv::imread(image_name.c_str());
    int raw_image_height = raw_image.rows;
    int raw_image_width  = raw_image.cols; 
    cv::Mat image;
    cv::resize(raw_image, image, cv::Size(INPUT_SIZE, INPUT_SIZE));

    // load and config mnn model
    auto revertor = std::unique_ptr<Revert>(new Revert(model_name.c_str()));
    revertor->initialize();
    auto modelBuffer      = revertor->getBuffer();
    const auto bufferSize = revertor->getBufferSize();
    auto net = std::shared_ptr<MNN::Interpreter>(MNN::Interpreter::createFromBuffer(modelBuffer, bufferSize));
    revertor.reset();
    MNN::ScheduleConfig config;
    config.numThread = 1;
    config.type      = static_cast<MNNForwardType>(forward);
    MNN::BackendConfig backendConfig;
    backendConfig.precision = (MNN::BackendConfig::PrecisionMode)precision;
    config.backendConfig = &backendConfig;
    auto session = net->createSession(config);


    clock_t start = clock();
    // preprocessing
    float img_mean = 123.0f;
    float img_std  = 58.0f;
    image.convertTo(image, CV_32FC3);
    image = (image - img_mean) / img_std;

    // wrapping input tensor, convert nhwc to nchw    
    std::vector<int> dims{1, INPUT_SIZE, INPUT_SIZE, 3};
    auto nhwc_Tensor = MNN::Tensor::create<float>(dims, NULL, MNN::Tensor::TENSORFLOW);
    auto nhwc_data   = nhwc_Tensor->host<float>();
    auto nhwc_size   = nhwc_Tensor->size();
    ::memcpy(nhwc_data, image.data, nhwc_size);

    std::string input_tensor = "normalized_input_image_tensor";
    auto inputTensor  = net->getSessionInput(session, nullptr);
    inputTensor->copyFromHostTensor(nhwc_Tensor);


    // run network
    net->runSession(session);

    // get output data
    std::string output_tensor_name0 = "concat";
    std::string output_tensor_name1 = "concat_1";

    MNN::Tensor *tensor_scores = net->getSessionOutput(session, output_tensor_name0.c_str());
    MNN::Tensor *tensor_boxes  = net->getSessionOutput(session, output_tensor_name1.c_str());


    MNN::Tensor tensor_scores_host(tensor_scores, tensor_scores->getDimensionType());
    MNN::Tensor tensor_boxes_host(tensor_boxes, tensor_boxes->getDimensionType());

    tensor_scores->copyToHostTensor(&tensor_scores_host);
    tensor_boxes->copyToHostTensor(&tensor_boxes_host);

    // post processing step, DIY NMS, 
    // find biggest face
    float maxProb = 0.0f;
    auto scores_dataPtr = tensor_scores_host.host<float>();
    auto boxes_dataPtr  = tensor_boxes_host.host<float>();
    cv::Rect biggest_face;
    for(int i = 0; i < OUTPUT_NUM; ++i)
    {
        // location decoding
        float ycenter =     boxes_dataPtr[i + 0 * OUTPUT_NUM] / Y_SCALE  * anchors[2][i] + anchors[0][i];
        float xcenter =     boxes_dataPtr[i + 1 * OUTPUT_NUM] / X_SCALE  * anchors[3][i] + anchors[1][i];
        float h       = exp(boxes_dataPtr[i + 2 * OUTPUT_NUM] / H_SCALE) * anchors[2][i];
        float w       = exp(boxes_dataPtr[i + 3 * OUTPUT_NUM] / W_SCALE) * anchors[3][i];

        float ymin    = ( ycenter - h * 0.5 ) * raw_image_height;
        float xmin    = ( xcenter - w * 0.5 ) * raw_image_width;
        float ymax    = ( ycenter + h * 0.5 ) * raw_image_height;
        float xmax    = ( xcenter + w * 0.5 ) * raw_image_width;

        // probability decoding, softmax
        float nonface_prob = exp(scores_dataPtr[i*2 + 0]);
        float face_prob    = exp(scores_dataPtr[i*2 + 1]);
        float ss           = nonface_prob + face_prob;
        nonface_prob       /= ss;
        face_prob          /= ss;
        
        if (face_prob > face_prob_thresh && face_prob > maxProb) {
            if (xmin > 0 && ymin > 0 && xmax < raw_image_width && ymax < raw_image_height) {
                maxProb = face_prob;
                biggest_face.x = (int) xmin;
                biggest_face.y = (int) ymin;
                biggest_face.width  = (int) (xmax - xmin);
                biggest_face.height = (int) (ymax - ymin); 
            }
        }
    }
    clock_t end = clock();
    float duration = (float) (end - start) / CLOCKS_PER_SEC;
    printf("time: %f\n", duration);

    cv::rectangle(raw_image, biggest_face, cv::Scalar(0,0,255), 1);
    cv::imwrite("./output.jpg", raw_image);

    return 0;
}