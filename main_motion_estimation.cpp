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

#include "NVXIO/Application.hpp"
#include "NVXIO/ConfigParser.hpp"
#include "NVXIO/FrameSource.hpp"
#include "NVXIO/Render.hpp"
#include "NVXIO/SyncTimer.hpp"
#include "NVXIO/Utility.hpp"

#include "iterative_motion_estimator.hpp"

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




//
// Process events
//

struct EventData
{
    EventData() : stop(false), pause(false) {}

    bool stop;
    bool pause;
};


static void keyboardEventCallback(void* eventData, vx_char key, vx_uint32, vx_uint32)
{
    EventData* data = static_cast<EventData*>(eventData);

    if (key == 27) // escape
    {
        data->stop = true;
    }
    else if (key == ' ') // space
    {
        data->pause = !data->pause;
    }
}

//
// Parse configuration file
//

static bool read(const std::string& configFile,
                 IterativeMotionEstimator::Params& params,
                 std::string& message)
{
    std::unique_ptr<nvxio::ConfigParser> parser(nvxio::createConfigParser());

    parser->addParameter("biasWeight", nvxio::OptionHandler::real(&params.biasWeight,
             nvxio::ranges::atLeast(0.0f)));
    parser->addParameter("mvDivFactor", nvxio::OptionHandler::integer(&params.mvDivFactor,
             nvxio::ranges::atLeast(0) & nvxio::ranges::atMost(16)));
    parser->addParameter("smoothnessFactor", nvxio::OptionHandler::real(&params.smoothnessFactor,
             nvxio::ranges::atLeast(0.0f)));

    message = parser->parse(configFile);

    return message.empty();
}

//
// main - Application entry point
//

int motion_estimation(int argc, char** argv, int i)
{
    try
    {
        nvxio::Application &app = nvxio::Application::get();

        //
        // Parse command line arguments
        //

        std::string sourceUri = app.findSampleFilePath("pedestrians.mp4");
        std::string configFile = app.findSampleFilePath("motion_estimation_demo_config.ini");

        app.setDescription("This sample demonstrates Iterative Motion Estimation algorithm");
        app.addOption('s', "source", "Source URI", nvxio::OptionHandler::string(&sourceUri));
        app.addOption('c', "config", "Config file path", nvxio::OptionHandler::string(&configFile));
        app.init(argc, argv);

        //
        // Reads and checks input parameters
        //

        IterativeMotionEstimator::Params params;
        std::string error;
        if (!read(configFile, params, error))
        {
            std::cout << error;
            return nvxio::Application::APP_EXIT_CODE_INVALID_VALUE;
        }

        //
        // Create OpenVX context
        //

        nvxio::ContextGuard context;
        vxDirective(context, VX_DIRECTIVE_ENABLE_PERFORMANCE);

	//vxSetImmediateModeTarget(context, NVX_TARGET_CPU, NULL);

        //
        // Messages generated by the OpenVX framework will be processed by nvxio::stdoutLogCallback
        //

        vxRegisterLogCallback(context, &nvxio::stdoutLogCallback, vx_false_e);

        //
        // Create a Frame Source
        //

        std::unique_ptr<nvxio::FrameSource> frameSource(nvxio::createDefaultFrameSource(context, sourceUri));

        if (!frameSource || !frameSource->open())
        {
            std::cerr << "Error: cannot open frame source!" << std::endl;
            return nvxio::Application::APP_EXIT_CODE_NO_RESOURCE;
        }

        if (frameSource->getSourceType() == nvxio::FrameSource::SINGLE_IMAGE_SOURCE)
        {
            std::cerr << "Can't work on a single image." << std::endl;
            return nvxio::Application::APP_EXIT_CODE_INVALID_FORMAT;
        }

        nvxio::FrameSource::Parameters frameConfig = frameSource->getConfiguration();

        //
        // Create a Render
        //

        std::unique_ptr<nvxio::Render> render = nvxio::createDefaultRender(context, "Motion Estimation Demo",
                                                                           frameConfig.frameWidth, frameConfig.frameHeight);

        if (!render)
        {
            std::cerr << "Error: Cannot create render!" << std::endl;
            return nvxio::Application::APP_EXIT_CODE_NO_RENDER;
        }

        EventData eventData;
        render->setOnKeyboardEventCallback(keyboardEventCallback, &eventData);

        //
        // Create OpenVX Image to hold frames from video source
        //

        vx_image frameExemplar = vxCreateImage(context,
            frameConfig.frameWidth, frameConfig.frameHeight, VX_DF_IMAGE_RGBX);
        NVXIO_CHECK_REFERENCE(frameExemplar);
        vx_delay frame_delay = vxCreateDelay(context, (vx_reference)frameExemplar, 2);
        NVXIO_CHECK_REFERENCE(frame_delay);
        vxReleaseImage(&frameExemplar);

        vx_image prevFrame = (vx_image)vxGetReferenceFromDelay(frame_delay, -1);
        vx_image currFrame = (vx_image)vxGetReferenceFromDelay(frame_delay, 0);

        //
        // Create algorithm
        //

        IterativeMotionEstimator ime(context);

        nvxio::FrameSource::FrameStatus frameStatus;
        do
        {
            frameStatus = frameSource->fetch(prevFrame);
        } while (frameStatus == nvxio::FrameSource::TIMEOUT);
        if (frameStatus == nvxio::FrameSource::CLOSED)
        {
            std::cerr << "Source has no frames" << std::endl;
            return nvxio::Application::APP_EXIT_CODE_NO_FRAMESOURCE;
        }

        ime.init(prevFrame, currFrame, params);

        //
        // Main loop
        //

        std::unique_ptr<nvxio::SyncTimer> syncTimer = nvxio::createSyncTimer();
        syncTimer->arm(1. / app.getFPSLimit());

        nvx::Timer totalTimer;
        totalTimer.tic();
        double proc_ms = 0;

	 struct timespec ts_start, ts_end;
        double elapsedTime;

        while (!eventData.stop)
        {
	//FIXME /*release*/
         progress[i]=0;
         //printf("task %d is on GPU\n", *onGPU);
        #ifdef sched
        cpuSched(i);
        #endif

        clock_gettime(CLOCK_MONOTONIC, &ts_start);//releas

            if (!eventData.pause)
            {
                //
                // Grab next frame
                //

                frameStatus = frameSource->fetch(currFrame);

                if (frameStatus == nvxio::FrameSource::TIMEOUT)
                    continue;

                if (frameStatus == nvxio::FrameSource::CLOSED)
                {
                    if (!frameSource->open())
                    {
                        std::cerr << "Failed to reopen the source" << std::endl;
                        break;
                    }

                    do
                    {
                        frameStatus = frameSource->fetch(prevFrame);
                    } while (frameStatus == nvxio::FrameSource::TIMEOUT);
                    if (frameStatus == nvxio::FrameSource::CLOSED)
                    {
                        std::cerr << "Source has no frames" << std::endl;
                        return nvxio::Application::APP_EXIT_CODE_NO_FRAMESOURCE;
                    }

                    ime.init(prevFrame, currFrame, params);

                    continue;
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

                ime.process();

                proc_ms = procTimer.toc();

     gpuUnlock(i);

        //FIXME /*CPU resume*/
        progress[i]=2;
#ifdef sched
        cpuSched(i);
#endif




            }

            double total_ms = totalTimer.toc();

            std::cout << "Display Time : " << total_ms << " ms" << std::endl << std::endl;

            syncTimer->synchronize();

            total_ms = totalTimer.toc();

            totalTimer.tic();

            //
            // Show performance statistics
            //

            if (!eventData.pause)
            {
                ime.printPerfs();
            }


cpuResume(i);
#ifdef sched
cpuSched(i);
#endif

/*GPU part*/
//printf("child process %d lock\n", getpid());
gpuLock(i);

            //
            // Render
            //

            render->putImage(prevFrame);

            nvxio::Render::MotionFieldStyle mfStyle = {
                {  0u, 255u, 255u, 255u} // color
            };

            render->putMotionField(ime.getMotionField(), mfStyle);

            std::ostringstream msg;
            msg << std::fixed << std::setprecision(1);

            msg << "Resolution: " << frameConfig.frameWidth << 'x' << frameConfig.frameHeight << std::endl;
            msg << "Algorithm: " << proc_ms << " ms / " << 1000.0 / proc_ms << " FPS" << std::endl;
            msg << "Display: " << total_ms  << " ms / " << 1000.0 / total_ms << " FPS" << std::endl;
            msg << "Space - pause/resume" << std::endl;
            msg << "Esc - close the sample";

            nvxio::Render::TextBoxStyle textStyle = {
                {255u, 255u, 255u, 255u}, // color
                {0u,   0u,   0u, 127u}, // bgcolor
                {10u, 10u} // origin
            };

            render->putTextViewport(msg.str(), textStyle);

            if (!render->flush())
            {
                eventData.stop = true;
            }

            if (!eventData.pause)
            {
                vxAgeDelay(frame_delay);
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

        vxReleaseDelay(&frame_delay);
    }
    catch (const std::exception& e)
    {
        std::cerr << "Error: " << e.what() << std::endl;
        return nvxio::Application::APP_EXIT_CODE_ERROR;
    }

    return nvxio::Application::APP_EXIT_CODE_SUCCESS;
}
