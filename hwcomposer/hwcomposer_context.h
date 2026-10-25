/****************************************************************************
**
** Copyright (C) 2013 Digia Plc and/or its subsidiary(-ies).
** Contact: http://www.qt-project.org/legal
**
** Based on qeglfshooks.h, part of the plugins of the Qt Toolkit.
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

#ifndef HWCOMPOSER_CONTEXT_H
#define HWCOMPOSER_CONTEXT_H

#include <QtGlobal>

#include <qpa/qplatformintegration.h>
#include <qpa/qplatformscreen.h>
#include <QtGui/QSurfaceFormat>
#include <QtGui/QImage>
#include <EGL/egl.h>

#if (QT_VERSION >= QT_VERSION_CHECK(6, 0, 0))
#include <QtGui/private/qeglplatformcontext_p.h>
#elif (QT_VERSION >= QT_VERSION_CHECK(5, 8, 0))
#include <QtEglSupport/private/qeglplatformcontext_p.h>
#else
#include <QtPlatformSupport/private/qeglplatformcontext_p.h>
#endif

#include <QtCore/QAtomicInt>
#include <QtCore/QHash>
#include <QtCore/QList>
#include <QtCore/QByteArray>

QT_BEGIN_NAMESPACE

class QEglFSContext;
class QEglFSWindow;
class QEglFSScreen;
class HwComposerScreenInfo;
class HwComposerBackend;
class QTimer;

class HwComposerContext
{
public:
    HwComposerContext();
    ~HwComposerContext();

    QSizeF physicalScreenSize() const;
    QSize screenSize() const;
    int screenDepth() const;

    // Clockwise rotation (0, 90, 180, 270) applied to the rendered UI before
    // it reaches the panel of the display currently rendered to, from
    // QPA_HWC_ROTATION (or QPA_HWC_DISPLAY_SWITCH). For panels mounted
    // rotated relative to the device (e.g. the Pixel 9 Pro Fold inner display).
    int displayRotation() const;
    // Screen size/physical size as seen by Qt, i.e. of the requested display
    // after rotation.
    QSize logicalScreenSize() const;
    QSizeF logicalPhysicalScreenSize() const;

    // Runtime switching between built-in displays, e.g. on foldables:
    //   QPA_HWC_DISPLAY_SWITCH_FILE=/sys/devices/platform/hall_sensor/state
    //   QPA_HWC_DISPLAY_SWITCH=1=0@90,0=1@0   (file value=display@rotation)
    //   QPA_HWC_DISPLAY_SWITCH_INTERVAL=250   (poll interval in ms)
    // Qt keeps one window; the plugin keeps a native window/EGL surface per
    // display and renders through an FBO, so a switch only changes the
    // screen geometry Qt sees and the surface the frame is blitted to.
    bool displaySwitching() const { return m_switchEnabled; }
    quint64 activeDisplay() const { return quint64(m_activeDisplay.load()); }
    // Called on the render thread at the start of a frame.
    void applyPendingDisplaySwitch();
    EGLNativeWindowType createNativeWindowFor(quint64 display);
    void setScreen(QEglFSScreen *screen) { m_screen = screen; }

    QSurfaceFormat surfaceFormatFor(const QSurfaceFormat &inputFormat) const;

    EGLNativeDisplayType platformDisplay() const;
    EGLNativeWindowType createNativeWindow(const QSurfaceFormat &format);
    void destroyNativeWindow(EGLNativeWindowType window);

    void swapToWindow(QEglFSContext *context, QPlatformSurface *surface);

    void sleepDisplay(bool sleep);
    qreal refreshRate() const;

    bool requestUpdate(QEglFSWindow *window);

private:
    struct SwitchTarget {
        QByteArray value;
        quint64 display;
        int rotation;
    };
    struct DisplayInfo {
        QSize size;
        QSizeF physicalSize;
    };

    void setupDisplaySwitching();
    void pollDisplaySwitch();
    int rotationFor(quint64 display) const;
    DisplayInfo infoFor(quint64 display) const;

    HwComposerScreenInfo *info;
    HwComposerBackend *backend;
    bool display_off;
    bool window_created;
    qreal fps;

    bool m_switchEnabled;
    QByteArray m_switchFile;
    QList<SwitchTarget> m_switchTargets;
    QHash<quint64, DisplayInfo> m_displayInfo;
    QAtomicInt m_requestedDisplay;
    QAtomicInt m_activeDisplay;
    int m_defaultRotation;
    QTimer *m_switchTimer;
    QEglFSScreen *m_screen;
};

QT_END_NAMESPACE

#endif // HWCOMPOSER_CONTEXT_H
