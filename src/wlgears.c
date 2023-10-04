/*
 * Copyright (C) 1999-2001  Brian Paul	All Rights Reserved.
 * Copyright © 2011 Benjamin Franzke
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * BRIAN PAUL BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN
 * AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/*
 * Ported to GLES2.
 * Kristian Høgsberg <krh@bitplanet.net>
 * May 3, 2010
 *
 * Improve GLES2 port:
 *	* Refactor gear drawing.
 *	* Use correct normals for surfaces.
 *	* Improve shader.
 *	* Use perspective projection transformation.
 *	* Add FPS count.
 *	* Add comments.
 * Alexandros Frantzis <alexandros.frantzis@linaro.org>
 * Jul 13, 2010
 */

/*
 * Merged with weston simple egl to make wayland xdg port
 * Scott Moreau <oreaus@gmail.com>
 * Jul 19, 2021
 */

#define GL_GLEXT_PROTOTYPES
#define EGL_EGLEXT_PROTOTYPES

#define _GNU_SOURCE

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <math.h>
#include <assert.h>
#include <signal.h>

#include <linux/input.h>

#include <wayland-client.h>
#include <wayland-egl.h>
#include <wayland-cursor.h>

#include <epoxy/gl.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>

#include "xdg-shell-client-protocol.h"
#include <sys/types.h>
#include <unistd.h>

#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#ifndef ARRAY_LENGTH
#define ARRAY_LENGTH(a) (sizeof (a) / sizeof (a)[0])
#endif

#define MIN(X, Y) (((X) < (Y)) ? (X) : (Y))

struct window;
struct seat;

struct display {
	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_compositor *compositor;
	struct xdg_wm_base *wm_base;
	struct wl_seat *seat;
	struct wl_pointer *pointer;
	struct wl_touch *touch;
	struct wl_keyboard *keyboard;
	struct wl_shm *shm;
	struct wl_cursor_theme *cursor_theme;
	struct wl_cursor *default_cursor;
	struct wl_surface *cursor_surface;
	struct {
		EGLDisplay dpy;
		EGLContext ctx;
		EGLConfig conf;
	} egl;
	struct window *window;

	PFNEGLSWAPBUFFERSWITHDAMAGEEXTPROC swap_buffers_with_damage;
};

struct geometry {
	int width, height;
};

struct window {
	struct display *display;
	struct geometry geometry, window_size;
	struct {
		GLuint rotation_uniform;
		GLuint pos;
		GLuint col;
	} gl;

	uint32_t benchmark_time, frames;
	struct wl_egl_window *native;
	struct wl_surface *surface;
	struct xdg_surface *xdg_surface;
	struct xdg_toplevel *xdg_toplevel;
	EGLSurface egl_surface;
	struct wl_callback *callback;
	int fullscreen, maximized, opaque, buffer_size, frame_sync, delay;
	bool wait_for_configure;
};

static int running = 1;

#define STRIPS_PER_TOOTH 7
#define VERTICES_PER_TOOTH 46
#define GEAR_VERTEX_STRIDE 6

/* Each vertex consist of GEAR_VERTEX_STRIDE GLfloat attributes */
typedef GLfloat GearVertex[GEAR_VERTEX_STRIDE];

/**
 * Struct representing a gear.
 */
struct gear {
	/** The array of vertices comprising the gear */
	GearVertex *vertices;
	/** The number of vertices comprising the gear */
	int nvertices;
	/** The Vertex Buffer Object holding the vertices in the graphics card */
	GLuint vbo;
};

/** The view rotation [x, y, z] */
static GLfloat view_rot[3] = { 20.0, 30.0, 0.0 };
/** The gears */
static struct gear *gear1, *gear2, *gear3;
/** The current gear rotation angle */
static GLfloat angle = 0.0;
/** The location of the shader uniforms */
static GLuint ModelViewProjectionMatrix_location,
		NormalMatrix_location,
		LightSourcePosition_location,
		MaterialColor_location;
/** The projection matrix */
static GLfloat ProjectionMatrix[16];
/** The direction of the directional light for the scene */
static const GLfloat LightSourcePosition[4] = { 5.0, 5.0, 10.0, 1.0};

/**
 * Fills a gear vertex.
 *
 * @param v the vertex to fill
 * @param x the x coordinate
 * @param y the y coordinate
 * @param z the z coortinate
 * @param n pointer to the normal table
 *
 * @return the operation error code
 */
static GearVertex *
vert(GearVertex *v, GLfloat x, GLfloat y, GLfloat z, GLfloat n[3])
{
	v[0][0] = x;
	v[0][1] = y;
	v[0][2] = z;
	v[0][3] = n[0];
	v[0][4] = n[1];
	v[0][5] = n[2];

	return v + 1;
}

/**
 *  Create a gear wheel.
 *
 *  @param inner_radius radius of hole at center
 *  @param outer_radius radius at center of teeth
 *  @param width width of gear
 *  @param teeth number of teeth
 *  @param tooth_depth depth of tooth
 *
 *  @return pointer to the constructed struct gear
 */
static struct gear *
create_gear(GLfloat inner_radius, GLfloat outer_radius, GLfloat width,
		GLint teeth, GLfloat tooth_depth)
{
	GLfloat r0, r1, r2;
	GLfloat da;
	GearVertex *v;
	struct gear *gear;
	double s[5], c[5];
	GLfloat normal[3];
	int cur_strip_start = 0;
	int i;

	/* Allocate memory for the gear */
	gear = malloc(sizeof *gear);
	if (gear == NULL)
		return NULL;

	/* Calculate the radii used in the gear */
	r0 = inner_radius;
	r1 = outer_radius - tooth_depth / 2.0;
	r2 = outer_radius + tooth_depth / 2.0;

	da = 2.0 * M_PI / teeth / 4.0;

	/* the first tooth doesn't need the first strip-restart sequence */
	assert(teeth > 0);
	gear->nvertices = VERTICES_PER_TOOTH + (VERTICES_PER_TOOTH + 2) * (teeth - 1);

	/* Allocate memory for the vertices */
	gear->vertices = calloc(gear->nvertices, sizeof(*gear->vertices));
	v = gear->vertices;

	for (i = 0; i < teeth; i++) {
		/* Calculate needed sin/cos for varius angles */
		sincos(i * 2.0 * M_PI / teeth, &s[0], &c[0]);
		sincos(i * 2.0 * M_PI / teeth + da, &s[1], &c[1]);
		sincos(i * 2.0 * M_PI / teeth + da * 2, &s[2], &c[2]);
		sincos(i * 2.0 * M_PI / teeth + da * 3, &s[3], &c[3]);
		sincos(i * 2.0 * M_PI / teeth + da * 4, &s[4], &c[4]);

		/* A set of macros for making the creation of the gears easier */
#define  GEAR_POINT(r, da) { (r) * c[(da)], (r) * s[(da)] }
#define  SET_NORMAL(x, y, z) do { \
	normal[0] = (x); normal[1] = (y); normal[2] = (z); \
} while(0)

#define  GEAR_VERT(v, point, sign) vert((v), p[(point)].x, p[(point)].y, (sign) * width * 0.5, normal)

#define START_STRIP do { \
	cur_strip_start = (v - gear->vertices); \
	if (cur_strip_start) \
		v += 2; \
} while(0);

/* emit prev last vertex
	emit first vertex */
#define END_STRIP do { \
	if (cur_strip_start) { \
		memcpy(gear->vertices + cur_strip_start, \
				 gear->vertices + (cur_strip_start - 1), sizeof(GearVertex)); \
		memcpy(gear->vertices + cur_strip_start + 1, \
				 gear->vertices + (cur_strip_start + 2), sizeof(GearVertex)); \
	} \
} while (0)

#define QUAD_WITH_NORMAL(p1, p2) do { \
	SET_NORMAL((p[(p1)].y - p[(p2)].y), -(p[(p1)].x - p[(p2)].x), 0); \
	v = GEAR_VERT(v, (p1), -1); \
	v = GEAR_VERT(v, (p1), 1); \
	v = GEAR_VERT(v, (p2), -1); \
	v = GEAR_VERT(v, (p2), 1); \
} while(0)

		struct point {
			GLfloat x;
			GLfloat y;
		};

		/* Create the 7 points (only x,y coords) used to draw a tooth */
		struct point p[7] = {
			GEAR_POINT(r2, 1), // 0
			GEAR_POINT(r2, 2), // 1
			GEAR_POINT(r1, 0), // 2
			GEAR_POINT(r1, 3), // 3
			GEAR_POINT(r0, 0), // 4
			GEAR_POINT(r1, 4), // 5
			GEAR_POINT(r0, 4), // 6
		};

		/* Front face */
		START_STRIP;
		SET_NORMAL(0, 0, 1.0);
		v = GEAR_VERT(v, 0, +1);
		v = GEAR_VERT(v, 1, +1);
		v = GEAR_VERT(v, 2, +1);
		v = GEAR_VERT(v, 3, +1);
		v = GEAR_VERT(v, 4, +1);
		v = GEAR_VERT(v, 5, +1);
		v = GEAR_VERT(v, 6, +1);
		END_STRIP;

		/* Back face */
		START_STRIP;
		SET_NORMAL(0, 0, -1.0);
		v = GEAR_VERT(v, 0, -1);
		v = GEAR_VERT(v, 1, -1);
		v = GEAR_VERT(v, 2, -1);
		v = GEAR_VERT(v, 3, -1);
		v = GEAR_VERT(v, 4, -1);
		v = GEAR_VERT(v, 5, -1);
		v = GEAR_VERT(v, 6, -1);
		END_STRIP;

		/* Outer face */
		START_STRIP;
		QUAD_WITH_NORMAL(0, 2);
		END_STRIP;

		START_STRIP;
		QUAD_WITH_NORMAL(1, 0);
		END_STRIP;

		START_STRIP;
		QUAD_WITH_NORMAL(3, 1);
		END_STRIP;

		START_STRIP;
		QUAD_WITH_NORMAL(5, 3);
		END_STRIP;

		/* Inner face */
		START_STRIP;
		SET_NORMAL(-c[0], -s[0], 0);
		v = GEAR_VERT(v, 4, -1);
		v = GEAR_VERT(v, 4, 1);
		SET_NORMAL(-c[4], -s[4], 0);
		v = GEAR_VERT(v, 6, -1);
		v = GEAR_VERT(v, 6, 1);
		END_STRIP;
	}

	assert(gear->nvertices == (v - gear->vertices));

	/* Store the vertices in a vertex buffer object (VBO) */
	glGenBuffers(1, &gear->vbo);
	glBindBuffer(GL_ARRAY_BUFFER, gear->vbo);
	glBufferData(GL_ARRAY_BUFFER, gear->nvertices * sizeof(GearVertex),
			gear->vertices, GL_STATIC_DRAW);

	return gear;
}

/**
 * Multiplies two 4x4 matrices.
 *
 * The result is stored in matrix m.
 *
 * @param m the first matrix to multiply
 * @param n the second matrix to multiply
 */
static void
multiply(GLfloat *m, const GLfloat *n)
{
	GLfloat tmp[16];
	const GLfloat *row, *column;
	div_t d;
	int i, j;

	for (i = 0; i < 16; i++) {
		tmp[i] = 0;
		d = div(i, 4);
		row = n + d.quot * 4;
		column = m + d.rem;
		for (j = 0; j < 4; j++)
			tmp[i] += row[j] * column[j * 4];
	}
	memcpy(m, &tmp, sizeof tmp);
}

/**
 * Rotates a 4x4 matrix.
 *
 * @param[in,out] m the matrix to rotate
 * @param angle the angle to rotate
 * @param x the x component of the direction to rotate to
 * @param y the y component of the direction to rotate to
 * @param z the z component of the direction to rotate to
 */
static void
rotate(GLfloat *m, GLfloat angle, GLfloat x, GLfloat y, GLfloat z)
{
	double s, c;

	sincos(angle, &s, &c);
	GLfloat r[16] = {
		x * x * (1 - c) + c,	  y * x * (1 - c) + z * s, x * z * (1 - c) - y * s, 0,
		x * y * (1 - c) - z * s, y * y * (1 - c) + c,	  y * z * (1 - c) + x * s, 0,
		x * z * (1 - c) + y * s, y * z * (1 - c) - x * s, z * z * (1 - c) + c,	  0,
		0, 0, 0, 1
	};

	multiply(m, r);
}


/**
 * Translates a 4x4 matrix.
 *
 * @param[in,out] m the matrix to translate
 * @param x the x component of the direction to translate to
 * @param y the y component of the direction to translate to
 * @param z the z component of the direction to translate to
 */
static void
translate(GLfloat *m, GLfloat x, GLfloat y, GLfloat z)
{
	GLfloat t[16] = { 1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  x, y, z, 1 };

	multiply(m, t);
}

/**
 * Creates an identity 4x4 matrix.
 *
 * @param m the matrix make an identity matrix
 */
static void
identity(GLfloat *m)
{
	GLfloat t[16] = {
		1.0, 0.0, 0.0, 0.0,
		0.0, 1.0, 0.0, 0.0,
		0.0, 0.0, 1.0, 0.0,
		0.0, 0.0, 0.0, 1.0,
	};

	memcpy(m, t, sizeof(t));
}

/**
 * Transposes a 4x4 matrix.
 *
 * @param m the matrix to transpose
 */
static void
transpose(GLfloat *m)
{
	GLfloat t[16] = {
		m[0], m[4], m[8],  m[12],
		m[1], m[5], m[9],  m[13],
		m[2], m[6], m[10], m[14],
		m[3], m[7], m[11], m[15]};

	memcpy(m, t, sizeof(t));
}

/**
 * Inverts a 4x4 matrix.
 *
 * This function can currently handle only pure translation-rotation matrices.
 * Read http://www.gamedev.net/community/forums/topic.asp?topic_id=425118
 * for an explanation.
 */
static void
invert(GLfloat *m)
{
	GLfloat t[16];
	identity(t);

	// Extract and invert the translation part 't'. The inverse of a
	// translation matrix can be calculated by negating the translation
	// coordinates.
	t[12] = -m[12]; t[13] = -m[13]; t[14] = -m[14];

	// Invert the rotation part 'r'. The inverse of a rotation matrix is
	// equal to its transpose.
	m[12] = m[13] = m[14] = 0;
	transpose(m);

	// inv(m) = inv(r) * inv(t)
	multiply(m, t);
}

/**
 * Calculate a frustum projection transformation.
 *
 * @param m the matrix to save the transformation in
 * @param l the left plane distance
 * @param r the right plane distance
 * @param b the bottom plane distance
 * @param t the top plane distance
 * @param n the near plane distance
 * @param f the far plane distance
 */
static void
frustum(GLfloat *m, GLfloat l, GLfloat r, GLfloat b, GLfloat t, GLfloat n, GLfloat f)
{
	GLfloat tmp[16];
	identity(tmp);

	GLfloat deltaX = r - l;
	GLfloat deltaY = t - b;
	GLfloat deltaZ = f - n;

	tmp[0] = (2 * n) / deltaX;
	tmp[5] = (2 * n) / deltaY;
	tmp[8] = (r + l) / deltaX;
	tmp[9] = (t + b) / deltaY;
	tmp[10] = -(f + n) / deltaZ;
	tmp[11] = -1;
	tmp[14] = -(2 * f * n) / deltaZ;
	tmp[15] = 0;

	memcpy(m, tmp, sizeof(tmp));
}

/**
 * Draws a gear.
 *
 * @param gear the gear to draw
 * @param transform the current transformation matrix
 * @param x the x position to draw the gear at
 * @param y the y position to draw the gear at
 * @param angle the rotation angle of the gear
 * @param color the color of the gear
 */
static void
draw_gear(struct gear *gear, GLfloat *transform,
		GLfloat x, GLfloat y, GLfloat angle, const GLfloat color[4])
{
	GLfloat model_view[16];
	GLfloat normal_matrix[16];
	GLfloat model_view_projection[16];

	/* Translate and rotate the gear */
	memcpy(model_view, transform, sizeof (model_view));
	translate(model_view, x, y, 0);
	rotate(model_view, 2 * M_PI * angle / 360.0, 0, 0, 1);

	/* Create and set the ModelViewProjectionMatrix */
	memcpy(model_view_projection, ProjectionMatrix, sizeof(model_view_projection));
	multiply(model_view_projection, model_view);

	glUniformMatrix4fv(ModelViewProjectionMatrix_location, 1, GL_FALSE,
							 model_view_projection);

	/*
	 * Create and set the NormalMatrix. It's the inverse transpose of the
	 * ModelView matrix.
	 */
	memcpy(normal_matrix, model_view, sizeof (normal_matrix));
	invert(normal_matrix);
	transpose(normal_matrix);
	glUniformMatrix4fv(NormalMatrix_location, 1, GL_FALSE, normal_matrix);

	/* Set the gear color */
	glUniform4fv(MaterialColor_location, 1, color);

	/* Set the vertex buffer object to use */
	glBindBuffer(GL_ARRAY_BUFFER, gear->vbo);

	/* Set up the position of the attributes in the vertex buffer object */
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE,
			6 * sizeof(GLfloat), NULL);
	glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE,
			6 * sizeof(GLfloat), (GLfloat *) 0 + 3);

	/* Enable the attributes */
	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);

	/* Draw the triangle strips that comprise the gear */
	glDrawArrays(GL_TRIANGLE_STRIP, 0, gear->nvertices);

	/* Disable the attributes */
	glDisableVertexAttribArray(1);
	glDisableVertexAttribArray(0);
}

static const char vertex_shader[] =
"attribute vec3 position;\n"
"attribute vec3 normal;\n"
"\n"
"uniform mat4 ModelViewProjectionMatrix;\n"
"uniform mat4 NormalMatrix;\n"
"uniform vec4 LightSourcePosition;\n"
"uniform vec4 MaterialColor;\n"
"\n"
"varying vec4 Color;\n"
"\n"
"void main(void)\n"
"{\n"
"	 // Transform the normal to eye coordinates\n"
"	 vec3 N = normalize(vec3(NormalMatrix * vec4(normal, 1.0)));\n"
"\n"
"	 // The LightSourcePosition is actually its direction for directional light\n"
"	 vec3 L = normalize(LightSourcePosition.xyz);\n"
"\n"
"	 float diffuse = max(dot(N, L), 0.0);\n"
"	 float ambient = 0.2;\n"
"\n"
"	 // Multiply the diffuse value by the vertex color (which is fixed in this case)\n"
"	 // to get the actual color that we will use to draw this vertex with\n"
"	 Color = vec4((ambient + diffuse) * MaterialColor.xyz, 1.0);\n"
"\n"
"	 // Transform the position to clip coordinates\n"
"	 gl_Position = ModelViewProjectionMatrix * vec4(position, 1.0);\n"
"}";

static const char fragment_shader[] =
"precision mediump float;\n"
"varying vec4 Color;\n"
"\n"
"void main(void)\n"
"{\n"
"	 gl_FragColor = Color;\n"
"}";

static bool check_egl_ext(const char *exts, const char *ext) {
	size_t extlen = strlen(ext);
	const char *end = exts + strlen(exts);

	while (exts < end) {
		if (*exts == ' ') {
			exts++;
			continue;
		}
		size_t n = strcspn(exts, " ");
		if (n == extlen && strncmp(ext, exts, n) == 0) {
			return true;
		}
		exts += n;
	}
	return false;
}

static void
init_egl(struct display *display, struct window *window)
{
	static const struct {
		char *extension, *entrypoint;
	} swap_damage_ext_to_entrypoint[] = {
		{
			.extension = "EGL_EXT_swap_buffers_with_damage",
			.entrypoint = "eglSwapBuffersWithDamageEXT",
		},
		{
			.extension = "EGL_KHR_swap_buffers_with_damage",
			.entrypoint = "eglSwapBuffersWithDamageKHR",
		},
	};

	static const EGLint context_attribs[] = {
		EGL_CONTEXT_CLIENT_VERSION, 2,
		EGL_NONE
	};
	const char *extensions;

	EGLint config_attribs[] = {
		EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
		EGL_RED_SIZE, 1,
		EGL_GREEN_SIZE, 1,
		EGL_BLUE_SIZE, 1,
		EGL_ALPHA_SIZE, 1,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
		EGL_DEPTH_SIZE, 1,
		EGL_NONE
	};

	EGLint major, minor, n, count, i, size;
	EGLConfig *configs;
	EGLBoolean ret;

	if (window->opaque || window->buffer_size == 16)
		config_attribs[9] = 0;

	display->egl.dpy =
		eglGetPlatformDisplay(EGL_PLATFORM_WAYLAND_KHR,
						display->display, NULL);
	assert(display->egl.dpy);

	ret = eglInitialize(display->egl.dpy, &major, &minor);
	assert(ret == EGL_TRUE);
	ret = eglBindAPI(EGL_OPENGL_ES_API);
	assert(ret == EGL_TRUE);

	if (!eglGetConfigs(display->egl.dpy, NULL, 0, &count) || count < 1)
		assert(0);

	configs = calloc(count, sizeof *configs);
	assert(configs);

	ret = eglChooseConfig(display->egl.dpy, config_attribs,
					configs, count, &n);
	assert(ret && n >= 1);

	for (i = 0; i < n; i++) {
		eglGetConfigAttrib(display->egl.dpy,
					configs[i], EGL_BUFFER_SIZE, &size);
		if (window->buffer_size == size) {
			display->egl.conf = configs[i];
			break;
		}
	}
	free(configs);
	if (display->egl.conf == NULL) {
		fprintf(stderr, "did not find config with buffer size %d\n",
			window->buffer_size);
		exit(EXIT_FAILURE);
	}

	display->egl.ctx = eglCreateContext(display->egl.dpy,
						 display->egl.conf,
						 EGL_NO_CONTEXT, context_attribs);
	assert(display->egl.ctx);

	display->swap_buffers_with_damage = NULL;
	extensions = eglQueryString(display->egl.dpy, EGL_EXTENSIONS);
	if (extensions &&
		 check_egl_ext(extensions, "EGL_EXT_buffer_age")) {
		for (i = 0; i < (int) ARRAY_LENGTH(swap_damage_ext_to_entrypoint); i++) {
			if (check_egl_ext(extensions, swap_damage_ext_to_entrypoint[i].extension)) {
				/* The EXTPROC is identical to the KHR one */
				display->swap_buffers_with_damage =
					(PFNEGLSWAPBUFFERSWITHDAMAGEEXTPROC)
					eglGetProcAddress(swap_damage_ext_to_entrypoint[i].entrypoint);
				break;
			}
		}
	}

	if (display->swap_buffers_with_damage)
		printf("has EGL_EXT_buffer_age and %s\n", swap_damage_ext_to_entrypoint[i].extension);

}

static void
fini_egl(struct display *display)
{
	eglTerminate(display->egl.dpy);
	eglReleaseThread();
}

static GLuint
create_shader(struct window *window, const char *source, GLenum shader_type)
{
	GLuint shader;
	GLint status;

	shader = glCreateShader(shader_type);
	assert(shader != 0);

	glShaderSource(shader, 1, (const char **) &source, NULL);
	glCompileShader(shader);

	glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
	if (!status) {
		char log[1000];
		GLsizei len;
		glGetShaderInfoLog(shader, 1000, &len, log);
		fprintf(stderr, "Error: compiling %s: %.*s\n",
			shader_type == GL_VERTEX_SHADER ? "vertex" : "fragment",
			len, log);
		exit(1);
	}

	return shader;
}

static void
init_gl(struct window *window)
{
	GLuint frag, vert;
	GLuint program;
	GLint status;

	frag = create_shader(window, fragment_shader, GL_FRAGMENT_SHADER);
	vert = create_shader(window, vertex_shader, GL_VERTEX_SHADER);

	program = glCreateProgram();
	glAttachShader(program, frag);
	glAttachShader(program, vert);
	glLinkProgram(program);

	glGetProgramiv(program, GL_LINK_STATUS, &status);
	if (!status) {
		char log[1000];
		GLsizei len;
		glGetProgramInfoLog(program, 1000, &len, log);
		fprintf(stderr, "Error: linking:\n%.*s\n", len, log);
		exit(1);
	}

	glUseProgram(program);

	window->gl.pos = 0;
	window->gl.col = 1;

	glBindAttribLocation(program, window->gl.pos, "pos");
	glBindAttribLocation(program, window->gl.col, "normal");
	glLinkProgram(program);

	window->gl.rotation_uniform =
		glGetUniformLocation(program, "rotation");

	/* Get the locations of the uniforms so we can access them */
	ModelViewProjectionMatrix_location = glGetUniformLocation(program, "ModelViewProjectionMatrix");
	NormalMatrix_location = glGetUniformLocation(program, "NormalMatrix");
	LightSourcePosition_location = glGetUniformLocation(program, "LightSourcePosition");
	MaterialColor_location = glGetUniformLocation(program, "MaterialColor");

	/* Set the LightSourcePosition uniform which is constant throught the program */
	glUniform4fv(LightSourcePosition_location, 1, LightSourcePosition);

	/* make the gears */
	gear1 = create_gear(1.0, 4.0, 1.0, 20, 0.7);
	gear2 = create_gear(0.5, 2.0, 2.0, 10, 0.7);
	gear3 = create_gear(1.3, 2.0, 0.5, 10, 0.7);

	glEnable(GL_CULL_FACE);
	glEnable(GL_DEPTH_TEST);
}

static void
handle_surface_configure(void *data, struct xdg_surface *surface,
			 uint32_t serial)
{
	struct window *window = data;

	xdg_surface_ack_configure(surface, serial);

	window->wait_for_configure = false;
}

static const struct xdg_surface_listener xdg_surface_listener = {
	handle_surface_configure
};

static void
handle_toplevel_configure(void *data, struct xdg_toplevel *toplevel,
			  int32_t width, int32_t height,
			  struct wl_array *states)
{
	struct window *window = data;
	uint32_t *p;

	window->fullscreen = 0;
	window->maximized = 0;
	wl_array_for_each(p, states) {
		uint32_t state = *p;
		switch (state) {
		case XDG_TOPLEVEL_STATE_FULLSCREEN:
			window->fullscreen = 1;
			break;
		case XDG_TOPLEVEL_STATE_MAXIMIZED:
			window->maximized = 1;
			break;
		}
	}

	if (width > 0 && height > 0) {
		if (!window->fullscreen && !window->maximized) {
			window->window_size.width = width;
			window->window_size.height = height;
		}
		window->geometry.width = width;
		window->geometry.height = height;
	} else if (!window->fullscreen && !window->maximized) {
		window->geometry = window->window_size;
	}

	if (window->native)
		wl_egl_window_resize(window->native,
					  window->geometry.width,
					  window->geometry.height, 0, 0);
	/* Update the projection matrix */
	GLfloat h = (GLfloat)window->geometry.height / (GLfloat)window->geometry.width;
	frustum(ProjectionMatrix, -1.0, 1.0, -h, h, 5.0, 60.0);

	/* Set the viewport */
	glViewport(0, 0, (GLint) window->geometry.width, (GLint) window->geometry.height);
}

static void
handle_toplevel_close(void *data, struct xdg_toplevel *xdg_toplevel)
{
	running = 0;
}

static void
handle_toplevel_configure_bounds (void *data, struct xdg_toplevel *xdg_toplevel,
                                  int32_t width, int32_t height)
{
}

static void handle_wm_capabilities (void *data, struct xdg_toplevel *xdg_toplevel,
                                    struct wl_array *capabilities)
{
}

static const struct xdg_toplevel_listener xdg_toplevel_listener = {
	handle_toplevel_configure,
	handle_toplevel_close,
	handle_toplevel_configure_bounds,
	handle_wm_capabilities,
};

static void
create_surface(struct window *window)
{
	struct display *display = window->display;
	EGLBoolean ret;

	window->surface = wl_compositor_create_surface(display->compositor);

	window->native =
		wl_egl_window_create(window->surface,
					  window->geometry.width,
					  window->geometry.height);
	window->egl_surface =
		eglCreatePlatformWindowSurface(display->egl.dpy,
							display->egl.conf,
							window->native, NULL);

	window->xdg_surface = xdg_wm_base_get_xdg_surface(display->wm_base,
							  window->surface);
	xdg_surface_add_listener(window->xdg_surface,
				 &xdg_surface_listener, window);

	window->xdg_toplevel =
		xdg_surface_get_toplevel(window->xdg_surface);
	xdg_toplevel_add_listener(window->xdg_toplevel,
				  &xdg_toplevel_listener, window);

	xdg_toplevel_set_title(window->xdg_toplevel, "Wayland Gears");

	window->wait_for_configure = true;
	wl_surface_commit(window->surface);

	ret = eglMakeCurrent(window->display->egl.dpy, window->egl_surface,
				  window->egl_surface, window->display->egl.ctx);
	assert(ret == EGL_TRUE);

	if (!window->frame_sync)
		eglSwapInterval(display->egl.dpy, 0);

	if (!display->wm_base)
		return;

	if (window->fullscreen)
		xdg_toplevel_set_fullscreen(window->xdg_toplevel, NULL);
}

static void
destroy_surface(struct window *window)
{
	/* Required, otherwise segfault in egl_dri2.c: dri2_make_current()
	 * on eglReleaseThread(). */
	eglMakeCurrent(window->display->egl.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE,
				 EGL_NO_CONTEXT);

	eglDestroySurface(window->display->egl.dpy,
						 window->egl_surface);
	wl_egl_window_destroy(window->native);

	if (window->xdg_toplevel)
		xdg_toplevel_destroy(window->xdg_toplevel);
	if (window->xdg_surface)
		xdg_surface_destroy(window->xdg_surface);
	wl_surface_destroy(window->surface);

	if (window->callback)
		wl_callback_destroy(window->callback);
}

static void
redraw(void *data, struct wl_callback *callback, uint32_t time)
{
	struct window *window = data;
	struct display *display = window->display;
	static const GLfloat red[4] = { 0.8, 0.1, 0.0, 1.0 };
	static const GLfloat green[4] = { 0.0, 0.8, 0.2, 1.0 };
	static const GLfloat blue[4] = { 0.2, 0.2, 1.0, 1.0 };
	GLfloat transform[16];
	identity(transform);

	glClearColor(0.0, 0.0, 0.0, 0.0);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	struct wl_region *region;
	EGLint buffer_age = 0;
	EGLint rect[4];

	usleep(window->delay);
	static double tRot0 = -1.0, tRate0 = -1.0;
	struct timeval  tv;
	gettimeofday(&tv, NULL);
	double ms = tv.tv_sec * 1000 + tv.tv_usec / 1000;
	double dt, t = ms / 1000.0;

	if (tRot0 < 0.0)
		tRot0 = t;
	dt = t - tRot0;
	tRot0 = t;

	/* advance rotation for next frame */
	angle += 70.0 * dt;  /* 70 degrees per second */
	if (angle > 3600.0)
		angle -= 3600.0;


	/* Translate and rotate the view */
	translate(transform, 0, 0, -40);
	rotate(transform, 2 * M_PI * view_rot[0] / 360.0, 1, 0, 0);
	rotate(transform, 2 * M_PI * view_rot[1] / 360.0, 0, 1, 0);
	rotate(transform, 2 * M_PI * view_rot[2] / 360.0, 0, 0, 1);

	/* Draw the gears */
	draw_gear(gear1, transform, -3.0, -2.0, angle, red);
	draw_gear(gear2, transform, 3.1, -2.0, -2 * angle - 9.0, green);
	draw_gear(gear3, transform, -3.1, 4.2, -2 * angle - 25.0, blue);

	if (window->opaque || window->fullscreen) {
		region = wl_compositor_create_region(window->display->compositor);
		wl_region_add(region, 0, 0,
					window->geometry.width,
					window->geometry.height);
		wl_surface_set_opaque_region(window->surface, region);
		wl_region_destroy(region);
	} else {
		wl_surface_set_opaque_region(window->surface, NULL);
	}

	if (display->swap_buffers_with_damage && buffer_age > 0) {
		rect[0] = window->geometry.width / 4 - 1;
		rect[1] = window->geometry.height / 4 - 1;
		rect[2] = window->geometry.width / 2 + 2;
		rect[3] = window->geometry.height / 2 + 2;
		display->swap_buffers_with_damage(display->egl.dpy,
						  window->egl_surface,
						  rect, 1);
	} else {
		eglSwapBuffers(display->egl.dpy, window->egl_surface);
	}
	window->frames++;

	if (tRate0 < 0.0)
		tRate0 = t;
	if (t - tRate0 >= 5.0) {
		GLfloat seconds = t - tRate0;
		GLfloat fps = window->frames / seconds;
		printf("%d frames in %3.1f seconds = %6.3f FPS\n", window->frames, seconds,
				fps);
		tRate0 = t;
		window->frames = 0;
	}
}

static void
pointer_handle_enter(void *data, struct wl_pointer *pointer,
			  uint32_t serial, struct wl_surface *surface,
			  wl_fixed_t sx, wl_fixed_t sy)
{
	struct display *display = data;
	struct wl_buffer *buffer;
	struct wl_cursor *cursor = display->default_cursor;
	struct wl_cursor_image *image;

	if (display->window->fullscreen)
		wl_pointer_set_cursor(pointer, serial, NULL, 0, 0);
	else if (cursor) {
		image = display->default_cursor->images[0];
		buffer = wl_cursor_image_get_buffer(image);
		if (!buffer)
			return;
		wl_pointer_set_cursor(pointer, serial,
						display->cursor_surface,
						image->hotspot_x,
						image->hotspot_y);
		wl_surface_attach(display->cursor_surface, buffer, 0, 0);
		wl_surface_damage(display->cursor_surface, 0, 0,
				  image->width, image->height);
		wl_surface_commit(display->cursor_surface);
	}
}

static void
pointer_handle_leave(void *data, struct wl_pointer *pointer,
			  uint32_t serial, struct wl_surface *surface)
{
}

static int rotate_drag;
static int last_pointer_x, last_pointer_y;

static void
pointer_handle_motion(void *data, struct wl_pointer *pointer,
				uint32_t time, wl_fixed_t sx, wl_fixed_t sy)
{
	int x = wl_fixed_to_int(sx);
	int y = wl_fixed_to_int(sy);

	if (rotate_drag)
	{
		view_rot[0] += (y - last_pointer_y) * 0.5;
		view_rot[1] += (x - last_pointer_x) * 0.5;
	}

	last_pointer_x = x;
	last_pointer_y = y;
}

static void
pointer_handle_button(void *data, struct wl_pointer *wl_pointer,
				uint32_t serial, uint32_t time, uint32_t button,
				uint32_t state)
{
	struct display *display = data;

	if (!display->window->xdg_toplevel)
		return;

	if (button == BTN_RIGHT)
	{
		rotate_drag = state == WL_POINTER_BUTTON_STATE_PRESSED;
	}

	if (button == BTN_LEFT && state == WL_POINTER_BUTTON_STATE_PRESSED)
		xdg_toplevel_move(display->window->xdg_toplevel,
				  display->seat, serial);
}

static void
pointer_handle_axis(void *data, struct wl_pointer *wl_pointer,
			 uint32_t time, uint32_t axis, wl_fixed_t value)
{
}

static void
pointer_handle_frame(void *data, struct wl_pointer *wl_pointer)
{
}

static const struct wl_pointer_listener pointer_listener = {
	pointer_handle_enter,
	pointer_handle_leave,
	pointer_handle_motion,
	pointer_handle_button,
	pointer_handle_axis,
	.frame = pointer_handle_frame,
};

static void
touch_handle_down(void *data, struct wl_touch *wl_touch,
		  uint32_t serial, uint32_t time, struct wl_surface *surface,
		  int32_t id, wl_fixed_t x_w, wl_fixed_t y_w)
{
	struct display *d = (struct display *)data;

	if (!d->wm_base)
		return;

	xdg_toplevel_move(d->window->xdg_toplevel, d->seat, serial);
}

static void
touch_handle_up(void *data, struct wl_touch *wl_touch,
		uint32_t serial, uint32_t time, int32_t id)
{
}

static void
touch_handle_motion(void *data, struct wl_touch *wl_touch,
			 uint32_t time, int32_t id, wl_fixed_t x_w, wl_fixed_t y_w)
{
}

static void
touch_handle_frame(void *data, struct wl_touch *wl_touch)
{
}

static void
touch_handle_cancel(void *data, struct wl_touch *wl_touch)
{
}

static void
touch_handle_shape(void *data, struct wl_touch *wl_touch, int32_t id,
			wl_fixed_t major, wl_fixed_t minor)
{
}

static const struct wl_touch_listener touch_listener = {
	touch_handle_down,
	touch_handle_up,
	touch_handle_motion,
	touch_handle_frame,
	touch_handle_cancel,
	.shape = touch_handle_shape,
};

static void
keyboard_handle_keymap(void *data, struct wl_keyboard *keyboard,
				 uint32_t format, int fd, uint32_t size)
{
	/* Just so we don’t leak the keymap fd */
	close(fd);
}

static void
keyboard_handle_enter(void *data, struct wl_keyboard *keyboard,
				uint32_t serial, struct wl_surface *surface,
				struct wl_array *keys)
{
}

static void
keyboard_handle_leave(void *data, struct wl_keyboard *keyboard,
				uint32_t serial, struct wl_surface *surface)
{
}

static void
keyboard_handle_key(void *data, struct wl_keyboard *keyboard,
			 uint32_t serial, uint32_t time, uint32_t key,
			 uint32_t state)
{
	struct display *d = data;

	if (!d->wm_base)
		return;

	if (key == KEY_F11 && state) {
		if (d->window->fullscreen)
			xdg_toplevel_unset_fullscreen(d->window->xdg_toplevel);
		else
			xdg_toplevel_set_fullscreen(d->window->xdg_toplevel, NULL);
	} else if (key == KEY_ESC && state)
		running = 0;
}

static void
keyboard_handle_modifiers(void *data, struct wl_keyboard *keyboard,
			  uint32_t serial, uint32_t mods_depressed,
			  uint32_t mods_latched, uint32_t mods_locked,
			  uint32_t group)
{
}

static void
keyboard_handle_repeat_info(void *data, struct wl_keyboard *keyboard,
				int32_t rate, int32_t delay)
{
}

static const struct wl_keyboard_listener keyboard_listener = {
	keyboard_handle_keymap,
	keyboard_handle_enter,
	keyboard_handle_leave,
	keyboard_handle_key,
	keyboard_handle_modifiers,
	.repeat_info = keyboard_handle_repeat_info,
};

static void
seat_handle_capabilities(void *data, struct wl_seat *seat,
			 enum wl_seat_capability caps)
{
	struct display *d = data;

	if ((caps & WL_SEAT_CAPABILITY_POINTER) && !d->pointer) {
		d->pointer = wl_seat_get_pointer(seat);
		wl_pointer_add_listener(d->pointer, &pointer_listener, d);
	} else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && d->pointer) {
		wl_pointer_destroy(d->pointer);
		d->pointer = NULL;
	}

	if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !d->keyboard) {
		d->keyboard = wl_seat_get_keyboard(seat);
		wl_keyboard_add_listener(d->keyboard, &keyboard_listener, d);
	} else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && d->keyboard) {
		wl_keyboard_destroy(d->keyboard);
		d->keyboard = NULL;
	}

	if ((caps & WL_SEAT_CAPABILITY_TOUCH) && !d->touch) {
		d->touch = wl_seat_get_touch(seat);
		wl_touch_set_user_data(d->touch, d);
		wl_touch_add_listener(d->touch, &touch_listener, d);
	} else if (!(caps & WL_SEAT_CAPABILITY_TOUCH) && d->touch) {
		wl_touch_destroy(d->touch);
		d->touch = NULL;
	}
}

static void
seat_handle_name(void *data, struct wl_seat *seat, const char *name)
{
}

static const struct wl_seat_listener seat_listener = {
	seat_handle_capabilities,
	.name = seat_handle_name,
};

static void
xdg_wm_base_ping(void *data, struct xdg_wm_base *shell, uint32_t serial)
{
	xdg_wm_base_pong(shell, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = {
	xdg_wm_base_ping,
};

static void
registry_handle_global(void *data, struct wl_registry *registry,
				 uint32_t name, const char *interface, uint32_t version)
{
	struct display *d = data;

	if (strcmp(interface, "wl_compositor") == 0) {
		d->compositor =
			wl_registry_bind(registry, name,
					 &wl_compositor_interface,
					 MIN(version, 4));
	} else if (strcmp(interface, "xdg_wm_base") == 0) {
		d->wm_base = wl_registry_bind(registry, name,
							&xdg_wm_base_interface, 1);
		xdg_wm_base_add_listener(d->wm_base, &wm_base_listener, d);
	} else if (strcmp(interface, "wl_seat") == 0) {
		d->seat = wl_registry_bind(registry, name,
						&wl_seat_interface, 1);
		wl_seat_add_listener(d->seat, &seat_listener, d);
	} else if (strcmp(interface, "wl_shm") == 0) {
		d->shm = wl_registry_bind(registry, name,
					  &wl_shm_interface, 1);
		d->cursor_theme = wl_cursor_theme_load(NULL, 32, d->shm);
		if (!d->cursor_theme) {
			fprintf(stderr, "unable to load default theme\n");
			return;
		}
		d->default_cursor =
			wl_cursor_theme_get_cursor(d->cursor_theme, "left_ptr");
		if (!d->default_cursor) {
			fprintf(stderr, "unable to load default left pointer\n");
			// TODO: abort ?
		}
	}
}

static void
registry_handle_global_remove(void *data, struct wl_registry *registry,
					uint32_t name)
{
}

static const struct wl_registry_listener registry_listener = {
	registry_handle_global,
	registry_handle_global_remove
};

static void
signal_int(int signum)
{
	running = 0;
}

static void
usage(int error_code)
{
	fprintf(stderr, "Usage: simple-egl [OPTIONS]\n\n"
		"  -d <us>\tBuffer swap delay in microseconds\n"
		"  -f\tRun in fullscreen mode\n"
		"  -o\tCreate an opaque surface\n"
		"  -s\tUse a 16 bpp EGL config\n"
		"  -b\tDon't sync to compositor redraw (eglSwapInterval 0)\n"
		"  -h\tThis help text\n\n");

	exit(error_code);
}

int
main(int argc, char **argv)
{
	struct sigaction sigint;
	struct display display = { 0 };
	struct window  window  = { 0 };
	int i, ret = 0;

	window.display = &display;
	display.window = &window;
	window.geometry.width  = 400;
	window.geometry.height = 400;
	window.window_size = window.geometry;
	window.buffer_size = 32;
	window.frame_sync = 1;
	window.delay = 0;

	for (i = 1; i < argc; i++) {
		if (strcmp("-d", argv[i]) == 0 && i+1 < argc)
			window.delay = atoi(argv[++i]);
		else if (strcmp("-f", argv[i]) == 0)
			window.fullscreen = 1;
		else if (strcmp("-o", argv[i]) == 0)
			window.opaque = 1;
		else if (strcmp("-s", argv[i]) == 0)
			window.buffer_size = 16;
		else if (strcmp("-b", argv[i]) == 0)
			window.frame_sync = 0;
		else if (strcmp("-h", argv[i]) == 0)
			usage(EXIT_SUCCESS);
		else
			usage(EXIT_FAILURE);
	}

	display.display = wl_display_connect(NULL);
	assert(display.display);

	display.registry = wl_display_get_registry(display.display);
	wl_registry_add_listener(display.registry,
				 &registry_listener, &display);

	wl_display_roundtrip(display.display);

	init_egl(&display, &window);
	create_surface(&window);
	init_gl(&window);

	display.cursor_surface =
		wl_compositor_create_surface(display.compositor);

	sigint.sa_handler = signal_int;
	sigemptyset(&sigint.sa_mask);
	sigint.sa_flags = SA_RESETHAND;
	sigaction(SIGINT, &sigint, NULL);

	/* The mainloop here is a little subtle.  Redrawing will cause
	 * EGL to read events so we can just call
	 * wl_display_dispatch_pending() to handle any events that got
	 * queued up as a side effect. */
	while (running && ret != -1) {
		if (window.wait_for_configure) {
			ret = wl_display_dispatch(display.display);
		} else {
			ret = wl_display_dispatch_pending(display.display);
			redraw(&window, NULL, 0);
		}
	}

	fprintf(stderr, "wl-gears exiting\n");

	destroy_surface(&window);
	fini_egl(&display);

	wl_surface_destroy(display.cursor_surface);
	if (display.cursor_theme)
		wl_cursor_theme_destroy(display.cursor_theme);

	if (display.wm_base)
		xdg_wm_base_destroy(display.wm_base);

	if (display.compositor)
		wl_compositor_destroy(display.compositor);

	wl_registry_destroy(display.registry);
	wl_display_flush(display.display);
	wl_display_disconnect(display.display);

	return 0;
}
