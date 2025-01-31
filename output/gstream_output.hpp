/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Copyright (C) 2020, Raspberry Pi (Trading) Ltd.
 *
 * gstream_output.hpp - send output over gstreamer.
 */

#pragma once

#include <gst/gst.h>
#include "output.hpp"

class GStreamOutput : public Output
{
public:
	GStreamOutput(VideoOptions const *options);
	~GStreamOutput();

protected:
	void outputBuffer(void *mem, size_t size, int64_t timestamp_us, uint32_t flags) override;

private:
    GstElement *pipeline;
    GstElement *appsrc;
};
