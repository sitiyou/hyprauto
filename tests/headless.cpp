#include <aquamarine/backend/Backend.hpp>
#include <aquamarine/backend/Headless.hpp>
#include <dlfcn.h>
#include <xf86drm.h>
#include <fcntl.h>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string_view>

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
