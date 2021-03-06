// DaNI_Kinect2_Tracker.cpp : Defines the entry point for the console application.
//

#include "stdafx.h"
#include <iostream>
#include "Kinect2_tools.h"
#include "util.h"
#include <thread>
#include <chrono>
#include "ppl.h"

Kinect::Kinect()
{
	initialize();
}

Kinect::~Kinect()
{
	finalize();
}

void Kinect::run()
{
	while (true)
	{
		update();

		draw();

		show();

		const int key = cv::waitKey(10);
		if (key == VK_ESCAPE) break;
	}
		
}

void Kinect::initialize()
{
	cv::setUseOptimized(true);

	initializeSensor();

	initializeColor();

	initializeBody();

	std::this_thread::sleep_for(std::chrono::seconds(2));
}
inline void Kinect::initializeSensor()
{
	ERROR_CHECK(GetDefaultKinectSensor(&kinect));

	ERROR_CHECK(kinect->Open());

	BOOLEAN isOpen = FALSE;

	ERROR_CHECK(kinect->get_IsOpen(&isOpen));
	if (!isOpen) {
		throw std::runtime_error("failed IKinectSensor::get_IsOpen( &isOpen)");
	}

	ERROR_CHECK(kinect->get_CoordinateMapper(&coordinateMapper));
}

inline void Kinect::initializeColor()
{
	//open the reader
	ComPtr<IColorFrameSource> colorFrameSource;
	ERROR_CHECK(kinect->get_ColorFrameSource(&colorFrameSource));
	ERROR_CHECK(colorFrameSource->OpenReader(&colorFrameReader));

	//Retrieve the frame description(resolution,etc)
	ComPtr<IFrameDescription> colorFrameDescription;
	ERROR_CHECK(colorFrameSource->CreateFrameDescription(ColorImageFormat::ColorImageFormat_Bgra, &colorFrameDescription));
	ERROR_CHECK(colorFrameDescription->get_Width(&colorWidth));
	ERROR_CHECK(colorFrameDescription->get_Height(&colorHeight));
	ERROR_CHECK(colorFrameDescription->get_BytesPerPixel(&colorBytesPerPixel));

	colorBuffer.resize(colorHeight * colorWidth * colorBytesPerPixel);
}

inline void Kinect::initializeBody()
{	
	//Open Body Reader
	ComPtr<IBodyFrameSource> bodyFrameSource;
	ERROR_CHECK(kinect->get_BodyFrameSource(&bodyFrameSource));
	ERROR_CHECK(bodyFrameSource->OpenReader(&bodyFrameReader));

	//Initialise Body Buffer
	Concurrency::parallel_for_each(bodies.begin(), bodies.end(), [](IBody*& body)
	{
		safeRelease(body);
	});

	//Color Table for Visualisations:
	colors[0] = cv::Vec3b(255, 0, 0); //blue
	colors[1] = cv::Vec3b(0, 255, 0); //green
	colors[2] = cv::Vec3b(0, 0, 255); //red
	colors[3] = cv::Vec3b(255, 255, 0); //cyan
	colors[4] = cv::Vec3b(255, 0, 255); //magenta
	colors[5] = cv::Vec3b(0, 255, 255); //yellow
}

void Kinect::finalize()
{
	cv::destroyAllWindows();

	//Release body buffer
	Concurrency::parallel_for_each(bodies.begin(), bodies.end(), [](IBody* &body)
	{
		safeRelease(body);
	});	

	if (kinect != nullptr)
		kinect->Close();
}

void Kinect::update()
{
	updateColor();	
	updateBody();
}

inline void Kinect::updateColor() 
{
	ComPtr<IColorFrame> colorFrame;
	const HRESULT ret = colorFrameReader->AcquireLatestFrame(&colorFrame);
	if (FAILED(ret)) return;

	//convert format
	ERROR_CHECK(colorFrame->CopyConvertedFrameDataToArray(static_cast<UINT>(colorBuffer.size()), &colorBuffer[0],
		ColorImageFormat::ColorImageFormat_Bgra));
}

inline void Kinect::updateBody()
{
	ComPtr<IBodyFrame> bodyFrame;
	const HRESULT ret = bodyFrameReader->AcquireLatestFrame(&bodyFrame);
	if (FAILED(ret)) return;

	//release previous bodies
	Concurrency::parallel_for_each(bodies.begin(), bodies.end(), [](IBody* &body)
	{
		safeRelease(body);
	});

	//retrieve data
	ERROR_CHECK(bodyFrame->GetAndRefreshBodyData(static_cast<UINT>(bodies.size()), &bodies[0]));
}

void Kinect::draw()
{
	drawColor();
	drawBody();
}

inline void Kinect::drawColor()
{
	colorMat = cv::Mat(colorHeight, colorWidth, CV_8UC4, &colorBuffer[0]);

}

inline void Kinect::drawBody()
{
	Concurrency::parallel_for(0, BODY_COUNT, [&](const int count) {
		const ComPtr<IBody> body = bodies[count];
		if (body == nullptr) return;

		BOOLEAN tracked = FALSE;
		ERROR_CHECK(body->get_IsTracked(&tracked));
		if (!tracked) return;
		std::array<Joint, JointType::JointType_Count> joints;
		ERROR_CHECK(body->GetJoints(static_cast<UINT>(joints.size()), &joints[0]));
		Concurrency::parallel_for_each(joints.begin(), joints.end(), [&](const Joint& joint) {
			if (joint.TrackingState == TrackingState::TrackingState_NotTracked) return;
			
			//draw joint positions
			drawEllipse(colorMat, joint, 5, colors[count]);

			//draw left hand
			if (joint.JointType == JointType::JointType_HandLeft) {
				HandState handState;
				TrackingConfidence handConfidence;
				ERROR_CHECK(body->get_HandLeftState(&handState));
				ERROR_CHECK(body->get_HandLeftConfidence(&handConfidence));

				drawHandState(colorMat, joint, handState, handConfidence);
			}

			//draw right hand
			if (joint.JointType == JointType::JointType_HandRight) {
				HandState handState;
				TrackingConfidence handConfidence;
				ERROR_CHECK(body->get_HandRightState(&handState));
				ERROR_CHECK(body->get_HandRightConfidence(&handConfidence));

				drawHandState(colorMat, joint, handState, handConfidence);
			}
		});
	});
}

inline void Kinect::drawEllipse(cv::Mat &image, const Joint &joint, const int radius, const cv::Vec3b &color, const int thickness)
{
	if (image.empty()) return;

	ColorSpacePoint colorSpacePoint;
	ERROR_CHECK(coordinateMapper->MapCameraPointToColorSpace(joint.Position, &colorSpacePoint));
	const int x = static_cast<int>(colorSpacePoint.X + 0.5f);
	const int y = static_cast<int>(colorSpacePoint.Y + 0.5f);

	if ((x >= 0) && (x < image.cols) && (y >= 0) && (y < image.rows)) {
		cv::circle(image, cv::Point(x, y), radius, static_cast<cv::Scalar>(color), thickness, cv::LINE_AA);
	}
}

inline void Kinect::drawHandState(cv::Mat &image, const Joint &joint, HandState handState, TrackingConfidence handConfidence)
{
	if (image.empty()) return;

	if (handConfidence != TrackingConfidence::TrackingConfidence_High) return;

	const int radius = 75;
	const cv::Vec3b blue = cv::Vec3b(128, 0, 0), green = cv::Vec3b(0, 128, 0), red = cv::Vec3b(0, 0, 128);
	
	switch (handState) {
	case HandState::HandState_Open:
		drawEllipse(image, joint, radius, green, 5);
		break;
	case HandState::HandState_Closed:
		drawEllipse(image, joint, radius, red, 5);
		break;
	case HandState::HandState_Lasso:
		drawEllipse(image, joint, radius, blue, 5);
		break;
	default:
		break;
	}
}

void Kinect::show() 
{
	showBody();
}

inline void Kinect::showBody()
{
	if (colorMat.empty()) return;

	cv::Mat resizeMat;
	const double scale = 0.5;
	cv::resize(colorMat, resizeMat, cv::Size(), scale, scale);

	cv::imshow("Body", resizeMat);
}
