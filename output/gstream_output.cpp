/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Copyright (C) 2020, Raspberry Pi (Trading) Ltd.
 *
 * gstream_output.cpp - send output over gstreamer.
 */

#include "gstream_output.hpp"

GStreamOutput::GStreamOutput(VideoOptions const *options) : Output(options)
{
	gst_init(nullptr, nullptr);
	GError *error = nullptr;
	pipeline = gst_parse_launch(options->output.c_str(), &error);
    if (!pipeline || error) {
        throw std::runtime_error("Failed to create GStreamer pipeline: " + std::string(error->message));
    }
	
    appsrc = gst_bin_get_by_name(GST_BIN(pipeline), "appsrc");
    if (!appsrc) {
        throw std::runtime_error("Pipeline does not contain an appsrc element");
    }

    gst_element_set_state(pipeline, GST_STATE_PLAYING);
}

GStreamOutput::~GStreamOutput()
{
	gst_element_set_state(pipeline, GST_STATE_NULL);
	if (pipeline) {
	    gst_object_unref(pipeline);
	}
	if (appsrc) {
	    gst_object_unref(appsrc);
	}
}

void GStreamOutput::outputBuffer(void *mem, size_t size, int64_t /*timestamp_us*/, uint32_t /*flags*/)
{
	LOG(2, "GStreamOutput: Start " << mem << " size " << size);
	
	//todo: gst_buffer_new_wrapped does not required to make a copy but have issues with deleting buffer
    //GstBuffer *gst_buffer = gst_buffer_new_wrapped((gpointer)mem, size);

	GstBuffer *gst_buffer = gst_buffer_new_allocate(nullptr, size, nullptr);
	GstMapInfo map;
	
	if (gst_buffer_map(gst_buffer, &map, GST_MAP_WRITE)) {
		memcpy(map.data, mem, size);
		gst_buffer_unmap(gst_buffer, &map);
		
		GstFlowReturn ret;
		g_signal_emit_by_name(appsrc, "push-buffer", gst_buffer, &ret);
        gst_buffer_unref(gst_buffer);
		if (ret != GST_FLOW_OK) {
			throw std::runtime_error("Failed to push buffer to GStreamer pipeline");
        }
	} else {
		gst_buffer_unref(gst_buffer);
		throw std::runtime_error("Failed to map GStreamer buffer for writing");
    }
	
	LOG(2, "GStreamOutput: Finish " << mem << " size " << size);
}
