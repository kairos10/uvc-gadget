/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * libcamera source
 *
 * Copyright (C) 2022 Ideas on Board Oy
 *
 * Contact: Daniel Scally <dan.scally@ideasonboard.com>
 */

#include <errno.h>
#include <fcntl.h>
#include <iostream>
#include <memory.h>
#include <queue>
#include <stdlib.h>
#include <string>
#include <string.h>
#include <unistd.h>

#include <libcamera/libcamera.h>
#include <linux/videodev2.h>

extern "C" {
#include "events.h"
#include "libcamera-source.h"
#include "tools.h"
#include "video-buffers.h"
}

using namespace libcamera;

#define to_libcamera_source(s) container_of(s, struct libcamera_source, src)

struct libcamera_source {
	struct video_source src;

	std::unique_ptr<CameraManager> cm;
	std::unique_ptr<CameraConfiguration> config;
	std::shared_ptr<Camera> camera;
	ControlList controls;

	FrameBufferAllocator *allocator;
	std::vector<std::unique_ptr<Request>> requests;
	std::queue<Request *> completed_requests;
	int pfds[2];

	struct video_buffer_set buffers;

	void requestComplete(Request *request);
};

void libcamera_source::requestComplete(Request *request)
{
	if (request->status() == Request::RequestCancelled)
		return;

	completed_requests.push(request);

	/*
	 * We want to hand off to the event loop to do any further processing,
	 * which we can achieve by simply writing to the end of the pipe since
	 * the loop is polling the other end. Once the event loop picks up this
	 * write it will run libcamera_source_video_process().
	 */
	write(pfds[1], "x", 1);
};

static void libcamera_source_video_process(void *d)
{
	struct libcamera_source *src = (struct libcamera_source *)d;
	struct video_buffer buffer;
	Request *request;

	if (src->completed_requests.empty())
		return;

	request = src->completed_requests.front();
	src->completed_requests.pop();

	/* We have only a single buffer per request, so just pick the first */
	FrameBuffer *framebuf = request->buffers().begin()->second;

	buffer.index = request->cookie();

	/* TODO: Correct this for formats libcamera treats as multiplanar */
	buffer.size = framebuf->planes()[0].length;
	buffer.mem = NULL;
	buffer.bytesused = framebuf->metadata().planes()[0].bytesused;
	buffer.timestamp.tv_usec = framebuf->metadata().timestamp;
	buffer.error = false;

	src->src.handler(src->src.handler_data, &src->src, &buffer);
}

static void libcamera_source_destroy(struct video_source *s)
{
	struct libcamera_source *src = to_libcamera_source(s);

	src->camera->requestCompleted.disconnect(src);

	/* Closing the event notification file descriptors */
	close(src->pfds[0]);
	close(src->pfds[1]);

	src->camera->release();
	src->camera.reset();
	src->cm->stop();
	delete src;
}

static int libcamera_source_set_format(struct video_source *s,
				       struct v4l2_pix_format *fmt)
{
	struct libcamera_source *src = to_libcamera_source(s);
	StreamConfiguration &streamConfig = src->config->at(0);

	streamConfig.size.width = fmt->width;
	streamConfig.size.height = fmt->height;
	streamConfig.pixelFormat = PixelFormat(fmt->pixelformat);

	src->config->validate();

	if (fmt->pixelformat != streamConfig.pixelFormat.fourcc())
		std::cerr << "Warning: set_format: Requested format unavailable" << std::endl;

	std::cout << "setting format to " << streamConfig.toString() << std::endl;

	/*
	 * No .configure() call at this stage, because we need to pick up the
	 * number of buffers to use later on so we'd need to call it then too.
	 */

	fmt->width = streamConfig.size.width;
	fmt->height = streamConfig.size.height;
	fmt->pixelformat = streamConfig.pixelFormat.fourcc();
	fmt->field = V4L2_FIELD_ANY;

	/* TODO: Can we use libcamera helpers to get image size / stride? */
	fmt->sizeimage = fmt->width * fmt->height * 2;

	return 0;
}

static int libcamera_source_set_frame_rate(struct video_source *s, unsigned int fps)
{
	struct libcamera_source *src = to_libcamera_source(s);
	int64_t frame_time = 1000000 / fps;

	src->controls.set(controls::FrameDurationLimits,
			  Span<const int64_t, 2>({ frame_time, frame_time }));

	return 0;
}

static int libcamera_source_alloc_buffers(struct video_source *s, unsigned int nbufs)
{
	struct libcamera_source *src = to_libcamera_source(s);
	StreamConfiguration &streamConfig = src->config->at(0);
	int ret;

	streamConfig.bufferCount = nbufs;
	ret = src->camera->configure(src->config.get());
	if (ret) {
		std::cerr << "failed to configure the camera" << std::endl;
		return ret;
	}

	Stream *stream = src->config->at(0).stream();
	FrameBufferAllocator *allocator;

	allocator = new FrameBufferAllocator(src->camera);

	ret = allocator->allocate(stream);
	if (ret < 0) {
		std::cerr << "failed to allocate buffers" << std::endl;
		return ret;
	}

	src->allocator = allocator;

	const std::vector<std::unique_ptr<FrameBuffer>> &buffers = allocator->buffers(stream);
	src->buffers.nbufs = buffers.size();

	src->buffers.buffers = (video_buffer *)calloc(src->buffers.nbufs, sizeof(*src->buffers.buffers));
	if (!src->buffers.buffers) {
		std::cerr << "failed to allocate buffers" << std::endl;
		return -ENOMEM;
	}

	for (unsigned int i = 0; i < buffers.size(); ++i) {
		src->buffers.buffers[i].index = i;
		src->buffers.buffers[i].dmabuf = -1;
	}

	return ret;
}

static int libcamera_source_export_buffers(struct video_source *s,
					   struct video_buffer_set **bufs)
{
	struct libcamera_source *src = to_libcamera_source(s);
	Stream *stream = src->config->at(0).stream();
	const std::vector<std::unique_ptr<FrameBuffer>> &buffers = src->allocator->buffers(stream);
	struct video_buffer_set *vid_buf_set;
	unsigned int i;

	for (i = 0; i < buffers.size(); i++) {
		const std::unique_ptr<FrameBuffer> &buffer = buffers[i];

		src->buffers.buffers[i].size = buffer->planes()[0].length;
		src->buffers.buffers[i].dmabuf = buffer->planes()[0].fd.get();
	}

	vid_buf_set = video_buffer_set_new(buffers.size());
	if (!vid_buf_set)
		return -ENOMEM;

	for (i = 0; i < src->buffers.nbufs; ++i) {
		struct video_buffer *buffer = &src->buffers.buffers[i];

		vid_buf_set->buffers[i].size = buffer->size;
		vid_buf_set->buffers[i].dmabuf = buffer->dmabuf;
	}

	*bufs = vid_buf_set;

	return 0;
}

static int libcamera_source_free_buffers(struct video_source *s)
{
	struct libcamera_source *src = to_libcamera_source(s);
	Stream *stream = src->config->at(0).stream();

	src->allocator->free(stream);
	delete src->allocator;
	free(src->buffers.buffers);

	return 0;
}

static int libcamera_source_stream_on(struct video_source *s)
{
	struct libcamera_source *src = to_libcamera_source(s);
	Stream *stream = src->config->at(0).stream();
	int ret;

	const std::vector<std::unique_ptr<FrameBuffer>> &buffers = src->allocator->buffers(stream);

	for (unsigned int i = 0; i < buffers.size(); ++i) {
		std::unique_ptr<Request> request = src->camera->createRequest(i);
		if (!request) {
			std::cerr << "failed to create request" << std::endl;
			return -ENOMEM;
		}

		const std::unique_ptr<FrameBuffer> &buffer = buffers[i];
		ret = request->addBuffer(stream, buffer.get());
		if (ret < 0) {
			std::cerr << "failed to set buffer for request" << std::endl;
			return ret;
		}

		src->requests.push_back(std::move(request));
	}

	ret = src->camera->start(&src->controls);
	if (ret) {
		std::cerr << "failed to start camera" << std::endl;
		return ret;
	}

	for (std::unique_ptr<Request> &request : src->requests) {
		ret = src->camera->queueRequest(request.get());
		if (ret) {
			std::cerr << "failed to queue request" << std::endl;
			src->camera->stop();
			return ret;
		}
	}

	/*
	 * Given our event handling code is designed for V4L2 file descriptors
	 * and lacks a way to trigger an event manually, we're using a pipe so
	 * that we can watch the read end and write to the other end when
	 * requestComplete() is ran.
	 */
	events_watch_fd(src->src.events, src->pfds[0], EVENT_READ,
			libcamera_source_video_process, src);

	return 0;
}

static int libcamera_source_stream_off(struct video_source *s)
{
	struct libcamera_source *src = to_libcamera_source(s);

	src->camera->stop();
	events_unwatch_fd(src->src.events, src->pfds[0], EVENT_READ);
	src->requests.clear();

	while (!src->completed_requests.empty())
		src->completed_requests.pop();

	return 0;
}

static int libcamera_source_queue_buffer(struct video_source *s,
					 struct video_buffer *buf)
{
	struct libcamera_source *src = to_libcamera_source(s);

	for (std::unique_ptr<Request> &r : src->requests) {
		if (r->cookie() == buf->index) {
			r->reuse(Request::ReuseBuffers);
			src->camera->queueRequest(r.get());

			break;
		}
	}

	return 0;
}

static const struct video_source_ops libcamera_source_ops = {
	.destroy = libcamera_source_destroy,
	.set_format = libcamera_source_set_format,
	.set_frame_rate = libcamera_source_set_frame_rate,
	.alloc_buffers = libcamera_source_alloc_buffers,
	.export_buffers = libcamera_source_export_buffers,
	.free_buffers = libcamera_source_free_buffers,
	.stream_on = libcamera_source_stream_on,
	.stream_off = libcamera_source_stream_off,
	.queue_buffer = libcamera_source_queue_buffer,
	.fill_buffer = NULL,
};

std::string cameraName(Camera *camera)
{
	const ControlList &props = camera->properties();
	std::string name;

	const auto &location = props.get(properties::Location);
	if (location) {
		switch (*location) {
		case properties::CameraLocationFront:
			name = "Internal Front Camera";
			break;
		case properties::CameraLocationBack:
			name = "Internal back camera";
			break;
		case properties::CameraLocationExternal:
			name = "External camera";
			const auto &model = props.get(properties::Model);
			if (model)
				name = *model;
			break;
		}
	}
	name += " (" + camera->id() + ")";

	return name;
}

struct video_source *libcamera_source_create(const char *devname)
{
	struct libcamera_source *src;
	int ret;

	if (!devname) {
		std::cerr << "No camera identifier was passed" << std::endl;
		return NULL;
	}

	src = new libcamera_source;

	/*
	 * Event handling in libuvcgadget currently depends on select(), but
	 * unlike a V4L2 devnode there's no file descriptor for completed
	 * libcamera Requests. We'll spoof the events using a pipe for now,
	 * but...
	 *
	 * TODO: Replace event handling with libevent
	 */

	ret = pipe2(src->pfds, O_NONBLOCK);
	if (ret) {
		std::cerr << "failed to create pipe" << std::endl;
		goto err_free_src;
	}

	src->src.ops = &libcamera_source_ops;
	src->src.type = VIDEO_SOURCE_DMABUF;
	src->cm = std::make_unique<CameraManager>();
	src->cm->start();

	if (src->cm->cameras().empty()) {
		std::cout << "No cameras were identified on the system" << std::endl;
		goto err_close_pipe;
	}

	/* TODO: make a separate way to list libcamera cameras */
	for (auto const &camera : src->cm->cameras())
		printf("- %s\n", cameraName(camera.get()).c_str());

	/*
	 * Camera selection is by ID or index. Camera ID's start with a slash.
	 * If the first character is a digit, assume we're indexing, otherwise
	 * treat it as an ID.
	 */
	if (std::isdigit(devname[0])) {
		unsigned long index = std::atoi(devname);

		if (index >= src->cm->cameras().size()) {
			std::cerr << "No camera at index " << index << std::endl;
			goto err_close_pipe;
		}

		src->camera = src->cm->cameras()[index];
	} else {
		src->camera = src->cm->get(std::string(devname));
		if (!src->camera) {
			std::cerr << "found no camera matching " << devname << std::endl;
			goto err_close_pipe;
		}
	}

	ret = src->camera->acquire();
	if (ret) {
		fprintf(stderr, "failed to acquire camera\n");
		goto err_close_pipe;
	}

	std::cout << "Using camera " << cameraName(src->camera.get()) << std::endl;

	src->config =
		src->camera->generateConfiguration( { StreamRole::VideoRecording });
	if (!src->config) {
		std::cerr << "failed to generate camera config" << std::endl;
		goto err_release_camera;
	}

	src->camera->requestCompleted.connect(src, &libcamera_source::requestComplete);

	{
		/*
		 * We enable AutoFocus by default if it's supported by the camera.
		 * Keep the infoMap scoped to calm the compiler worrying about
		 * jumping over the reference with the gotos.
		 */
		const ControlInfoMap &infoMap = src->camera->controls();
		if (infoMap.find(&controls::AfMode) != infoMap.end()) {
			std::cout << "Enabling continuous auto-focus" << std::endl;
			src->controls.set(controls::AfMode, controls::AfModeContinuous);
		}
	}

	return &src->src;

err_release_camera:
	src->camera->release();
err_close_pipe:
	close(src->pfds[0]);
	close(src->pfds[1]);
	src->cm->stop();
err_free_src:
	delete src;

	return NULL;
}

void libcamera_source_init(struct video_source *s, struct events *events)
{
	struct libcamera_source *src = to_libcamera_source(s);

	src->src.events = events;
}
