// OpenGL through opengl32.dll on macOS's OpenGL: a legacy context and a 4.1 core one (wglCreateContextAttribsARB),
// functions from wglGetProcAddress, a framebuffer object cleared and read back, and a shader compiled.
#include <windows.h>
#include <GL/gl.h>
#include <stdio.h>
#include <string.h>
static int failures;
#define CHECK(cond) do { if (!(cond)) { printf("check failed (line %d): %s\n", __LINE__, #cond); failures++; } } while (0)
typedef HGLRC (WINAPI *create_attribs_fn)(HDC, HGLRC, const int *);
typedef void (APIENTRY *gen_fn)(GLsizei, GLuint *);
typedef void (APIENTRY *bind_fn)(GLenum, GLuint);
typedef void (APIENTRY *storage_fn)(GLenum, GLenum, GLsizei, GLsizei);
typedef void (APIENTRY *attach_fn)(GLenum, GLenum, GLenum, GLuint);
typedef GLenum (APIENTRY *status_fn)(GLenum);
typedef GLuint (APIENTRY *create_shader_fn)(GLenum);
typedef void (APIENTRY *source_fn)(GLuint, GLsizei, const char *const *, const GLint *);
typedef void (APIENTRY *compile_fn)(GLuint);
typedef void (APIENTRY *getiv_fn)(GLuint, GLenum, GLint *);

static GLuint clear_read(float r, float g, float b) {    // a 4x4 RGBA8 framebuffer cleared, its middle read back
    gen_fn gen = (gen_fn)wglGetProcAddress("glGenFramebuffers"), genr = (gen_fn)wglGetProcAddress("glGenRenderbuffers");
    bind_fn bind = (bind_fn)wglGetProcAddress("glBindFramebuffer"), bindr = (bind_fn)wglGetProcAddress("glBindRenderbuffer");
    storage_fn storage = (storage_fn)wglGetProcAddress("glRenderbufferStorage");
    attach_fn attach = (attach_fn)wglGetProcAddress("glFramebufferRenderbuffer");
    status_fn status = (status_fn)wglGetProcAddress("glCheckFramebufferStatus");
    CHECK(gen && genr && bind && bindr && storage && attach && status);
    if (!gen || !genr || !bind || !bindr || !storage || !attach || !status) return 0;
    GLuint fb = 0, rb = 0;
    gen(1, &fb); genr(1, &rb);
    bindr(0x8D41, rb); storage(0x8D41, 0x8058 /* GL_RGBA8 */, 4, 4);
    bind(0x8D40, fb); attach(0x8D40, 0x8CE0, 0x8D41, rb);
    CHECK(status(0x8D40) == 0x8CD5);                  // GL_FRAMEBUFFER_COMPLETE
    glViewport(0, 0, 4, 4);
    glClearColor(r, g, b, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    GLuint px = 0;
    glReadPixels(2, 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, &px);
    return px;
}
static int compiles(const char *src) {
    create_shader_fn create = (create_shader_fn)wglGetProcAddress("glCreateShader");
    source_fn source = (source_fn)wglGetProcAddress("glShaderSource");
    compile_fn compile = (compile_fn)wglGetProcAddress("glCompileShader");
    getiv_fn getiv = (getiv_fn)wglGetProcAddress("glGetShaderiv");
    if (!create || !source || !compile || !getiv) return 0;
    GLuint s = create(0x8B30 /* GL_FRAGMENT_SHADER */);
    source(s, 1, &src, NULL);
    compile(s);
    GLint ok = 0;
    getiv(s, 0x8B81 /* GL_COMPILE_STATUS */, &ok);
    return ok;
}
int main(void) {
    HWND hwnd = CreateWindowA("STATIC", "gl", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, NULL, NULL, NULL, NULL);
    HDC dc = GetDC(hwnd);
    PIXELFORMATDESCRIPTOR pfd = { sizeof pfd, 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER, PFD_TYPE_RGBA, 32 };
    int f = ChoosePixelFormat(dc, &pfd);
    CHECK(f > 0 && SetPixelFormat(dc, f, &pfd));
    HGLRC legacy = wglCreateContext(dc);
    CHECK(legacy && wglMakeCurrent(dc, legacy) && wglGetCurrentContext() == legacy);
    const char *version = (const char *)glGetString(GL_VERSION), *renderer = (const char *)glGetString(GL_RENDERER);
    CHECK(version && version[0] == '2');
    CHECK(clear_read(1.0f, 0.5f, 0.0f) == 0xff0080ffu);
    CHECK(compiles("#version 110\nvoid main() { gl_FragColor = vec4(1.0); }"));
    create_attribs_fn create_attribs = (create_attribs_fn)wglGetProcAddress("wglCreateContextAttribsARB");
    CHECK(create_attribs != NULL);
    char core_version[64] = "";
    if (create_attribs) {
        int attribs[] = { 0x2091, 4, 0x2092, 1, 0x9126, 1, 0 };   // 4.1, core profile
        HGLRC core = create_attribs(dc, NULL, attribs);
        CHECK(core && wglMakeCurrent(dc, core));
        const char *v = (const char *)glGetString(GL_VERSION);
        if (v) snprintf(core_version, sizeof core_version, "%s", v);
        CHECK(v && v[0] == '4');
        CHECK(clear_read(0.0f, 0.25f, 1.0f) == 0xffff4000u);
        CHECK(compiles("#version 410\nout vec4 c; void main() { c = vec4(1.0); }"));
        wglMakeCurrent(NULL, NULL);
        CHECK(wglDeleteContext(core));
    }
    wglMakeCurrent(NULL, NULL);
    CHECK(wglDeleteContext(legacy));
    printf("opengl: %s on %s; core %s: %d failed checks -> %s\n", version ? version : "?", renderer ? renderer : "?", core_version, failures, failures ? "FAILED" : "ok");
    return failures;
}
