/****************************************************************************
**
** Copyright (C) 2013 Digia Plc and/or its subsidiary(-ies).
** Contact: http://www.qt-project.org/legal
**
** Based on qeglfshooks_stub.cpp, part of the plugins of the Qt Toolkit.
**
** Copyright (C) 2013 Jolla Ltd.
** Contact: Thomas Perl <thomas.perl@jolla.com>
**
** This file is part of the hwcomposer plugin.
**
** $QT_BEGIN_LICENSE:LGPL$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and Digia.  For licensing terms and
** conditions see http://qt.digia.com/licensing.  For further information
** use the contact form at http://qt.digia.com/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 2.1 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL included in the
** packaging of this file.  Please review the following information to
** ensure the GNU Lesser General Public License version 2.1 requirements
** will be met: http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** In addition, as a special exception, Digia gives you certain additional
** rights.  These rights are described in the Digia Qt LGPL Exception
** version 1.1, included in the file LGPL_EXCEPTION.txt in this package.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3.0 as published by the Free Software
** Foundation and appearing in the file LICENSE.GPL included in the
** packaging of this file.  Please review the following information to
** ensure the GNU General Public License version 3.0 requirements will be
** met: http://www.gnu.org/copyleft/gpl.html.
**
**
** $QT_END_LICENSE$
**
****************************************************************************/

#include "hwcomposer_context.h"

#include "qeglfscontext.h"
#include "hwcomposer_screeninfo.h"
#include "hwcomposer_backend.h"

#include "qeglfsscreen.h"
#include "qeglfswindow.h"

#include <qcoreapplication.h>
#include <QtCore/QFile>
#include <QtCore/QTimer>
#include <QtGui/QGuiApplication>
#include <QtGui/QWindow>
#include <QtGui/QScreen>
#include <qpa/qwindowsysteminterface.h>

QT_BEGIN_NAMESPACE

HwComposerContext::HwComposerContext()
    : info(NULL)
    , backend(NULL)
    , display_off(false)
    , window_created(false)
    , fps(0)
    , m_switchEnabled(false)
    , m_requestedDisplay(0)
    , m_activeDisplay(0)
    , m_defaultRotation(0)
    , m_switchTimer(NULL)
    , m_screen(NULL)
{
    bool ok = false;
    int rotation = qEnvironmentVariableIntValue("QPA_HWC_ROTATION", &ok);
    if (ok && (rotation == 0 || rotation == 90 || rotation == 180 || rotation == 270))
        m_defaultRotation = rotation;
    else if (ok)
        qWarning("Ignoring invalid QPA_HWC_ROTATION=%d (use 0, 90, 180 or 270)", rotation);

    // Picks the initial display (sets QPA_HWC_DISPLAY_ID for the backend).
    setupDisplaySwitching();

    // This actually opens the hwcomposer device
    backend = HwComposerBackend::create();
    HWC_PLUGIN_ASSERT_NOT_NULL(backend);

    fps = backend->refreshRate();

    info = new HwComposerScreenInfo(backend);

    if (m_switchEnabled) {
        // Look up every display now, on the GUI thread; later lookups
        // (some from the render thread) are then read-only.
        for (const SwitchTarget &t : m_switchTargets) {
            int w = 0, h = 0;
            float pw = 0, ph = 0;
            if (backend->getScreenSizesFor(t.display, &w, &h, &pw, &ph)) {
                DisplayInfo di;
                di.size = QSize(w, h);
                di.physicalSize = QSizeF(pw, ph);
                m_displayInfo.insert(t.display, di);
                qDebug("Display switching: display %llu is %dx%d, rotation %d",
                       t.display, w, h, t.rotation);
            } else {
                qWarning("Display switching: display %llu unavailable, disabling switching",
                         t.display);
                m_switchEnabled = false;
            }
        }
    }

    if (m_switchEnabled) {
        m_switchTimer = new QTimer;
        m_switchTimer->setInterval(qBound(50, qEnvironmentVariableIntValue("QPA_HWC_DISPLAY_SWITCH_INTERVAL", &ok), 5000));
        if (!ok)
            m_switchTimer->setInterval(250);
        QObject::connect(m_switchTimer, &QTimer::timeout, [this]() { pollDisplaySwitch(); });
        m_switchTimer->start();
    }

    if (rotationFor(activeDisplay()))
        qDebug("Rotating output by %d degrees", rotationFor(activeDisplay()));
}

void HwComposerContext::setupDisplaySwitching()
{
    m_switchFile = qgetenv("QPA_HWC_DISPLAY_SWITCH_FILE");
    const QByteArray spec = qgetenv("QPA_HWC_DISPLAY_SWITCH");

    bool ok = false;
    int initial = qEnvironmentVariableIntValue("QPA_HWC_DISPLAY_ID", &ok);
    if (!ok || initial < 0)
        initial = 0;

    if (!m_switchFile.isEmpty() && !spec.isEmpty()) {
        for (const QByteArray &entry : spec.split(',')) {
            // value=display@rotation
            const int eq = entry.indexOf('=');
            if (eq <= 0) {
                qWarning("QPA_HWC_DISPLAY_SWITCH: ignoring '%s'", entry.constData());
                continue;
            }
            SwitchTarget t;
            t.value = entry.left(eq).trimmed();
            QByteArray rest = entry.mid(eq + 1).trimmed();
            t.rotation = 0;
            const int at = rest.indexOf('@');
            if (at >= 0) {
                t.rotation = rest.mid(at + 1).toInt();
                rest = rest.left(at);
            }
            bool idOk = false;
            t.display = rest.toULongLong(&idOk);
            if (!idOk || (t.rotation != 0 && t.rotation != 90 && t.rotation != 180 && t.rotation != 270)) {
                qWarning("QPA_HWC_DISPLAY_SWITCH: ignoring '%s'", entry.constData());
                continue;
            }
            m_switchTargets.append(t);
        }
        m_switchEnabled = !m_switchTargets.isEmpty();
    }

    if (m_switchEnabled) {
        QFile f(QString::fromLocal8Bit(m_switchFile));
        if (f.open(QIODevice::ReadOnly)) {
            const QByteArray value = f.readAll().trimmed();
            for (const SwitchTarget &t : m_switchTargets) {
                if (t.value == value) {
                    initial = int(t.display);
                    break;
                }
            }
            qDebug("Display switching: %s is '%s', starting on display %d",
                   m_switchFile.constData(), value.constData(), initial);
        } else {
            qWarning("Display switching: cannot read %s", m_switchFile.constData());
        }
        // The backend picks its display from QPA_HWC_DISPLAY_ID.
        qputenv("QPA_HWC_DISPLAY_ID", QByteArray::number(initial));
    }

    m_requestedDisplay.store(initial);
    m_activeDisplay.store(initial);
}

int HwComposerContext::rotationFor(quint64 display) const
{
    for (const SwitchTarget &t : m_switchTargets) {
        if (t.display == display)
            return t.rotation;
    }
    return m_defaultRotation;
}

HwComposerContext::DisplayInfo HwComposerContext::infoFor(quint64 display) const
{
    if (m_switchEnabled && m_displayInfo.contains(display))
        return m_displayInfo.value(display);
    DisplayInfo di;
    di.size = info->screenSize();
    di.physicalSize = info->physicalScreenSize();
    return di;
}

void HwComposerContext::pollDisplaySwitch()
{
    QFile f(QString::fromLocal8Bit(m_switchFile));
    if (!f.open(QIODevice::ReadOnly))
        return;
    const QByteArray value = f.readAll().trimmed();

    const SwitchTarget *target = NULL;
    for (const SwitchTarget &t : m_switchTargets) {
        if (t.value == value) {
            target = &t;
            break;
        }
    }
    if (!target || target->display == quint64(m_requestedDisplay.load()))
        return;

    qDebug("Display switching: %s is '%s', switching to display %llu",
           m_switchFile.constData(), value.constData(), target->display);
    m_requestedDisplay.store(int(target->display));

    // Tell Qt about the new screen; the render thread switches the surface
    // at the start of its next frame (applyPendingDisplaySwitch()).
    if (m_screen && m_screen->screen()) {
        const QRect geometry(QPoint(0, 0), logicalScreenSize());
        QWindowSystemInterface::handleScreenGeometryChange(m_screen->screen(), geometry, geometry);
        const QDpi dpi = m_screen->logicalDpi();
        QWindowSystemInterface::handleScreenLogicalDotsPerInchChange(m_screen->screen(), dpi.first, dpi.second);
    }
    for (QWindow *w : QGuiApplication::allWindows()) {
        if (w->handle())
            static_cast<QEglFSWindow *>(w->handle())->setGeometry(QRect());
        w->requestUpdate();
    }
}

void HwComposerContext::applyPendingDisplaySwitch()
{
    if (!m_switchEnabled)
        return;
    const int requested = m_requestedDisplay.load();
    if (requested == m_activeDisplay.load())
        return;
    if (backend->switchDisplay(quint64(requested))) {
        m_activeDisplay.store(requested);
        qDebug("Display switching: now rendering to display %d (rotation %d)",
               requested, rotationFor(quint64(requested)));
    } else {
        qWarning("Display switching: switching to display %d failed", requested);
    }
}

EGLNativeWindowType HwComposerContext::createNativeWindowFor(quint64 display)
{
    const QSize size = infoFor(display).size;
    return backend->createWindowFor(display, size.width(), size.height());
}

HwComposerContext::~HwComposerContext()
{
    delete m_switchTimer;

    // Properly clean up hwcomposer backend
    HwComposerBackend::destroy(backend);

    // Free framebuffer device parameters info
    delete info;
}

EGLNativeDisplayType HwComposerContext::platformDisplay() const
{
    return backend->display();
}

QSizeF HwComposerContext::physicalScreenSize() const
{
    // Of the display Qt is laid out for
    return infoFor(quint64(m_requestedDisplay.load())).physicalSize;
}

int HwComposerContext::screenDepth() const
{
    return info->screenDepth();
}

QSize HwComposerContext::screenSize() const
{
    // Native size of the display currently rendered to
    return infoFor(activeDisplay()).size;
}

int HwComposerContext::displayRotation() const
{
    return rotationFor(activeDisplay());
}

QSize HwComposerContext::logicalScreenSize() const
{
    const quint64 display = quint64(m_requestedDisplay.load());
    const QSize size = infoFor(display).size;
    return (rotationFor(display) % 180) ? size.transposed() : size;
}

QSizeF HwComposerContext::logicalPhysicalScreenSize() const
{
    const quint64 display = quint64(m_requestedDisplay.load());
    const QSizeF size = infoFor(display).physicalSize;
    return (rotationFor(display) % 180) ? size.transposed() : size;
}

QSurfaceFormat HwComposerContext::surfaceFormatFor(const QSurfaceFormat &inputFormat) const
{
    QSurfaceFormat newFormat = inputFormat; 
    if (screenDepth() == 16) {
        newFormat.setRedBufferSize(5);
        newFormat.setGreenBufferSize(6);
        newFormat.setBlueBufferSize(5);
    } else {
        newFormat.setStencilBufferSize(8);
        newFormat.setAlphaBufferSize(8);
        newFormat.setRedBufferSize(8);
        newFormat.setGreenBufferSize(8);
        newFormat.setBlueBufferSize(8);
    }

    return newFormat;
}

EGLNativeWindowType HwComposerContext::createNativeWindow(const QSurfaceFormat &format)
{
    Q_UNUSED(format);

    if (window_created) {
        HWC_PLUGIN_FATAL("There can only be one window, someone tried to create more.");
    }

    window_created = true;
    QSize size = screenSize();
    return backend->createWindow(size.width(), size.height());
}

void HwComposerContext::destroyNativeWindow(EGLNativeWindowType window)
{
    return backend->destroyWindow(window);
}

void HwComposerContext::swapToWindow(QEglFSContext *context, QPlatformSurface *surface)
{
    if (display_off) {
        qWarning("Swap requested while display is off");
        return;
    }

    EGLDisplay egl_display = context->eglDisplay();
    EGLSurface egl_surface = context->eglSurfaceForPlatformSurface(surface);
    return backend->swap(egl_display, egl_surface);
}

void HwComposerContext::sleepDisplay(bool sleep)
{
    if (sleep) {
        qDebug("sleepDisplay");
        display_off = true;
    } else {
        qDebug("unsleepDisplay");
        display_off = false;
        // A fold/unfold while the display was off: switch before powering
        // on, so the previous display doesn't light up first. Nothing is
        // being rendered while the display is off.
        if (m_switchEnabled && m_requestedDisplay.load() != m_activeDisplay.load()) {
            const int requested = m_requestedDisplay.load();
            if (backend->switchDisplay(quint64(requested))) {
                m_activeDisplay.store(requested);
                qDebug("Display switching: switched to display %d while asleep", requested);
            }
        }
    }

    backend->sleepDisplay(sleep);
}

qreal HwComposerContext::refreshRate() const
{
    return fps;
}

bool HwComposerContext::requestUpdate(QEglFSWindow *window)
{
    if (backend)
        return backend->requestUpdate(window);
    return false;
}

QT_END_NAMESPACE
