/**
*
* Copyright(c)<2018>, <Huawei Technologies Co.,Ltd>
*
* @version 1.0
*
* @date 2018-5-30
*/
#include "SaveFilePostProcess_1.h"
#include <hiaiengine/log.h>
#include <vector>
#include <unistd.h>
#include <thread>
#include <fstream>
#include <algorithm>
#include <iostream>
#include <stdlib.h>
#include <sys/stat.h>
#include <sstream>
#include <fcntl.h>
#include "opencv2/opencv.hpp"
#include "opencv2/core.hpp"
#include "opencv2/imgproc.hpp"
#include "opencv2/imgcodecs/imgcodecs_c.h"

HIAI_StatusT SaveFilePostProcess_1::Init(const hiai::AIConfig& config, const  std::vector<hiai::AIModelDescription>& model_desc)
{
    HIAI_ENGINE_LOG(HIAI_IDE_INFO, "[SaveFilePostProcess_1] start init!");
    if (NULL == postprocess_config_)
    {
        postprocess_config_ = std::make_shared<PostprocessConfig>();
    }
    for (int index = 0; index < config.items_size(); ++index)
    {
        const ::hiai::AIConfigItem& item = config.items(index);
        std::string name = item.name();
        if(name == "path"){
            postprocess_config_->path = item.value();
            break;
        }
    }
    std::string datainfo_path = postprocess_config_->path;
    while (datainfo_path.back() == '/' || datainfo_path.back() == '\\') {
        datainfo_path.pop_back();
    }

    std::size_t tmp_ind = datainfo_path.find_last_of("/\\");
    postprocess_config_->info_file = "." + datainfo_path.substr(tmp_ind + 1) + "_data.info";
    std::string info_file_ = datainfo_path + "/" + postprocess_config_->info_file;
    id_img_correlation.clear();
    char path[PATH_MAX] = {0};
    if(realpath(info_file_.c_str(), path) == NULL){
        has_data_info_file = false;
        if(datainfo_path.substr(tmp_ind + 1) != "MnistDataset"){
            HIAI_ENGINE_LOG(HIAI_IDE_ERROR, "[SaveFilePostProcess_1] can not find %s!", postprocess_config_->info_file.c_str());
            return HIAI_ERROR;
        }
    }else{
        has_data_info_file = true;
        id_img_correlation = SetImgPredictionCorrelation(info_file_, "");
    }

    uint32_t graph_id = Engine::GetGraphId();
    std::shared_ptr<Graph> graph = Graph::GetInstance(graph_id);
    if (nullptr == graph)
    {
        HIAI_ENGINE_LOG(HIAI_IDE_ERROR, "Fail to get the graph");
        return HIAI_ERROR;
    }
    std::ostringstream deviceId;
    deviceId << graph->GetDeviceID();
    string device_dir = RESULT_FOLDER + "/" + deviceId.str();
    store_path = device_dir + "/" + ENGINE_NAME;
    if (HIAI_OK != CreateFolder(RESULT_FOLDER, PERMISSION)) {
        return HIAI_ERROR;
    }
    if (HIAI_OK != CreateFolder(device_dir, PERMISSION)) {
        return HIAI_ERROR;
    }
    if (HIAI_OK != CreateFolder(store_path, PERMISSION)) {
        return HIAI_ERROR;
    }
    HIAI_ENGINE_LOG(HIAI_IDE_INFO, "[SaveFilePostProcess_1] end init!");
    return HIAI_OK;
}

HIAI_IMPL_ENGINE_PROCESS("SaveFilePostProcess_1", SaveFilePostProcess_1, INPUT_SIZE)
{
    HIAI_ENGINE_LOG(HIAI_IDE_INFO, "[SaveFilePostProcess_1] start process!");
    if (nullptr == arg0)
    {
        HIAI_ENGINE_LOG(HIAI_IDE_ERROR, "fail to process invalid message");
        return HIAI_ERROR;
    }
    std::shared_ptr<EngineTransT> tran = std::static_pointer_cast<EngineTransT>(arg0);
    //add sentinel image for showing this data in dataset are all sended, this is last step.
    BatchImageParaWithScaleT image_handle = {tran->b_info, tran->v_img};
    if (isSentinelImage(std::make_shared<BatchImageParaWithScaleT>(image_handle)))
    {
        HIAI_ENGINE_LOG(HIAI_IDE_INFO, "[SaveFilePostProcess_1] sentinel image, process over.");
        std::shared_ptr<std::string> result_data(new std::string);
        HIAI_StatusT hiai_ret = HIAI_OK;
        do{
            hiai_ret = SendData(0, "string", std::static_pointer_cast<void>(result_data));
            if (HIAI_OK != hiai_ret) {
                if (HIAI_ENGINE_NULL_POINTER == hiai_ret || HIAI_HDC_SEND_MSG_ERROR == hiai_ret || HIAI_HDC_SEND_ERROR == hiai_ret
                        || HIAI_GRAPH_SRC_PORT_NOT_EXIST == hiai_ret || HIAI_GRAPH_ENGINE_NOT_EXIST == hiai_ret) {
                    HIAI_ENGINE_LOG(HIAI_IDE_ERROR, "[SaveFilePostProcess_1] SendData error[%d], break.", hiai_ret);
                    break;
                }
                HIAI_ENGINE_LOG(HIAI_IDE_INFO, "[SaveFilePostProcess_1] SendData return value[%d] not OK, sleep 200ms", hiai_ret);
                usleep(SEND_DATA_INTERVAL_MS);

            }
        } while (HIAI_OK != hiai_ret);
        return hiai_ret;
    }
    if (!tran->status)
    {
        HIAI_ENGINE_LOG(HIAI_IDE_ERROR, tran->msg.c_str());
        return HIAI_ERROR;
    }
    std::vector<OutputT> output_data_vec = tran->output_data_vec;
    std::vector<uint32_t> frame_ID = tran->b_info.frame_ID;
    for(unsigned int ind = 0; ind < tran->b_info.batch_size; ind++)
    {
        if ((int)frame_ID[ind] == -1) {
            HIAI_ENGINE_LOG(HIAI_IDE_INFO, "[ImageClassificationPostProcess] image number %d failed, add empty struct", ind);
            continue;
        }
        std::string prefix = "";
        if(has_data_info_file){
            ImageInfor img_infor = id_img_correlation[frame_ID[ind]];
            prefix = store_path  + "/" + img_infor.tfilename;
        }else{
            prefix = store_path  + "/" + std::to_string(frame_ID[ind]);
        }
        for(unsigned int i=0 ; i < output_data_vec.size(); ++i){
            OutputT out = output_data_vec[i];
            int size = out.size / sizeof(float);
            if(size <= 0){
                HIAI_ENGINE_LOG(HIAI_IDE_ERROR, "[SaveFilePostProcess_1]: the OutPutT size less than 0!");
                return HIAI_ERROR;
            }
            float* result = nullptr;
            try{
                result = new float[size];
            }catch (const std::bad_alloc& e) {
                HIAI_ENGINE_LOG(HIAI_IDE_ERROR, "[SaveFilePostProcess_1] alloc output data error!");
                return HIAI_ERROR;
            }
            int ret  = memcpy_s(result, sizeof(float)*size, out.data.get(), sizeof(float)*size);
            if(ret != 0){
                HIAI_ENGINE_LOG(HIAI_IDE_ERROR, "[SaveFilePostProcess_1] memcpy_s output data error!");
                delete[] result;
                result = NULL;
                return HIAI_ERROR;
            }
            std::string name(out.name);
            GetOutputName(name);
            std::string outFileName = prefix + "_" + name + ".jpeg";
            int fd = open(outFileName.c_str(), O_CREAT| O_WRONLY, FIlE_PERMISSION);
            if(fd == -1){
                HIAI_ENGINE_LOG(HIAI_IDE_ERROR, "[SaveFilePostProcess_1] open file %s error!", outFileName.c_str());
                delete[] result;
                result = NULL;
                return HIAI_ERROR;
            }

            cv::Mat data2;
            cv::Mat data(960,1440,CV_32F,result);
            //data = data * 255;

            // // seperate channels
            cv::Mat ch[3];
            cv::split(data,ch);

            data2 = ch[0];
            data2 = data2 * 255.0f;
            data2.convertTo(data,CV_8U);
            

            // data2 = data;
            // 
            // open input file and convert image to YcrCb
            cv::Mat ycc;
            std::string realImagePath = "/home/HwHiAiUser/HIAI_DATANDMODELSET/workspace_mind_studio/Final/final.png";
            cv::Mat rgb = cv::imread(realImagePath,3);
            
            // auto* cimg = cvLoadImage(realImagePath.c_str(),1);
            // rgb = cv::cvarrToMat(cimg);
            if(!rgb.data){
               HIAI_ENGINE_LOG(HIAI_IDE_ERROR, "[SaveFilePostProcess_1] open file %s error!", realImagePath.c_str()); 
               return HIAI_ERROR;
            }
            // Append CrCb to the data

            cv::cvtColor(rgb,ycc,cv::COLOR_BGR2YCrCb);
            
            // seperate channels
            cv::Mat channels[3];
            cv::split(ycc,channels);

            // interpolate and resize Cr and Cb
            cv::Mat updatedchannels[3];
            cv::Size updatedsize(1440,960);

            cv::resize(channels[0],updatedchannels[0],updatedsize);
            cv::resize(channels[1],updatedchannels[1],updatedsize);
            cv::resize(channels[2],updatedchannels[2],updatedsize);

            std::vector<cv::Mat> fin;

            fin.push_back(data);
            fin.push_back(updatedchannels[1]);
            fin.push_back(updatedchannels[2]);

//            cv::Size d1 = data2.size();
//            cv::Size d2 = updatedchannels[0].size();
//            cv::Size d3 = updatedchannels[1].size();

            // HIAI_ENGINE_LOG(HIAI_IDE_ERROR, "[SaveFilePostProcess_1] SIZE of data %d   %d ", d1.width,d1.height);
            // HIAI_ENGINE_LOG(HIAI_IDE_ERROR, "[SaveFilePostProcess_1] TYPE of Cr %d   %d ", data.type(),d2.height); 
            // HIAI_ENGINE_LOG(HIAI_IDE_ERROR, "[SaveFilePostProcess_1] SIZE of Cb %d   %d ", d3.width,d3.height); 

            cv::Mat finalimage,finalrgb;
            cv::merge(fin,finalimage);

            cv::cvtColor(finalimage,finalrgb,cv::COLOR_YCrCb2BGR);
            cv::imwrite(outFileName,finalrgb);
            

            // Write file
            //cv::imwrite(outFileName,data);
        }
    }
    HIAI_ENGINE_LOG(HIAI_IDE_INFO, "[SaveFilePostProcess_1] end process!");
    return HIAI_OK;
}
