#ifndef DEMO_GL_API_H
#define DEMO_GL_API_H
#ifdef DEMO_USE_GLAD
#include <glad/gl.h>
#define GLFW_INCLUDE_NONE
#else
#define GL_GLEXT_PROTOTYPES
#define GLFW_INCLUDE_GLEXT
#endif
#include <GLFW/glfw3.h>
#endif
