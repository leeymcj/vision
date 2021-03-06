/*
# Copyright (c) 2014-2016, NVIDIA CORPORATION. All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions
# are met:
#  * Redistributions of source code must retain the above copyright
#    notice, this list of conditions and the following disclaimer.
#  * Redistributions in binary form must reproduce the above copyright
#    notice, this list of conditions and the following disclaimer in the
#    documentation and/or other materials provided with the distribution.
#  * Neither the name of NVIDIA CORPORATION nor the names of its
#    contributors may be used to endorse or promote products derived
#    from this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
# EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
# IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
# PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
# CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
# EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
# PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
# PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
# OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <iostream>
#include <sstream>
#include <iomanip>
#include <string>
#include <memory>

#include <NVX/nvx.h>
#include <NVX/nvx_timer.hpp>

#include <NVXIO/Application.hpp>
#include <NVXIO/FrameSource.hpp>
#include <NVXIO/Render.hpp>
#include <NVXIO/SyncTimer.hpp>
#include <NVXIO/Utility.hpp>

#include "stabilizer.hpp"



#include <sys/resource.h>
#include <unistd.h>

#define sched

extern int *progress;
extern int *T;
extern void gpuLock(int i);
extern void gpuUnlock(int i);
extern void cpuSched(int i);
extern void cpuResume(int i);
extern void jobRelease(int i);

struct EventData
{
    EventData(): shouldStop(false), pause(false) {}
    bool shouldStop;
    bool pause;
};

static void eventCallback(void* eventData, vx_char key, vx_uint32, vx_uint32)
{
    EventData* data = static_cast<EventData*>(eventData);

    if (key == 27)
    {
        data->shouldStop = true;
    }
    else if (key == 32)
    {
        data->pause = !data->pause;
    }
}

static void displayState(nvxio::Render *renderer,
                         const nvxio::FrameSource::Parameters &sourceParams,
                         double proc_ms, double total_ms, float cropMargin)
{
    vx_uint32 renderWidth = renderer->getViewportWidth();

    std::ostringstream txt;
    txt << std::fixed << std::setprecision(1);

    const vx_int32 borderSize = 10;
    nvxio::Render::TextBoxStyle style = {{255, 255, 255, 255}, {0, 0, 0, 127}, {renderWidth / 2 + borderSize, borderSize}};

    txt << "Source size: " << sourceParams.frameWidth << 'x' << sourceParams.frameHeight << std::endl;
    txt << "Algorithm: " << proc_ms << " ms / " << 1000.0 / proc_ms << " FPS" << std::endl;
    txt << "Display: " << total_ms  << " ms / " << 1000.0 / total_ms << " FPS" << std::endl;

    txt << std::setprecision(6);
    txt.unsetf(std::ios_base::floatfield);
    txt << "LIMITED TO " << nvxio::Application::get().getFPSLimit() << " FPS FOR DISPLAY" << std::endl;
    txt << "Space - pause/resume" << std::endl;
    txt << "Esc - close the demo";
    renderer->putTextViewport(txt.str(), style);

    const vx_int32 stabilizedLabelLenght = 100;
    style.origin.x = renderWidth - stabilizedLabelLenght;
    style.origin.y = borderSize;
    renderer->putTextViewport("stabilized", style);

    style.origin.x = renderWidth / 2 - stabilizedLabelLenght;
    renderer->putTextViewport("original", style);

    if (cropMargin > 0)
    {
        vx_uint32 dx = static_cast<vx_uint32>(cropMargin * sourceParams.frameWidth);
        vx_uint32 dy = static_cast<vx_uint32>(cropMargin * sourceParams.frameHeight);
        vx_rectangle_t rect = {dx, dy, sourceParams.frameWidth - dx, sourceParams.frameHeight - dy};

        nvxio::Render::DetectedObjectStyle rectStyle = {{""}, {255, 255, 255, 255}, 2, 0, false};
        renderer->putObjectLocation(rect, rectStyle);
    }
}

//
// main - Application entry point
//

int video_stabilizer(int argc, char* argv[], int i)
{
    try
    {
        nvxio::Application &app = nvxio::Application::get();

        //
        // Parse command line arguments
        //

        std::string videoFilePath = app.findSampleFilePath("parking.avi");
        unsigned numOfSmoothingFrames = 5;
        float cropMargin = 0.07f;

        app.setDescription("This demo demonstrates Video Stabilization algorithm");
        app.addOption('s', "source", "Input URI", nvxio::OptionHandler::string(&videoFilePath));
        app.addOption('n', "", "Number of smoothing frames",
                      nvxio::OptionHandler::unsignedInteger(&numOfSmoothingFrames, nvxio::ranges::atLeast(1u) & nvxio::ranges::atMost(6u)));
        app.addOption(0, "crop", "Crop margin for stabilized frames. If it is negative then the frame cropping is turned off",
                      nvxio::OptionHandler::real(&cropMargin, nvxio::ranges::lessThan(0.5f)));
        app.init(argc, argv);

        //
        // Create OpenVX context
        //

        nvxio::ContextGuard context;
        vxRegisterLogCallback(context, &nvxio::stdoutLogCallback, vx_false_e);
        vxDirective(context, VX_DIRECTIVE_ENABLE_PERFORMANCE);

	//vxSetImmediateModeTarget(context, NVX_TARGET_CPU, NULL);

        //
        // Create FrameSource and Render
        //

        std::unique_ptr<nvxio::FrameSource> source(
            nvxio::createDefaultFrameSource(context, videoFilePath));

        if (!source || !source->open())
        {
            std::cerr << "Error: Can't open source file: " << videoFilePath << std::endl;
            return nvxio::Application::APP_EXIT_CODE_NO_RESOURCE;
        }

        if (source->getSourceType() == nvxio::FrameSource::SINGLE_IMAGE_SOURCE)
        {
            std::cerr << "Error: Can't work on a single image." << std::endl;
            return nvxio::Application::APP_EXIT_CODE_INVALID_FORMAT;
        }

        nvxio::FrameSource::Parameters sourceParams = source->getConfiguration();

        vx_int32 demoImgWidth = 2 * sourceParams.frameWidth;
        vx_int32 demoImgHeight = sourceParams.frameHeight;

        std::unique_ptr<nvxio::Render> renderer(nvxio::createDefaultRender(
            context, "Video Stabilization Demo", demoImgWidth, demoImgHeight));

        if (!renderer)
        {
            std::cerr << "Error: Can't create a renderer" << std::endl;
            return nvxio::Application::APP_EXIT_CODE_NO_RENDER;
        }

        EventData eventData;
        renderer->setOnKeyboardEventCallback(eventCallback, &eventData);

        //
        // Create OpenVX Image to hold frames from video source
        //

        vx_image demoImg = vxCreateImage(context, demoImgWidth,
                                       demoImgHeight, VX_DF_IMAGE_RGBX);
        NVXIO_CHECK_REFERENCE(demoImg);

        vx_image frameExemplar = vxCreateImage(context,
                                               sourceParams.frameWidth, sourceParams.frameHeight, VX_DF_IMAGE_RGBX);
        vx_size orig_frame_delay_size = numOfSmoothingFrames + 2; //must have such size to be synchronized with the stabilized frames
        vx_delay orig_frame_delay = vxCreateDelay(context, (vx_reference)frameExemplar, orig_frame_delay_size);
        NVXIO_CHECK_REFERENCE(orig_frame_delay);
        NVXIO_SAFE_CALL( nvx::initDelayOfImages(context, orig_frame_delay) );
        NVXIO_SAFE_CALL(vxReleaseImage(&frameExemplar));

        vx_image frame = (vx_image)vxGetReferenceFromDelay(orig_frame_delay, 0);
        vx_image lastFrame = (vx_image)vxGetReferenceFromDelay(orig_frame_delay,
                                                               1 - static_cast<vx_int32>(orig_frame_delay_size));

        //
        // Create VideoStabilizer instance
        //

        nvx::VideoStabilizer::VideoStabilizerParams params;
        params.numOfSmoothingFrames_ = numOfSmoothingFrames;
        params.cropMargin_ = cropMargin;
        std::unique_ptr<nvx::VideoStabilizer> stabilizer(nvx::VideoStabilizer::createImageBasedVStab(context, params));

        nvxio::FrameSource::FrameStatus frameStatus;

        do
        {
            frameStatus = source->fetch(frame);
        } while (frameStatus == nvxio::FrameSource::TIMEOUT);

        if (frameStatus == nvxio::FrameSource::CLOSED)
        {
            std::cerr << "Error: Source has no frames" << std::endl;
            return nvxio::Application::APP_EXIT_CODE_NO_FRAMESOURCE;
        }

        stabilizer->init(frame);

        vx_rectangle_t leftRect;
        NVXIO_SAFE_CALL( vxGetValidRegionImage(frame, &leftRect) );

        vx_rectangle_t rightRect;
        rightRect.start_x = leftRect.end_x;
        rightRect.start_y = leftRect.start_y;
        rightRect.end_x = 2 * leftRect.end_x;
        rightRect.end_y = leftRect.end_y;

        vx_image leftRoi = vxCreateImageFromROI(demoImg, &leftRect);
        NVXIO_CHECK_REFERENCE(leftRoi);
        vx_image rightRoi = vxCreateImageFromROI(demoImg, &rightRect);
        NVXIO_CHECK_REFERENCE(rightRoi);

        //
        // Run processing loop
        //

        std::unique_ptr<nvxio::SyncTimer> syncTimer = nvxio::createSyncTimer();
        syncTimer->arm(1. / app.getFPSLimit());

        nvx::Timer totalTimer;
        totalTimer.tic();
        double proc_ms = 0;


struct timespec ts_start, ts_end;
double elapsedTime;

        while (!eventData.shouldStop)
        {

jobRelease(i);

//FIXME /*release*/
progress[i]=0;
         //printf("task %d is on GPU\n", *onGPU);
#ifdef sched
cpuSched(i);
#endif

clock_gettime(CLOCK_MONOTONIC, &ts_start);//release

            if (!eventData.pause)
            {

//FIXME /*CPU completion*
cpuResume(i);

#ifdef sched
cpuSched(i);
#endif

gpuLock(i);

		 //
                // Read frame
                //

                frameStatus = source->fetch(frame);

                if (frameStatus == nvxio::FrameSource::TIMEOUT)
                    continue;
                else if (frameStatus == nvxio::FrameSource::CLOSED)
                {
                    if (!source->open())
                    {
                        std::cerr << "Error: Failed to reopen the source" << std::endl;
                        break;
                    }
                }

//FIXME /*CPU completion*
cpuResume(i);

#ifdef sched
cpuSched(i);
#endif

gpuLock(i);

                //
                // Process
                //

                nvx::Timer procTimer;
                procTimer.tic();
                stabilizer->process(frame);
                proc_ms = procTimer.toc();

                NVXIO_SAFE_CALL( vxAgeDelay(orig_frame_delay) );

                vx_image stabImg = stabilizer->getStabilizedFrame();
                NVXIO_SAFE_CALL( nvxuCopyImage(context, stabImg, rightRoi) );
                NVXIO_SAFE_CALL( nvxuCopyImage(context, lastFrame, leftRoi) );

gpuUnlock(i);

//FIXME /*CPU resume*/
progress[i]=2;
#ifdef sched
cpuSched(i);
#endif

                //
                // Print performance results
                //

                stabilizer->printPerfs();


cpuResume(i);
#ifdef sched
cpuSched(i);
#endif
/*GPU part*/
//printf("child process %d lock\n", getpid());
gpuLock(i);               
      


      }

            renderer->putImage(demoImg);

            double total_ms = totalTimer.toc();

            std::cout << "Display Time : " << total_ms << " ms" << std::endl << std::endl;

            syncTimer->synchronize();

            total_ms = totalTimer.toc();
            totalTimer.tic();

            displayState(renderer.get(), sourceParams, proc_ms, total_ms, cropMargin);

            if (!renderer->flush())
            {
                eventData.shouldStop = true;
            }




gpuUnlock(i);

 //FIXME CPU resume
progress[i]=4;

#ifdef sched
cpuSched(i);
#endif


//FIXME CPU completion
cpuResume(i);
#ifdef sched
cpuSched(i);
#endif
clock_gettime(CLOCK_MONOTONIC, &ts_end);
elapsedTime = (ts_end.tv_sec - ts_start.tv_sec) * 1000.0;      // sec to ms
elapsedTime += (ts_end.tv_nsec - ts_start.tv_nsec) / 1000000.0;   // us to ms
printf("task %d completion time %lf\n", i, elapsedTime);




sleep(T[i]-elapsedTime/1000 );




        }

        //
        // Release all objects
        //

        renderer->close();

        vxReleaseImage(&demoImg);
        vxReleaseImage(&leftRoi);
        vxReleaseImage(&rightRoi);
        vxReleaseDelay(&orig_frame_delay);
    }
    catch (const std::exception& e)
    {
        std::cerr << "Error: " << e.what() << std::endl;
        return nvxio::Application::APP_EXIT_CODE_ERROR;
    }

    return nvxio::Application::APP_EXIT_CODE_SUCCESS;
}
