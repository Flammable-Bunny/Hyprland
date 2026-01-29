#include "Subsurface.hpp"
#include <cstdlib>
#include "../state/FocusState.hpp"
#include "Window.hpp"
#include "../../config/ConfigValue.hpp"
#include "../../protocols/core/Compositor.hpp"
#include "../../protocols/core/Subcompositor.hpp"
#include "../../render/Renderer.hpp"
#include "../../managers/input/InputManager.hpp"

using namespace Desktop;
using namespace Desktop::View;

SP<CSubsurface> CSubsurface::create(PHLWINDOW pOwner) {
    auto subsurface            = SP<CSubsurface>(new CSubsurface());
    subsurface->m_windowParent = pOwner;
    subsurface->m_self         = subsurface;

    subsurface->initSignals();
    subsurface->initExistingSubsurfaces(pOwner->wlSurface()->resource());
    return subsurface;
}

SP<CSubsurface> CSubsurface::create(WP<Desktop::View::CPopup> pOwner) {
    auto subsurface           = SP<CSubsurface>(new CSubsurface());
    subsurface->m_popupParent = pOwner;
    subsurface->m_self        = subsurface;
    subsurface->initSignals();
    subsurface->initExistingSubsurfaces(pOwner->wlSurface()->resource());
    return subsurface;
}

SP<CSubsurface> CSubsurface::create(SP<CWLSubsurfaceResource> pSubsurface, PHLWINDOW pOwner) {
    auto subsurface            = SP<CSubsurface>(new CSubsurface());
    subsurface->m_windowParent = pOwner;
    subsurface->m_subsurface   = pSubsurface;
    subsurface->m_self         = subsurface;
    subsurface->wlSurface()    = CWLSurface::create();
    subsurface->wlSurface()->assign(pSubsurface->m_surface.lock(), subsurface);
    subsurface->initSignals();
    subsurface->initExistingSubsurfaces(pSubsurface->m_surface.lock());
    return subsurface;
}

SP<CSubsurface> CSubsurface::create(SP<CWLSubsurfaceResource> pSubsurface, WP<Desktop::View::CPopup> pOwner) {
    auto subsurface           = SP<CSubsurface>(new CSubsurface());
    subsurface->m_popupParent = pOwner;
    subsurface->m_subsurface  = pSubsurface;
    subsurface->m_self        = subsurface;
    subsurface->wlSurface()   = CWLSurface::create();
    subsurface->wlSurface()->assign(pSubsurface->m_surface.lock(), subsurface);
    subsurface->initSignals();
    subsurface->initExistingSubsurfaces(pSubsurface->m_surface.lock());
    return subsurface;
}

SP<CSubsurface> CSubsurface::fromView(SP<IView> v) {
    if (!v || v->type() != VIEW_TYPE_SUBSURFACE)
        return nullptr;
    return dynamicPointerCast<CSubsurface>(v);
}

CSubsurface::CSubsurface() : IView(CWLSurface::create()) {
    ;
}

eViewType CSubsurface::type() const {
    return VIEW_TYPE_SUBSURFACE;
}

bool CSubsurface::visible() const {
    if (!m_wlSurface || !m_wlSurface->resource() || !m_wlSurface->resource()->m_mapped)
        return false;

    if (!m_windowParent.expired())
        return g_pHyprRenderer->shouldRenderWindow(m_windowParent.lock());
    if (m_popupParent)
        return m_popupParent->visible();
    if (m_parent)
        return m_parent->visible();

    return false;
}

bool CSubsurface::desktopComponent() const {
    return true;
}

std::optional<CBox> CSubsurface::logicalBox() const {
    return surfaceLogicalBox();
}

std::optional<CBox> CSubsurface::surfaceLogicalBox() const {
    if (!visible())
        return std::nullopt;

    return CBox{coordsGlobal(), m_lastSize};
}

void CSubsurface::initSignals() {
    if (m_subsurface) {
        m_listeners.commitSubsurface  = m_subsurface->m_surface->m_events.commit.listen([this] { onCommit(); });
        m_listeners.destroySubsurface = m_subsurface->m_events.destroy.listen([this] { onDestroy(); });
        m_listeners.mapSubsurface     = m_subsurface->m_surface->m_events.map.listen([this] { onMap(); });
        m_listeners.unmapSubsurface   = m_subsurface->m_surface->m_events.unmap.listen([this] { onUnmap(); });
        m_listeners.newSubsurface     = m_subsurface->m_surface->m_events.newSubsurface.listen([this](const auto& resource) { onNewSubsurface(resource); });
    } else {
        if (m_windowParent)
            m_listeners.newSubsurface = m_windowParent->wlSurface()->resource()->m_events.newSubsurface.listen([this](const auto& resource) { onNewSubsurface(resource); });
        else if (m_popupParent)
            m_listeners.newSubsurface = m_popupParent->wlSurface()->resource()->m_events.newSubsurface.listen([this](const auto& resource) { onNewSubsurface(resource); });
        else
            ASSERT(false);
    }
}

void CSubsurface::checkSiblingDamage() {
    if (!m_parent)
        return; // ??????????

    const double SCALE = m_windowParent.lock() && m_windowParent->m_isX11 ? 1.0 / m_windowParent->m_X11SurfaceScaledBy : 1.0;

    for (auto const& n : m_parent->m_children) {
        if (n.get() == this)
            continue;

        const auto COORDS = n->coordsGlobal();
        g_pHyprRenderer->damageSurface(n->wlSurface()->resource(), COORDS.x, COORDS.y, SCALE);
    }
}

void CSubsurface::recheckDamageForSubsurfaces() {
    for (auto const& n : m_children) {
        const auto COORDS = n->coordsGlobal();
        g_pHyprRenderer->damageSurface(n->wlSurface()->resource(), COORDS.x, COORDS.y);
    }
}

void CSubsurface::onCommit() {
    // no damaging if it's not visible
    if (!m_windowParent.expired() && (!m_windowParent->m_isMapped || !m_windowParent->m_workspace->m_visible)) {
        m_lastSize = m_wlSurface->resource()->m_current.size;

        static auto PLOGDAMAGE = CConfigValue<Hyprlang::INT>("debug:log_damage");
        if (*PLOGDAMAGE)
            Log::logger->log(Log::DEBUG, "Refusing to commit damage from a subsurface of {} because it's invisible.", m_windowParent.lock());
        return;
    }

    const auto COORDS = coordsGlobal();

    g_pHyprRenderer->damageSurface(m_wlSurface->resource(), COORDS.x, COORDS.y);

    if (m_popupParent && !m_popupParent->inert() && m_popupParent->wlSurface())
        m_popupParent->recheckTree();
    if (!m_windowParent.expired()) // I hate you firefox why are you doing this
        m_windowParent->m_popupHead->recheckTree();

    // I do not think this is correct, but it solves a lot of issues with some apps (e.g. firefox)
    checkSiblingDamage();

    if (m_lastSize != m_wlSurface->resource()->m_current.size || m_lastPosition != m_subsurface->m_position) {
        damageLastArea();
        m_lastSize     = m_wlSurface->resource()->m_current.size;
        m_lastPosition = m_subsurface->m_position;
    }

    const auto TRACE_TEARING = []() {
        static int enabled = -1;
        if (enabled == -1)
            enabled = getenv("HYPRLAND_TRACE_TEARING") ? 1 : 0;
        return enabled == 1;
    };

    // tearing: allow subsurface commits to trigger immediate render for tearing candidates
    if (!m_windowParent.expired()) {
        const auto WINDOW   = m_windowParent.lock();
        const auto PMONITOR = WINDOW ? WINDOW->m_monitor.lock() : nullptr;
        if (PMONITOR && PMONITOR->m_solitaryClient.lock() == WINDOW && WINDOW->canBeTorn() && PMONITOR->m_tearingState.canTear && m_wlSurface->resource()->m_current.texture) {
            CRegion damageBox{m_wlSurface->resource()->m_current.accumulateBufferDamage()};

            if (!damageBox.empty()) {
                if (PMONITOR->m_tearingState.busy) {
                    PMONITOR->m_tearingState.frameScheduledWhileBusy = true;
                    if (TRACE_TEARING())
                        Log::logger->log(Log::INFO, "Tearing: subsurface commit busy, scheduling while busy for {}", WINDOW);
                } else {
                    PMONITOR->m_tearingState.nextRenderTorn = true;
                    if (TRACE_TEARING())
                        Log::logger->log(Log::INFO, "Tearing: subsurface commit immediate render for {}", WINDOW);
                    g_pHyprRenderer->renderMonitor(PMONITOR);
                }
            } else if (TRACE_TEARING()) {
                Log::logger->log(Log::INFO, "Tearing: subsurface commit skipped (no damage) for {}", WINDOW);
            }
        } else if (TRACE_TEARING()) {
            const bool solitary = PMONITOR && PMONITOR->m_solitaryClient.lock() == WINDOW;
            const bool canTear  = WINDOW && WINDOW->canBeTorn();
            const bool monTear  = PMONITOR && PMONITOR->m_tearingState.canTear;
            const bool texture  = m_wlSurface->resource()->m_current.texture != nullptr;
            Log::logger->log(Log::INFO,
                             "Tearing: subsurface commit blocked for {} (solitary={} canTear={} monitorCanTear={} hasTexture={})",
                             WINDOW, solitary, canTear, monTear, texture);
        }
    }
}

void CSubsurface::onDestroy() {
    // destroy children
    m_children.clear();

    m_inert = true;

    if (!m_subsurface)
        return; // dummy node, nothing to do, it's the parent dying

    // kill ourselves
    std::erase_if(m_parent->m_children, [this](const auto& other) { return other.get() == this; });
}

void CSubsurface::onNewSubsurface(SP<CWLSubsurfaceResource> pSubsurface) {
    WP<CSubsurface> PSUBSURFACE;

    if (!m_windowParent.expired())
        PSUBSURFACE = m_children.emplace_back(CSubsurface::create(pSubsurface, m_windowParent.lock()));
    else if (m_popupParent)
        PSUBSURFACE = m_children.emplace_back(CSubsurface::create(pSubsurface, m_popupParent));

    PSUBSURFACE->m_self = PSUBSURFACE;

    ASSERT(PSUBSURFACE);

    PSUBSURFACE->m_parent = m_self;
}

void CSubsurface::onMap() {
    m_lastSize     = m_wlSurface->resource()->m_current.size;
    m_lastPosition = m_subsurface->m_position;

    const auto COORDS = coordsGlobal();
    CBox       box{COORDS, m_lastSize};
    box.expand(4);
    g_pHyprRenderer->damageBox(box);

    if (!m_windowParent.expired())
        m_windowParent->updateSurfaceScaleTransformDetails();
}

void CSubsurface::onUnmap() {
    damageLastArea();

    if (m_wlSurface->resource() == Desktop::focusState()->surface())
        g_pInputManager->releaseAllMouseButtons();

    g_pInputManager->simulateMouseMovement();

    // TODO: should this remove children? Currently it won't, only on .destroy
}

void CSubsurface::damageLastArea() {
    const auto     COORDS = coordsGlobal() + m_lastPosition - m_subsurface->m_position;

    const Vector2D MAX_DAMAGE_SIZE = m_wlSurface && m_wlSurface->resource() ?
        Vector2D{
            std::max(m_lastSize.x, m_wlSurface->resource()->m_current.size.x),
            std::max(m_lastSize.y, m_wlSurface->resource()->m_current.size.y),
        } :
        m_lastSize;

    CBox           box{COORDS, m_lastSize};
    box.expand(4);
    g_pHyprRenderer->damageBox(box);
}

Vector2D CSubsurface::coordsRelativeToParent() const {
    if (!m_subsurface)
        return {};
    return m_subsurface->posRelativeToParent();
}

Vector2D CSubsurface::coordsGlobal() const {
    Vector2D coords = coordsRelativeToParent();

    if (!m_windowParent.expired())
        coords += m_windowParent->m_realPosition->value();
    else if (m_popupParent)
        coords += m_popupParent->coordsGlobal();

    return coords;
}

void CSubsurface::initExistingSubsurfaces(SP<CWLSurfaceResource> pSurface) {
    for (auto const& s : pSurface->m_subsurfaces) {
        if (!s || s->m_surface->m_hlSurface /* already assigned */)
            continue;
        onNewSubsurface(s.lock());
    }
}

Vector2D CSubsurface::size() {
    return m_wlSurface->resource()->m_current.size;
}
