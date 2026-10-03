#include "Capture.hpp"
#include <src/render/Renderer.hpp>
#include <src/render/pass/SurfacePassElement.hpp>
#include <src/protocols/core/Compositor.hpp>
#include <src/protocols/XDGShell.hpp>
#include <src/xwayland/XSurface.hpp>
#include <src/desktop/view/Popup.hpp>
#include <src/helpers/cm/ColorManagement.hpp>
#include <hyprutils/utils/ScopeGuard.hpp>
#include <nlohmann/json.hpp>
#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

using Hyprutils::Utils::CScopeGuard;
using nlohmann::json;
using Render::GL::g_pHyprOpenGL;

namespace Hyprauto::Capture {
    struct Job::State {
        uint64_t                              id     = 0;
        bool                                  active = false;
        uint32_t                              width = 0, height = 0;
        SP<Render::IFramebuffer>              framebuffer;
        GLuint                                pbo   = 0;
        GLsync                                fence = nullptr;
        std::atomic<bool>                     done  = true;
        std::jthread                          worker;
        std::string                           pixels, error, path;
        std::chrono::steady_clock::time_point started;

        ~State() {
            shutdown();
        }

        void cancel() {
            active = false;
            if (done.load())
                pixels.clear();
            if (fence) {
                g_pHyprOpenGL->makeEGLCurrent();
                glDeleteSync(fence);
                fence = nullptr;
            }
        }

        void shutdown() {
            cancel();
            if (worker.joinable())
                worker.join();
            if (pbo) {
                g_pHyprOpenGL->makeEGLCurrent();
                glDeleteBuffers(1, &pbo);
                pbo = 0;
            }
            framebuffer.reset();
            pixels.clear();
        }

        std::string start(PHLWINDOW window, const std::string& outputPath) {
            if (active || !done.load())
                return "error: screenshot is busy; release the previous capture";
            if (g_pHyprRenderer->type() != Render::IHyprRenderer::RT_GL)
                return "error: screenshot requires the OpenGL renderer";
            const auto monitor = window->m_monitor.lock();
            if (!monitor || !monitor->m_output)
                return "error: session target is unavailable";
            if (window->m_ruleApplicator->noScreenShare().valueOrDefault())
                return "error: target disallows screen capture";
            const auto size = (window->getWindowMainSurfaceBox().size() * monitor->m_scale).round();
            if (size.x < 1 || size.y < 1 || size.x > 16384 || size.y > 16384 || size.x * size.y > 67108864)
                return "error: framebuffer dimensions exceed capture limits";
            if (worker.joinable())
                worker.join();
            pixels.clear();
            error.clear();
            path   = outputPath;
            width  = static_cast<uint32_t>(size.x);
            height = static_cast<uint32_t>(size.y);
            g_pHyprOpenGL->makeEGLCurrent();
            if (!framebuffer)
                framebuffer = g_pHyprRenderer->createFB("hyprauto capture");
            framebuffer->alloc(width, height, DRM_FORMAT_ABGR8888);
            if (!framebuffer->isAllocated())
                return "error: cannot allocate capture framebuffer";
            framebuffer->setImageDescription(NColorManagement::DEFAULT_SRGB_IMAGE_DESCRIPTION);
            CRegion damage{0, 0, size.x, size.y};
            if (!g_pHyprRenderer->beginFullFakeRender(monitor, damage, framebuffer))
                return "error: cannot begin window capture";
            const auto feedback                      = g_pHyprRenderer->m_bBlockSurfaceFeedback;
            g_pHyprRenderer->m_bBlockSurfaceFeedback = true;
            CScopeGuard restoreFeedback([&] { g_pHyprRenderer->m_bBlockSurfaceFeedback = feedback; });
            g_pHyprRenderer->m_renderData.fbSize = size;
            g_pHyprRenderer->setProjectionType(Render::RPT_EXPORT);
            g_pHyprRenderer->m_renderData.transformDamage = false;
            g_pHyprRenderer->setViewport(0, 0, width, height);
            g_pHyprRenderer->draw(CClearPassElement::SClearData{{0, 0, 0, 0}}, damage);
            g_pHyprRenderer->startRenderPass();
            const auto now            = Time::steadyNow();
            const auto windowSize     = window->getWindowMainSurfaceBox().size();
            auto       addSurfaceTree = [&](SP<CWLSurfaceResource> root, const Vector2D& pos, bool popup, bool squishOversized) {
                int surfaceCounter = 0;
                root->breadthfirst(
                    [&](SP<CWLSurfaceResource> surface, const Vector2D& offset, void*) {
                        if (!surface->m_current.texture || surface->m_current.size.x < 1 || surface->m_current.size.y < 1)
                            return;
                        CSurfacePassElement::SRenderData renderdata{monitor, now};
                        renderdata.pos             = pos;
                        renderdata.localPos        = offset;
                        renderdata.surface         = surface;
                        renderdata.texture         = surface->m_current.texture;
                        renderdata.mainSurface     = !popup && surface == root;
                        renderdata.w               = windowSize.x;
                        renderdata.h               = windowSize.y;
                        renderdata.dontRound       = true;
                        renderdata.alpha           = 1.F;
                        renderdata.fadeAlpha       = 1.F;
                        renderdata.blur            = false;
                        renderdata.squishOversized = squishOversized;
                        renderdata.popup           = popup;
                        renderdata.surfaceCounter  = surfaceCounter++;
                        g_pHyprRenderer->addPassElement(makeUnique<CSurfacePassElement>(renderdata));
                    },
                    nullptr);
            };
            addSurfaceTree(window->wlSurface()->resource(), monitor->m_position, false, true);
            const auto geometry = [&] {
                if (const auto surface = window->m_xdgSurface.lock())
                    return surface->m_current.geometry;
                if (const auto surface = window->m_xwaylandSurface.lock())
                    return surface->m_geometry;
                return CBox{};
            }();
            window->m_popupHead->breadthfirst(
                [&](SP<Desktop::View::CPopup> popup, void*) {
                    if (!popup->m_mapped)
                        return;
                    addSurfaceTree(popup->wlSurface()->resource(), monitor->m_position - geometry.pos() + popup->coordsRelativeToParent(), true, false);
                },
                nullptr);
            g_pHyprRenderer->m_renderData.blockScreenShader = true;
            g_pHyprRenderer->endRender();
            g_pHyprRenderer->m_renderData.pMonitor.reset();

            GLint previousBuffer, previousFramebuffer, alignment, rowLength, skipRows, skipPixels;
            glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &previousBuffer);
            glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previousFramebuffer);
            glGetIntegerv(GL_PACK_ALIGNMENT, &alignment);
            glGetIntegerv(GL_PACK_ROW_LENGTH, &rowLength);
            glGetIntegerv(GL_PACK_SKIP_ROWS, &skipRows);
            glGetIntegerv(GL_PACK_SKIP_PIXELS, &skipPixels);
            CScopeGuard restoreGL([&] {
                glBindBuffer(GL_PIXEL_PACK_BUFFER, previousBuffer);
                glBindFramebuffer(GL_READ_FRAMEBUFFER, previousFramebuffer);
                glPixelStorei(GL_PACK_ALIGNMENT, alignment);
                glPixelStorei(GL_PACK_ROW_LENGTH, rowLength);
                glPixelStorei(GL_PACK_SKIP_ROWS, skipRows);
                glPixelStorei(GL_PACK_SKIP_PIXELS, skipPixels);
            });
            if (!pbo)
                glGenBuffers(1, &pbo);
            glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo);
            glBufferData(GL_PIXEL_PACK_BUFFER, static_cast<GLsizeiptr>(width) * height * 4, nullptr, GL_STREAM_READ);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<Render::GL::CGLFramebuffer*>(framebuffer.get())->getFBID());
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glPixelStorei(GL_PACK_ROW_LENGTH, 0);
            glPixelStorei(GL_PACK_SKIP_ROWS, 0);
            glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
            glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
            glFlush();
            if (!fence || glGetError() != GL_NO_ERROR) {
                cancel();
                return "error: cannot submit asynchronous pixel readback";
            }
            active  = true;
            started = std::chrono::steady_clock::now();
            return json{{"id", ++id}}.dump();
        }

        void progress() {
            if (!fence)
                return;
            g_pHyprOpenGL->makeEGLCurrent();
            const auto status = glClientWaitSync(fence, 0, 0);
            if (status == GL_TIMEOUT_EXPIRED && std::chrono::steady_clock::now() - started < std::chrono::seconds(10))
                return;
            glDeleteSync(fence);
            fence = nullptr;
            if (status != GL_ALREADY_SIGNALED && status != GL_CONDITION_SATISFIED) {
                error = "asynchronous pixel readback failed or timed out";
                return;
            }
            const auto  byteSize = static_cast<size_t>(width) * height * 4;
            std::string data;
            data.reserve(byteSize);
            GLint previousBuffer;
            glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &previousBuffer);
            glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo);
            CScopeGuard restore([&] { glBindBuffer(GL_PIXEL_PACK_BUFFER, previousBuffer); });
            const auto* mapped = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, byteSize, GL_MAP_READ_BIT);
            if (!mapped) {
                error = "cannot map completed pixel readback";
                return;
            }
            data.resize_and_overwrite(byteSize, [mapped](char* destination, size_t size) {
                std::memcpy(destination, mapped, size);
                return size;
            });
            if (!glUnmapBuffer(GL_PIXEL_PACK_BUFFER)) {
                error = "pixel readback buffer was corrupted";
                return;
            }
            done = false;
            try {
                worker = std::jthread([this, data = std::move(data), outputPath = path, w = width, h = height]() mutable {
                    try {
                        for (size_t i = 0; i < data.size(); i += 4)
                            std::swap(data[i], data[i + 2]);
                        if (outputPath.empty())
                            pixels = std::move(data);
                        else {
                            auto*      image = cairo_image_surface_create_for_data(reinterpret_cast<unsigned char*>(data.data()), CAIRO_FORMAT_ARGB32, w, h, w * 4);
                            const auto result =
                                cairo_surface_status(image) == CAIRO_STATUS_SUCCESS ? cairo_surface_write_to_png(image, outputPath.c_str()) : cairo_surface_status(image);
                            cairo_surface_destroy(image);
                            if (result != CAIRO_STATUS_SUCCESS)
                                error = cairo_status_to_string(result);
                        }
                    } catch (const std::exception& exception) { error = exception.what(); }
                    done.store(true);
                });
            } catch (const std::exception& exception) {
                error = exception.what();
                done.store(true);
            }
        }

        std::string status(uint64_t requestedID) {
            if (!active || requestedID != id)
                return "error: screenshot ID is unavailable";
            progress();
            if (fence || !done.load())
                return json{{"state", "pending"}}.dump();
            if (!error.empty())
                return json{{"state", "failed"}, {"error", error}}.dump();
            return json{{"state", "ready"}, {"width", width}, {"height", height}, {"format", "BGRA"}, {"size", pixels.size()}, {"path", path}}.dump();
        }

        std::string read(uint64_t requestedID, size_t offset, size_t length) {
            if (!active || requestedID != id || fence || !done.load() || !error.empty() || !path.empty())
                return "error: screenshot pixels are unavailable";
            if (!length || length > 65536 || offset > pixels.size() || length > pixels.size() - offset)
                return "error: screenshot read is out of bounds";
            return "data:" + pixels.substr(offset, length);
        }

        std::string release(uint64_t requestedID) {
            if (!active || requestedID != id)
                return "error: screenshot ID is unavailable";
            cancel();
            return "ok";
        }
    };

    Job::Job() : state(std::make_unique<State>()) {}
    Job::~Job() = default;
    std::string Job::start(PHLWINDOW window, const std::string& path) {
        return state->start(window, path);
    }
    std::string Job::status(uint64_t id) {
        return state->status(id);
    }
    std::string Job::read(uint64_t id, size_t offset, size_t length) {
        return state->read(id, offset, length);
    }
    std::string Job::release(uint64_t id) {
        return state->release(id);
    }
    void Job::cancel() {
        state->cancel();
    }
}
