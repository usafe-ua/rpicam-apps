/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Copyright (C) 2020, Raspberry Pi (Trading) Ltd.
 *
 * egl_preview.cpp - X/EGL-based preview window.
 */

#include <map>
#include <string>

// Include libcamera stuff before X11, as X11 #defines both Status and None
// which upsets the libcamera headers.

#include "core/options.hpp"

#include "preview.hpp"

#include <libdrm/drm_fourcc.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
// We don't use Status below, so we could consider #undefining it here.
// We do use None, so if we had to #undefine it we could replace it by zero
// in what follows below.

#include <epoxy/egl.h>
#include <epoxy/gl.h>

class EglPreview : public Preview
{
public:
	EglPreview(Options const *options);
	~EglPreview();
	virtual void SetInfoText(const std::string &text) override;
	// Display the buffer. You get given the fd back in the BufferDoneCallback
	// once its available for re-use.
	virtual void Show(int fd, libcamera::Span<uint8_t> span, StreamInfo const &info) override;
	// Reset the preview window, clearing the current buffers and being ready to
	// show new ones.
	virtual void Reset() override;
	// Check if the window manager has closed the preview.
	virtual bool Quit() override;
	// Return the maximum image size allowed.
	virtual void MaxImageSize(unsigned int &w, unsigned int &h) const override
	{
		w = max_image_width_;
		h = max_image_height_;
	}
    virtual void SetOverlay(uint8_t* buf, int width, int height) override;

private:
	struct Buffer
	{
		Buffer() : fd(-1) {}
		int fd;
		size_t size;
		StreamInfo info;
		GLuint texture;
	};
	void makeWindow(char const *name);
	void makeBuffer(int fd, size_t size, StreamInfo const &info, Buffer &buffer);
    void gl_setup(int width, int height, int window_width, int window_height);
    void setup_overlay();

	::Display *display_;
	EGLDisplay egl_display_;
	Window window_;
	EGLContext egl_context_;
	EGLSurface egl_surface_;
	std::map<int, Buffer> buffers_; // map the DMABUF's fd to the Buffer
	int last_fd_;
	bool first_time_;
	Atom wm_delete_window_;
	// size of preview window
	int x_;
	int y_;
	int width_;
	int height_;
	unsigned int max_image_width_;
	unsigned int max_image_height_;
    GLint prog_;
    float verts_[8];
    GLint overlay_prog_;
    bool overlay_present_ = false;
    GLuint overlay_texture_;
};

static GLint compile_shader(GLenum target, const char *source)
{
	GLuint s = glCreateShader(target);
	glShaderSource(s, 1, (const GLchar **)&source, NULL);
	glCompileShader(s);

	GLint ok;
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);

	if (!ok)
	{
		GLchar *info;
		GLint size;

		glGetShaderiv(s, GL_INFO_LOG_LENGTH, &size);
		info = (GLchar *)malloc(size);

		glGetShaderInfoLog(s, size, NULL, info);
		throw std::runtime_error("failed to compile shader: " + std::string(info) + "\nsource:\n" +
								 std::string(source));
	}

	return s;
}

static GLint link_program(GLint vs, GLint fs)
{
	GLint prog = glCreateProgram();
	glAttachShader(prog, vs);
	glAttachShader(prog, fs);
	glLinkProgram(prog);

	GLint ok;
	glGetProgramiv(prog, GL_LINK_STATUS, &ok);
	if (!ok)
	{
		/* Some drivers return a size of 1 for an empty log.  This is the size
		 * of a log that contains only a terminating NUL character.
		 */
		GLint size;
		GLchar *info = NULL;
		glGetProgramiv(prog, GL_INFO_LOG_LENGTH, &size);
		if (size > 1)
		{
			info = (GLchar *)malloc(size);
			glGetProgramInfoLog(prog, size, NULL, info);
		}

		throw std::runtime_error("failed to link: " + std::string(info ? info : "<empty log>"));
	}

	return prog;
}

void EglPreview::setup_overlay()
{
    const char* vs = R""""(
             attribute vec2 aPosition;
             varying vec2 texcoord;

             void main()
             {
                 gl_Position = vec4(aPosition * 2.0 - 1.0, 0.0, 1.0);
                 texcoord.x = aPosition.x;
                 texcoord.y = 1.0 - aPosition.y;
             })"""";
    GLint vs_s = compile_shader(GL_VERTEX_SHADER, vs);
    const char *fs = R""""(
            precision mediump float;
            varying vec2 texcoord;
            uniform sampler2D overlay;

            void main()
            {
                gl_FragColor = texture2D(overlay, texcoord);
            }
            )"""";
    GLint fs_s = compile_shader(GL_FRAGMENT_SHADER, fs);
    overlay_prog_ = link_program(vs_s, fs_s);

    static const float verts[] = {         0.0, 0.0,
                                           1.0, 0.0,
                                           1.0, 1.0,
                                           0.0, 1.0  };

    GLint inputAttrib = glGetAttribLocation(overlay_prog_, "aPosition");
    glVertexAttribPointer(inputAttrib, 2, GL_FLOAT, GL_FALSE, 0, verts);
    glEnableVertexAttribArray(inputAttrib);
//    glDisableVertexAttribArray(inputAttrib);

    glUseProgram(overlay_prog_);
    glUniform1i(glGetUniformLocation(overlay_prog_, "overlay"), 0);

    glGenTextures(1, &overlay_texture_);

    glBindTexture(GL_TEXTURE_2D, overlay_texture_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    //allocate memory on the graphics card for the texture. It's fine if
    //texture_data doesn't have any data in it, the texture will just appear black
    //until you update it.

    int overlay_width = width_ /4;
    int overlay_height = height_/4;

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, overlay_width, overlay_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
}

void EglPreview::gl_setup(int width, int height, int window_width, int window_height)
{
	float w_factor = width / (float)window_width;
	float h_factor = height / (float)window_height;
	float max_dimension = std::max(w_factor, h_factor);
	w_factor /= max_dimension;
	h_factor /= max_dimension;
	char vs[256];
	snprintf(vs, sizeof(vs),
			 "attribute vec4 pos;\n"
			 "varying vec2 texcoord;\n"
			 "\n"
			 "void main() {\n"
			 "  gl_Position = pos;\n"
			 "  texcoord.x = pos.x / %f + 0.5;\n"
			 "  texcoord.y = 0.5 - pos.y / %f;\n"
			 "}\n",
			 2.0 * w_factor, 2.0 * h_factor);
	vs[sizeof(vs) - 1] = 0;
	GLint vs_s = compile_shader(GL_VERTEX_SHADER, vs);
	const char *fs = "#extension GL_OES_EGL_image_external : enable\n"
					 "precision mediump float;\n"
					 "uniform samplerExternalOES s;\n"
					 "varying vec2 texcoord;\n"
					 "void main() {\n"
					 "  gl_FragColor = texture2D(s, texcoord);\n"
					 "}\n";
	GLint fs_s = compile_shader(GL_FRAGMENT_SHADER, fs);
    prog_ = link_program(vs_s, fs_s);

    glUseProgram(prog_);

    const static float verts[] = { -w_factor, -h_factor, w_factor, -h_factor, w_factor, h_factor, -w_factor, h_factor };
    std::copy(verts, verts+sizeof(verts)/sizeof(verts[0]), verts_);

    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, verts_);
    glEnableVertexAttribArray(0);

    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable( GL_BLEND );
}

EglPreview::EglPreview(Options const *options) : Preview(options), last_fd_(-1), first_time_(true)
{
	display_ = XOpenDisplay(NULL);
	if (!display_)
		throw std::runtime_error("Couldn't open X display");

	egl_display_ = eglGetDisplay(display_);
	if (!egl_display_)
		throw std::runtime_error("eglGetDisplay() failed");

	EGLint egl_major, egl_minor;

	if (!eglInitialize(egl_display_, &egl_major, &egl_minor))
		throw std::runtime_error("eglInitialize() failed");

	x_ = options_->preview_x;
	y_ = options_->preview_y;
	width_ = options_->preview_width;
	height_ = options_->preview_height;
	makeWindow("rpicam-app");

	// gl_setup() has to happen later, once we're sure we're in the display thread.
}

EglPreview::~EglPreview()
{
	EglPreview::Reset();
	eglDestroyContext(egl_display_, egl_context_);
    glDeleteTextures(1, &overlay_texture_);
}

static void no_border(Display *display, Window window)
{
	static const unsigned MWM_HINTS_DECORATIONS = (1 << 1);
	static const int PROP_MOTIF_WM_HINTS_ELEMENTS = 5;

	typedef struct
	{
		unsigned long flags;
		unsigned long functions;
		unsigned long decorations;
		long inputMode;
		unsigned long status;
	} PropMotifWmHints;

	PropMotifWmHints motif_hints;
	Atom prop, proptype;
	unsigned long flags = 0;

	/* setup the property */
	motif_hints.flags = MWM_HINTS_DECORATIONS;
	motif_hints.decorations = flags;

	/* get the atom for the property */
	prop = XInternAtom(display, "_MOTIF_WM_HINTS", True);
	if (!prop)
	{
		/* something went wrong! */
		return;
	}

	/* not sure this is correct, seems to work, XA_WM_HINTS didn't work */
	proptype = prop;

	XChangeProperty(display, window, /* display, window */
					prop, proptype, /* property, type */
					32, /* format: 32-bit datums */
					PropModeReplace, /* mode */
					(unsigned char *)&motif_hints, /* data */
					PROP_MOTIF_WM_HINTS_ELEMENTS /* nelements */
	);


    Atom atoms[2] = { XInternAtom(display, "_NET_WM_STATE_FULLSCREEN", False), None };

    XChangeProperty(
        display, window,
        XInternAtom(display, "_NET_WM_STATE", False),
        XA_ATOM, 32, PropModeReplace, (unsigned char*)atoms, 1);
}

void EglPreview::makeWindow(char const *name)
{
	int screen_num = DefaultScreen(display_);
	XSetWindowAttributes attr;
	unsigned long mask;
	Window root = RootWindow(display_, screen_num);
	int screen_width = DisplayWidth(display_, screen_num);
	int screen_height = DisplayHeight(display_, screen_num);

	// Default behaviour here is to use a 1024x768 window.
	if (width_ == 0 || height_ == 0)
	{
		width_ = 1024;
		height_ = 768;
	}

	if (options_->fullscreen || x_ + width_ > screen_width || y_ + height_ > screen_height)
	{
		x_ = y_ = 0;
		width_ = DisplayWidth(display_, screen_num);
		height_ = DisplayHeight(display_, screen_num);
        attr.cursor  = 0;//remove cursor
	}

	static const EGLint attribs[] =
		{
			EGL_RED_SIZE, 1,
			EGL_GREEN_SIZE, 1,
			EGL_BLUE_SIZE, 1,
            EGL_ALPHA_SIZE, 0,
			EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
			EGL_NONE
		};
	EGLConfig config;
	EGLint num_configs;
	if (!eglChooseConfig(egl_display_, attribs, &config, 1, &num_configs))
		throw std::runtime_error("couldn't get an EGL visual config");

	EGLint vid;
	if (!eglGetConfigAttrib(egl_display_, config, EGL_NATIVE_VISUAL_ID, &vid))
		throw std::runtime_error("eglGetConfigAttrib() failed\n");

	XVisualInfo visTemplate = {};
	visTemplate.visualid = (VisualID)vid;
	int num_visuals;
	XVisualInfo *visinfo = XGetVisualInfo(display_, VisualIDMask, &visTemplate, &num_visuals);

	/* window attributes */
	attr.background_pixel = 0;
	attr.border_pixel = 0;
	attr.colormap = XCreateColormap(display_, root, visinfo->visual, AllocNone);
	attr.event_mask = StructureNotifyMask | ExposureMask | KeyPressMask;

	/* XXX this is a bad way to get a borderless window! */
	mask = CWBackPixel | CWBorderPixel | CWColormap | CWEventMask;

	window_ = XCreateWindow(display_, root, x_, y_, width_, height_, 0, visinfo->depth, InputOutput, visinfo->visual,
							mask, &attr);

	if (options_->fullscreen)
		no_border(display_, window_);

	/* set hints and properties */
	{
		XSizeHints sizehints;
		sizehints.x = x_;
		sizehints.y = y_;
		sizehints.width = width_;
		sizehints.height = height_;
		sizehints.flags = USSize | USPosition;
		XSetNormalHints(display_, window_, &sizehints);
		XSetStandardProperties(display_, window_, name, name, None, (char **)NULL, 0, &sizehints);
	}

	eglBindAPI(EGL_OPENGL_ES_API);

	static const EGLint ctx_attribs[] = {
		EGL_CONTEXT_CLIENT_VERSION, 2,
		EGL_NONE
	};
	egl_context_ = eglCreateContext(egl_display_, config, EGL_NO_CONTEXT, ctx_attribs);
	if (!egl_context_)
		throw std::runtime_error("eglCreateContext failed");

	XFree(visinfo);

	XMapWindow(display_, window_);

	// This stops the window manager from closing the window, so we get an event instead.
	wm_delete_window_ = XInternAtom(display_, "WM_DELETE_WINDOW", False);
	XSetWMProtocols(display_, window_, &wm_delete_window_, 1);

	egl_surface_ = eglCreateWindowSurface(egl_display_, config, reinterpret_cast<EGLNativeWindowType>(window_), NULL);
	if (!egl_surface_)
		throw std::runtime_error("eglCreateWindowSurface failed");

	// We have to do eglMakeCurrent in the thread where it will run, but we must do it
	// here temporarily so as to get the maximum texture size.
	eglMakeCurrent(egl_display_, EGL_NO_SURFACE, EGL_NO_SURFACE, egl_context_);
	int max_texture_size = 0;
	glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_texture_size);
	max_image_width_ = max_image_height_ = max_texture_size;
	// This "undoes" the previous eglMakeCurrent.
	eglMakeCurrent(egl_display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
}

static void get_colour_space_info(std::optional<libcamera::ColorSpace> const &cs, EGLint &encoding, EGLint &range)
{
	encoding = EGL_ITU_REC601_EXT;
	range = EGL_YUV_NARROW_RANGE_EXT;

	if (cs == libcamera::ColorSpace::Sycc)
		range = EGL_YUV_FULL_RANGE_EXT;
	else if (cs == libcamera::ColorSpace::Smpte170m)
		/* all good */;
	else if (cs == libcamera::ColorSpace::Rec709)
		encoding = EGL_ITU_REC709_EXT;
	else
		LOG(1, "EglPreview: unexpected colour space " << libcamera::ColorSpace::toString(cs));
}

void EglPreview::makeBuffer(int fd, size_t size, StreamInfo const &info, Buffer &buffer)
{
	if (first_time_)
	{
		// This stuff has to be delayed until we know we're in the thread doing the display.
		if (!eglMakeCurrent(egl_display_, egl_surface_, egl_surface_, egl_context_))
			throw std::runtime_error("eglMakeCurrent failed");
        gl_setup(info.width, info.height, width_, height_);
        setup_overlay();
		first_time_ = false;
	}

	buffer.fd = fd;
	buffer.size = size;
	buffer.info = info;

	EGLint encoding, range;
	get_colour_space_info(info.colour_space, encoding, range);

	EGLint attribs[] = {
		EGL_WIDTH, static_cast<EGLint>(info.width),
		EGL_HEIGHT, static_cast<EGLint>(info.height),
		EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_YUV420,
		EGL_DMA_BUF_PLANE0_FD_EXT, fd,
		EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
		EGL_DMA_BUF_PLANE0_PITCH_EXT, static_cast<EGLint>(info.stride),
		EGL_DMA_BUF_PLANE1_FD_EXT, fd,
		EGL_DMA_BUF_PLANE1_OFFSET_EXT, static_cast<EGLint>(info.stride * info.height),
		EGL_DMA_BUF_PLANE1_PITCH_EXT, static_cast<EGLint>(info.stride / 2),
		EGL_DMA_BUF_PLANE2_FD_EXT, fd,
		EGL_DMA_BUF_PLANE2_OFFSET_EXT, static_cast<EGLint>(info.stride * info.height + (info.stride / 2) * (info.height / 2)),
		EGL_DMA_BUF_PLANE2_PITCH_EXT, static_cast<EGLint>(info.stride / 2),
		EGL_YUV_COLOR_SPACE_HINT_EXT, encoding,
		EGL_SAMPLE_RANGE_HINT_EXT, range,
		EGL_NONE
	};

	EGLImage image = eglCreateImageKHR(egl_display_, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, attribs);
	if (!image)
		throw std::runtime_error("failed to import fd " + std::to_string(fd));

	glGenTextures(1, &buffer.texture);
	glBindTexture(GL_TEXTURE_EXTERNAL_OES, buffer.texture);
	glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, image);

	eglDestroyImageKHR(egl_display_, image);
}

void EglPreview::SetInfoText(const std::string &text)
{
	if (!text.empty())
		XStoreName(display_, window_, text.c_str());
}

void EglPreview::SetOverlay(uint8_t* buf, int width, int height){
    if (!buf){
        overlay_present_ = false;
        return;
    }

    eglMakeCurrent(egl_display_, egl_surface_, egl_surface_, egl_context_);


    glBindTexture(GL_TEXTURE_2D, overlay_texture_);
//    glPixelStorei(GL_UNPACK_ROW_LENGTH, quadTextureSize);
//            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, buf);
//            glPixelStorei (GL_UNPACK_ROW_LENGTH, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, buf);
    overlay_present_ = true;

    glBindTexture( GL_TEXTURE_2D, 0);
}

void EglPreview::Show(int fd, libcamera::Span<uint8_t> span, StreamInfo const &info)
{
	Buffer &buffer = buffers_[fd];
	if (buffer.fd == -1)
		makeBuffer(fd, span.size(), info, buffer);

    glUseProgram(prog_);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, verts_);
    glEnableVertexAttribArray(0);

	glClearColor(0, 0, 0, 0);
	glClear(GL_COLOR_BUFFER_BIT);

    glEnableVertexAttribArray(0);

	glBindTexture(GL_TEXTURE_EXTERNAL_OES, buffer.texture);
	glDrawArrays(GL_TRIANGLE_FAN, 0, 4);

    if (overlay_present_){
        glUseProgram(overlay_prog_);
        glBindTexture(GL_TEXTURE_2D, overlay_texture_);

        static const float verts[] = {         0.0, 0.0,
                                               1.0, 0.0,
                                               1.0, 1.0,
                                               0.0, 1.0  };

        GLint inputAttrib = glGetAttribLocation(overlay_prog_, "aPosition");
        glVertexAttribPointer(inputAttrib, 2, GL_FLOAT, GL_FALSE, 0, verts);
        glEnableVertexAttribArray(inputAttrib);
    //    glDisableVertexAttribArray(inputAttrib);

//        glUseProgram(overlay_prog_);
//        glUniform1i(glGetUniformLocation(overlay_prog_, "overlay"), 0);

//        glGenTextures(1, &overlay_texture_);

//        glBindTexture(GL_TEXTURE_2D, overlay_texture_);
//        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
//        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
//        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
//        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

//    //    glUseProgram(prog_);

//        //allocate memory on the graphics card for the texture. It's fine if
//        //texture_data doesn't have any data in it, the texture will just appear black
//        //until you update it.
//        int overlay_width = width_ / 4;
//        int overlay_height = height_ / 4;
////        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, overlay_width, overlay_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
          glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
    }



	EGLBoolean success [[maybe_unused]] = eglSwapBuffers(egl_display_, egl_surface_);
	if (last_fd_ >= 0)
		done_callback_(last_fd_);
	last_fd_ = fd;
}

void EglPreview::Reset()
{
	for (auto &it : buffers_)
		glDeleteTextures(1, &it.second.texture);
	buffers_.clear();
	last_fd_ = -1;
	eglMakeCurrent(egl_display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	first_time_ = true;
}

bool EglPreview::Quit()
{
	XEvent event;
	while (XCheckTypedWindowEvent(display_, window_, ClientMessage, &event))
	{
		if (static_cast<Atom>(event.xclient.data.l[0]) == wm_delete_window_)
			return true;
	}
	return false;
}

Preview *make_egl_preview(Options const *options)
{
	return new EglPreview(options);
}
