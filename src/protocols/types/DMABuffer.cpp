#include "DMABuffer.hpp"
#include "WLBuffer.hpp"
#include "../../desktop/view/LayerSurface.hpp"
#include "../../render/Renderer.hpp"
#include "../../helpers/Format.hpp"
#include "../../Compositor.hpp"
#include "../../render/OpenGL.hpp"
#include "../../render/Texture.hpp"
#include <stdlib.h>

#if defined(__linux__)
#include <linux/dma-buf.h>
#include <linux/sync_file.h>
#endif
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <xf86drm.h>
#include <drm_fourcc.h>
#include <libdrm/drm_mode.h>

using namespace Hyprutils::OS;

CDMABuffer::CDMABuffer(uint32_t id, wl_client* client, Aquamarine::SDMABUFAttrs const& attrs_) : m_attrs(attrs_) {
    g_pHyprRenderer->makeEGLCurrent();

    m_listeners.resourceDestroy = events.destroy.listen([this] {
        closeFDs();
        m_listeners.resourceDestroy.reset();
    });

    size          = m_attrs.size;
    m_resource    = CWLBufferResource::create(makeShared<CWlBuffer>(client, 1, id));

    // For cross-GPU buffers, we need special handling since the buffer
    // was created on a different GPU than the compositor's primary
    const bool enableCPUFallback  = getenv("HYPRLAND_DMABUF_ENABLE_CPU_FALLBACK") != NULL;
    const bool disableCPUFallback = getenv("HYPRLAND_DMABUF_DISABLE_CPU_FALLBACK") != NULL;
    const bool allowCPUFallback   = enableCPUFallback && !disableCPUFallback;
    const bool logDMABUF          = getenv("HYPRLAND_DMABUF_LOG") != NULL;
    if (m_attrs.crossGPU && g_pCompositor->m_secondaryDrmRenderNode.available && allowCPUFallback) {
        Debug::log(LOG, "CDMABuffer: Cross-GPU buffer detected, using CPU copy fallback (opt-in)");

        // For cross-GPU, try CPU copy path:
        // 1. Map the dmabuf via mmap (dmabufs can often be mmap'd directly)
        // 2. Create texture with glTexImage2D
        // This is slower but works across different GPU vendors

        if (!createCrossGPUTexture()) {
            Debug::log(ERR, "CDMABuffer: Cross-GPU fallback failed, trying EGLImage anyway");
            // Fall through to try EGLImage as last resort
        } else {
            m_opaque  = NFormatUtils::isFormatOpaque(m_attrs.format);
            m_success = m_texture && m_texture->m_texID;
            if (m_success) {
                Debug::log(LOG, "CDMABuffer: Cross-GPU texture created successfully via CPU copy");
                return;
            }
        }
    } else if (m_attrs.crossGPU && g_pCompositor->m_secondaryDrmRenderNode.available && !allowCPUFallback) {
        Debug::log(LOG, "CDMABuffer: Cross-GPU buffer detected, CPU fallback disabled; trying EGL import");
    }

    auto eglImage = g_pHyprOpenGL->createEGLImage(m_attrs);

    if UNLIKELY (!eglImage) {
        if (logDMABUF && m_attrs.modifier != DRM_FORMAT_MOD_INVALID) {
            Debug::log(WARN, "CDMABuffer: dmabuf import failed with modifier {} (0x{:x}), retrying without modifier",
                       NFormatUtils::drmModifierName(m_attrs.modifier), sc<uint64_t>(m_attrs.modifier));
        }
        Debug::log(ERR, "CDMABuffer: failed to import EGLImage, retrying as implicit");
        m_attrs.modifier = DRM_FORMAT_MOD_INVALID;
        eglImage         = g_pHyprOpenGL->createEGLImage(m_attrs);

        if UNLIKELY (!eglImage) {
            Debug::log(ERR, "CDMABuffer: failed to import EGLImage");
            if (logDMABUF)
                Debug::log(ERR, "CDMABuffer: dmabuf import failed even without modifier");
            return;
        }
    }

    m_texture = makeShared<CTexture>(m_attrs, eglImage); // texture takes ownership of the eglImage
    m_opaque  = NFormatUtils::isFormatOpaque(m_attrs.format);
    m_success = m_texture->m_texID;

    if UNLIKELY (!m_success)
        Debug::log(ERR, "Failed to create a dmabuf: texture is null");
}

CDMABuffer::~CDMABuffer() {
    if (m_resource)
        m_resource->sendRelease();

    closeFDs();
}

Aquamarine::eBufferCapability CDMABuffer::caps() {
    return Aquamarine::eBufferCapability::BUFFER_CAPABILITY_DATAPTR;
}

Aquamarine::eBufferType CDMABuffer::type() {
    return Aquamarine::eBufferType::BUFFER_TYPE_DMABUF;
}

void CDMABuffer::update(const CRegion& damage) {
    ;
}

bool CDMABuffer::isSynchronous() {
    return false;
}

Aquamarine::SDMABUFAttrs CDMABuffer::dmabuf() {
    return m_attrs;
}

std::tuple<uint8_t*, uint32_t, size_t> CDMABuffer::beginDataPtr(uint32_t flags) {
    // FIXME:
    return {nullptr, 0, 0};
}

void CDMABuffer::endDataPtr() {
    // FIXME:
}

bool CDMABuffer::good() {
    return m_success;
}

void CDMABuffer::closeFDs() {
    for (int i = 0; i < m_attrs.planes; ++i) {
        if (m_attrs.fds[i] == -1)
            continue;
        close(m_attrs.fds[i]);
        m_attrs.fds[i] = -1;
    }
    m_attrs.planes = 0;
}

static int doIoctl(int fd, unsigned long request, void* arg) {
    int ret;

    do {
        ret = ioctl(fd, request, arg);
    } while (ret == -1 && (errno == EINTR || errno == EAGAIN));
    return ret;
}

// https://www.kernel.org/doc/html/latest/driver-api/dma-buf.html#c.dma_buf_export_sync_file
// returns a sync file that will be signalled when dmabuf is ready to be read
CFileDescriptor CDMABuffer::exportSyncFile() {
    if (!good())
        return {};

#if !defined(__linux__)
    return {};
#else
    std::vector<CFileDescriptor> syncFds;
    syncFds.reserve(m_attrs.fds.size());

    for (const auto& fd : m_attrs.fds) {
        if (fd == -1)
            continue;

        // buffer readability checks are rather slow on some Intel laptops
        // see https://gitlab.freedesktop.org/drm/intel/-/issues/9415
        if (g_pHyprRenderer && !g_pHyprRenderer->isIntel()) {
            if (CFileDescriptor::isReadable(fd))
                continue;
        }

        dma_buf_export_sync_file request{
            .flags = DMA_BUF_SYNC_READ,
            .fd    = -1,
        };

        if (doIoctl(fd, DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &request) == 0)
            syncFds.emplace_back(request.fd);
    }

    if (syncFds.empty())
        return {};

    CFileDescriptor syncFd;
    for (auto& fd : syncFds) {
        if (!syncFd.isValid()) {
            syncFd = std::move(fd);
            continue;
        }

        const std::string      name = "merged release fence";
        struct sync_merge_data data{
            .name  = {}, // zero-initialize name[]
            .fd2   = fd.get(),
            .fence = -1,
        };

        std::ranges::copy_n(name.c_str(), std::min(name.size() + 1, sizeof(data.name)), data.name);

        if (doIoctl(syncFd.get(), SYNC_IOC_MERGE, &data) == 0)
            syncFd = CFileDescriptor(data.fence);
        else
            syncFd = {};
    }

    return syncFd;
#endif
}

// Cross-GPU texture creation via CPU copy
// This is used when a buffer was created on a different GPU (e.g., Intel)
// than the compositor's primary GPU (e.g., AMD)
bool CDMABuffer::createCrossGPUTexture() {
#if !defined(__linux__)
    return false;
#else
    if (m_attrs.planes != 1) {
        Debug::log(ERR, "Cross-GPU: Multi-plane buffers not yet supported");
        return false;
    }

    // Calculate buffer size based on stride and height
    // For simplicity, we assume a simple linear layout
    const size_t bufferSize = m_attrs.strides[0] * static_cast<size_t>(m_attrs.size.y);
    if (bufferSize == 0) {
        Debug::log(ERR, "Cross-GPU: Invalid buffer size");
        return false;
    }

    // Try to mmap the dmabuf fd directly
    // This works for many drivers when the buffer uses a linear modifier
    void* mapped = mmap(nullptr, bufferSize, PROT_READ, MAP_SHARED, m_attrs.fds[0], m_attrs.offsets[0]);
    if (mapped == MAP_FAILED) {
        Debug::log(ERR, "Cross-GPU: Failed to mmap dmabuf fd (errno: {}), trying DRM handle path", errno);

        // Fallback: try to map via DRM handle on the secondary device
        if (m_attrs.sourceDevice < 0) {
            Debug::log(ERR, "Cross-GPU: No source device available for DRM handle mapping");
            return false;
        }

        uint32_t handle = 0;
        if (drmPrimeFDToHandle(m_attrs.sourceDevice, m_attrs.fds[0], &handle) != 0) {
            Debug::log(ERR, "Cross-GPU: drmPrimeFDToHandle failed");
            return false;
        }

        // For DRM dumb buffers, we can use MODE_MAP_DUMB
        // But for GPU-rendered buffers, this may not work
        // This is a best-effort fallback
        struct drm_mode_map_dumb mapReq = {
            .handle = handle,
            .pad = 0,
            .offset = 0,
        };

        if (drmIoctl(m_attrs.sourceDevice, DRM_IOCTL_MODE_MAP_DUMB, &mapReq) != 0) {
            Debug::log(ERR, "Cross-GPU: DRM_IOCTL_MODE_MAP_DUMB failed");
            drmCloseBufferHandle(m_attrs.sourceDevice, handle);
            return false;
        }

        mapped = mmap(nullptr, bufferSize, PROT_READ, MAP_SHARED, m_attrs.sourceDevice, mapReq.offset);
        drmCloseBufferHandle(m_attrs.sourceDevice, handle);

        if (mapped == MAP_FAILED) {
            Debug::log(ERR, "Cross-GPU: mmap via DRM handle failed");
            return false;
        }
    }

    // Determine GL format from DRM format
    GLenum glFormat = GL_RGBA;
    GLenum glType = GL_UNSIGNED_BYTE;
    int    bpp = 4;

    switch (m_attrs.format) {
        case DRM_FORMAT_ARGB8888:
        case DRM_FORMAT_XRGB8888:
            glFormat = GL_BGRA_EXT;
            glType = GL_UNSIGNED_BYTE;
            bpp = 4;
            break;
        case DRM_FORMAT_ABGR8888:
        case DRM_FORMAT_XBGR8888:
            glFormat = GL_RGBA;
            glType = GL_UNSIGNED_BYTE;
            bpp = 4;
            break;
        case DRM_FORMAT_RGB888:
            glFormat = GL_RGB;
            glType = GL_UNSIGNED_BYTE;
            bpp = 3;
            break;
        case DRM_FORMAT_BGR888:
            // BGR888 is not directly supported in GLES, would need swizzling
            // Fall through to unsupported for now
            Debug::log(ERR, "Cross-GPU: BGR888 format not supported in GLES");
            munmap(mapped, bufferSize);
            return false;
        default:
            Debug::log(ERR, "Cross-GPU: Unsupported DRM format 0x{:x}", m_attrs.format);
            munmap(mapped, bufferSize);
            return false;
    }

    // Create GL texture
    GLuint texID = 0;
    glGenTextures(1, &texID);
    if (texID == 0) {
        Debug::log(ERR, "Cross-GPU: glGenTextures failed");
        munmap(mapped, bufferSize);
        return false;
    }

    glBindTexture(GL_TEXTURE_2D, texID);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // Handle stride (row alignment)
    const int expectedStride = static_cast<int>(m_attrs.size.x) * bpp;
    if (m_attrs.strides[0] != static_cast<uint32_t>(expectedStride)) {
        glPixelStorei(GL_UNPACK_ROW_LENGTH, m_attrs.strides[0] / bpp);
    }

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, static_cast<GLsizei>(m_attrs.size.x), static_cast<GLsizei>(m_attrs.size.y),
                 0, glFormat, glType, mapped);

    // Reset row length
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glBindTexture(GL_TEXTURE_2D, 0);

    munmap(mapped, bufferSize);

    // Check for GL errors
    GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
        Debug::log(ERR, "Cross-GPU: GL error after texture upload: 0x{:x}", err);
        glDeleteTextures(1, &texID);
        return false;
    }

    // Create texture wrapper
    m_texture = makeShared<CTexture>();
    m_texture->m_texID = texID;
    m_texture->m_size = m_attrs.size;
    m_texture->m_target = GL_TEXTURE_2D;
    m_texture->m_isSynchronous = true; // CPU-copied textures are synchronous

    // Set texture type based on format (with/without alpha)
    switch (m_attrs.format) {
        case DRM_FORMAT_XRGB8888:
        case DRM_FORMAT_XBGR8888:
        case DRM_FORMAT_RGB888:
            m_texture->m_type = TEXTURE_RGBX;
            break;
        default:
            m_texture->m_type = TEXTURE_RGBA;
            break;
    }

    Debug::log(LOG, "Cross-GPU: Created texture {} ({}x{}) via CPU copy", texID, m_attrs.size.x, m_attrs.size.y);
    return true;
#endif
}
