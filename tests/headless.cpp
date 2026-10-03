#include <aquamarine/backend/Backend.hpp>
#include <aquamarine/backend/Headless.hpp>
#include <dlfcn.h>
#include <xf86drm.h>
#include <fcntl.h>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <string_view>
#include <GLES3/gl32.h>
#include <chrono>

static int renderNode() {
    static const int fd = [] {
        const char* path = std::getenv("AUTO_INPUT_RENDER_NODE");
        if (!path || !std::string_view(path).starts_with("/dev/dri/renderD"))
            throw std::runtime_error("AUTO_INPUT_RENDER_NODE must name a render node");
        int node = open(path, O_RDWR | O_CLOEXEC);
        if (node < 0 || drmGetNodeTypeFromFd(node) != DRM_NODE_RENDER)
            throw std::runtime_error("Cannot open a DRM render node");
        return node;
    }();
    return fd;
}

Hyprutils::Memory::CSharedPointer<Aquamarine::CBackend> Aquamarine::CBackend::create(const std::vector<SBackendImplementationOptions>& requested, const SBackendOptions& options) {
    using Create  = decltype(&CBackend::create);
    auto original = reinterpret_cast<Create>(dlsym(RTLD_NEXT, "_ZN10Aquamarine8CBackend6createERKSt6vectorINS_29SBackendImplementationOptionsESaIS2_EERKNS_15SBackendOptionsE"));
    if (!original)
        throw std::runtime_error("Cannot resolve Aquamarine::CBackend::create");
    SBackendImplementationOptions headless;
    headless.backendType        = AQ_BACKEND_HEADLESS;
    headless.backendRequestMode = AQ_BACKEND_REQUEST_MANDATORY;
    return original({headless}, options);
}

int Aquamarine::CHeadlessBackend::drmFD() {
    return renderNode();
}

int Aquamarine::CHeadlessBackend::drmRenderNodeFD() {
    return renderNode();
}

using Clock = std::chrono::steady_clock;
static Clock::time_point mappedAt;

extern "C" void          glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void* data) {
    static auto original = reinterpret_cast<decltype(&glReadPixels)>(dlsym(RTLD_NEXT, "glReadPixels"));
    const auto  started  = Clock::now();
    original(x, y, width, height, format, type, data);
    if (!data)
        std::fprintf(stderr, "capture-readback-submit-us %lld\n", static_cast<long long>(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - started).count()));
}

extern "C" void* glMapBufferRange(GLenum target, GLintptr offset, GLsizeiptr length, GLbitfield access) {
    static auto original = reinterpret_cast<decltype(&glMapBufferRange)>(dlsym(RTLD_NEXT, "glMapBufferRange"));
    if (target == GL_PIXEL_PACK_BUFFER)
        mappedAt = Clock::now();
    return original(target, offset, length, access);
}

extern "C" GLboolean glUnmapBuffer(GLenum target) {
    static auto original = reinterpret_cast<decltype(&glUnmapBuffer)>(dlsym(RTLD_NEXT, "glUnmapBuffer"));
    const auto  result   = original(target);
    if (target == GL_PIXEL_PACK_BUFFER)
        std::fprintf(stderr, "capture-map-copy-us %lld\n", static_cast<long long>(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - mappedAt).count()));
    return result;
}

extern "C" GLenum glClientWaitSync(GLsync sync, GLbitfield flags, GLuint64 timeout) {
    static auto original = reinterpret_cast<decltype(&glClientWaitSync)>(dlsym(RTLD_NEXT, "glClientWaitSync"));
    if (std::getenv("HYPRAUTO_TEST_HOLD_FENCES"))
        return GL_TIMEOUT_EXPIRED;
    return original(sync, flags, timeout);
}

extern "C" GLsync glFenceSync(GLenum condition, GLbitfield flags) {
    static auto original = reinterpret_cast<decltype(&glFenceSync)>(dlsym(RTLD_NEXT, "glFenceSync"));
    if (const auto count = std::getenv("HYPRAUTO_TEST_FAIL_FENCE")) {
        const auto remaining = std::atoi(count) - 1;
        if (!remaining) {
            unsetenv("HYPRAUTO_TEST_FAIL_FENCE");
            static auto enable = reinterpret_cast<decltype(&glEnable)>(dlsym(RTLD_NEXT, "glEnable"));
            enable(0);
            return nullptr;
        }
        const auto value = std::to_string(remaining);
        setenv("HYPRAUTO_TEST_FAIL_FENCE", value.c_str(), 1);
    }
    return original(condition, flags);
}
