/****************************************************************************
**
** Copyright (C) 2013 Digia Plc and/or its subsidiary(-ies).
** Contact: http://www.qt-project.org/legal
**
** This file is part of the plugins of the Qt Toolkit.
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

#include <QtGlobal>

#if (QT_VERSION >= QT_VERSION_CHECK(6, 0, 0))
#include <QtGui/private/qeglpbuffer_p.h>
#elif (QT_VERSION >= QT_VERSION_CHECK(5, 8, 0))
#include <QtEglSupport/private/qeglpbuffer_p.h>
#else
#include <QtPlatformSupport/private/qeglpbuffer_p.h>
#endif

#include <QtGui/QSurface>
#include <QtGui/QOpenGLContext>
#include <QtGui/QOpenGLFunctions>
#if QT_VERSION >= QT_VERSION_CHECK(5, 6, 0)
#include <QtGui/QOpenGLExtraFunctions>
#endif
#include <QtDebug>

#ifndef GL_DEPTH24_STENCIL8_OES
#define GL_DEPTH24_STENCIL8_OES 0x88F0
#endif

#include "qeglfscontext.h"
#include "qeglfswindow.h"
#include "qeglfsintegration.h"

QT_BEGIN_NAMESPACE

QEglFSContext::QEglFSContext(
                               HwComposerContext *hwc
                             , const QSurfaceFormat &format
                             , QPlatformOpenGLContext *share
                             , EGLDisplay display
#if QT_VERSION < QT_VERSION_CHECK(5, 3, 0)
                             , EGLenum eglApi
#endif
                            )
    : QEGLPlatformContext(
                          hwc->surfaceFormatFor(format)
                        , share
                        , display
                        , &(m_config = QEglFSIntegration::chooseConfig(display, hwc->surfaceFormatFor(format)))
#if QT_VERSION < QT_VERSION_CHECK(5, 3, 0)
                        , eglApi
#endif
      ),
    m_hwc(hwc), m_swapIntervalConfigured(false),
    m_rotFbo(0), m_rotTexture(0), m_rotDepthStencil(0), m_rotProgram(0), m_rotVbo(0),
    m_rawBindFramebuffer(0),
    m_rotFailed(false)
{
}

QEglFSContext::~QEglFSContext()
{
    // GL objects die with the EGL context; nothing to release explicitly.
}

bool QEglFSContext::rotating(QPlatformSurface *surface) const
{
    return !m_rotFailed && m_hwc->displayRotation() != 0 && surface
            && surface->surface()->surfaceClass() == QSurface::Window;
}

GLuint QEglFSContext::defaultFramebufferObject(QPlatformSurface *surface) const
{
    if (rotating(surface) && m_rotFbo)
        return m_rotFbo;
    return QEGLPlatformContext::defaultFramebufferObject(surface);
}

static GLuint compileShader(QOpenGLFunctions *f, GLenum type, const char *src)
{
    GLuint shader = f->glCreateShader(type);
    f->glShaderSource(shader, 1, &src, 0);
    f->glCompileShader(shader);
    GLint ok = 0;
    f->glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512] = { 0 };
        f->glGetShaderInfoLog(shader, sizeof(log) - 1, 0, log);
        qWarning("QPA_HWC_ROTATION: shader compile failed: %s", log);
        f->glDeleteShader(shader);
        return 0;
    }
    return shader;
}

bool QEglFSContext::ensureRotationTarget()
{
    if (m_rotFbo)
        return true;

    // QOpenGLFunctions::glBindFramebuffer(0) is redirected by Qt to
    // defaultFramebufferObject(), i.e. back to our own FBO. Reaching the real
    // window surface needs the driver's entry point.
    m_rawBindFramebuffer = reinterpret_cast<RawBindFramebuffer>(
            QOpenGLContext::currentContext()->getProcAddress("glBindFramebuffer"));
    if (!m_rawBindFramebuffer) {
        qWarning("QPA_HWC_ROTATION: cannot resolve glBindFramebuffer, rendering unrotated");
        m_rotFailed = true;
        return false;
    }

    QOpenGLFunctions *f = QOpenGLContext::currentContext()->functions();
    const QSize size = m_hwc->logicalScreenSize();

    f->glGenTextures(1, &m_rotTexture);
    f->glBindTexture(GL_TEXTURE_2D, m_rotTexture);
    f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    f->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, size.width(), size.height(), 0,
                    GL_RGBA, GL_UNSIGNED_BYTE, 0);
    f->glBindTexture(GL_TEXTURE_2D, 0);

    f->glGenRenderbuffers(1, &m_rotDepthStencil);
    f->glBindRenderbuffer(GL_RENDERBUFFER, m_rotDepthStencil);
    f->glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8_OES, size.width(), size.height());
    f->glBindRenderbuffer(GL_RENDERBUFFER, 0);

    f->glGenFramebuffers(1, &m_rotFbo);
    f->glBindFramebuffer(GL_FRAMEBUFFER, m_rotFbo);
    f->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_rotTexture, 0);
    f->glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, m_rotDepthStencil);
    f->glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, m_rotDepthStencil);
    GLenum status = f->glCheckFramebufferStatus(GL_FRAMEBUFFER);

    static const char *vs =
        "attribute vec2 pos;\n"
        "attribute vec2 tc;\n"
        "varying vec2 v_tc;\n"
        "void main() { v_tc = tc; gl_Position = vec4(pos, 0.0, 1.0); }\n";
    static const char *fs =
        "precision mediump float;\n"
        "varying vec2 v_tc;\n"
        "uniform sampler2D tex;\n"
        "void main() { gl_FragColor = texture2D(tex, v_tc); }\n";
    GLuint v = compileShader(f, GL_VERTEX_SHADER, vs);
    GLuint fr = compileShader(f, GL_FRAGMENT_SHADER, fs);
    if (v && fr) {
        m_rotProgram = f->glCreateProgram();
        f->glAttachShader(m_rotProgram, v);
        f->glAttachShader(m_rotProgram, fr);
        f->glBindAttribLocation(m_rotProgram, 0, "pos");
        f->glBindAttribLocation(m_rotProgram, 1, "tc");
        f->glLinkProgram(m_rotProgram);
        GLint linked = 0;
        f->glGetProgramiv(m_rotProgram, GL_LINK_STATUS, &linked);
        if (!linked) {
            f->glDeleteProgram(m_rotProgram);
            m_rotProgram = 0;
        }
    }
    if (v) f->glDeleteShader(v);
    if (fr) f->glDeleteShader(fr);

    if (status != GL_FRAMEBUFFER_COMPLETE || !m_rotProgram) {
        qWarning("QPA_HWC_ROTATION: cannot set up rotation (fbo status 0x%x, program %u), "
                 "rendering unrotated", status, m_rotProgram);
        f->glBindFramebuffer(GL_FRAMEBUFFER, 0);
        f->glDeleteFramebuffers(1, &m_rotFbo);
        f->glDeleteRenderbuffers(1, &m_rotDepthStencil);
        f->glDeleteTextures(1, &m_rotTexture);
        if (m_rotProgram) f->glDeleteProgram(m_rotProgram);
        m_rotFbo = m_rotTexture = m_rotDepthStencil = m_rotProgram = 0;
        m_rotFailed = true;
        return false;
    }
    qDebug("QPA_HWC_ROTATION: rendering %dx%d, rotating by %d onto %dx%d",
           size.width(), size.height(), m_hwc->displayRotation(),
           m_hwc->screenSize().width(), m_hwc->screenSize().height());
    return true;
}

void QEglFSContext::blitRotated()
{
    QOpenGLFunctions *f = QOpenGLContext::currentContext()->functions();

    // Save the state we touch; Qt Quick caches some of it between frames.
    GLint program = 0, arrayBuffer = 0, activeTexture = 0, texture = 0;
    GLint viewport[4] = { 0, 0, 0, 0 };
    GLint attrib0 = 0, attrib1 = 0;
    f->glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    f->glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &arrayBuffer);
    f->glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTexture);
    f->glActiveTexture(GL_TEXTURE0);
    f->glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
    f->glGetIntegerv(GL_VIEWPORT, viewport);
    f->glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &attrib0);
    f->glGetVertexAttribiv(1, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &attrib1);
    const GLenum caps[] = { GL_BLEND, GL_DEPTH_TEST, GL_SCISSOR_TEST, GL_STENCIL_TEST, GL_CULL_FACE };
    GLboolean capState[5];
    for (int i = 0; i < 5; ++i) {
        capState[i] = f->glIsEnabled(caps[i]);
        f->glDisable(caps[i]);
    }

    // A non-zero VAO (Qt Quick / Mali on ES3) would make our vertex setup
    // invalid; temporarily switch to the default one.
    GLint vao = 0;
#if QT_VERSION >= QT_VERSION_CHECK(5, 6, 0)
    QOpenGLExtraFunctions *ef = 0;
    if (QOpenGLContext::currentContext()->format().majorVersion() >= 3) {
        ef = QOpenGLContext::currentContext()->extraFunctions();
        f->glGetIntegerv(0x85B5 /* GL_VERTEX_ARRAY_BINDING */, &vao);
        if (vao)
            ef->glBindVertexArray(0);
    }
#endif

    const QSize native = m_hwc->screenSize();
    m_rawBindFramebuffer(GL_FRAMEBUFFER, 0);
    f->glViewport(0, 0, native.width(), native.height());


    // Quad corners on the panel: bottom-left, bottom-right, top-left, top-right.
    static const GLfloat pos[] = { -1, -1,  1, -1,  -1, 1,  1, 1 };
    // Logical texture coordinates shown at those corners for a clockwise rotation.
    static const GLfloat tc90[]  = { 1, 0,  1, 1,  0, 0,  0, 1 };
    static const GLfloat tc180[] = { 1, 1,  0, 1,  1, 0,  0, 0 };
    static const GLfloat tc270[] = { 0, 1,  0, 0,  1, 1,  1, 0 };
    const GLfloat *tc = m_hwc->displayRotation() == 90 ? tc90
                      : m_hwc->displayRotation() == 180 ? tc180 : tc270;

    if (!m_rotVbo) {
        f->glGenBuffers(1, &m_rotVbo);
        GLfloat data[3][16];
        const GLfloat *tcs[3] = { tc90, tc180, tc270 };
        for (int r = 0; r < 3; ++r)
            for (int i = 0; i < 4; ++i) {
                data[r][i * 4 + 0] = pos[i * 2];
                data[r][i * 4 + 1] = pos[i * 2 + 1];
                data[r][i * 4 + 2] = tcs[r][i * 2];
                data[r][i * 4 + 3] = tcs[r][i * 2 + 1];
            }
        f->glBindBuffer(GL_ARRAY_BUFFER, m_rotVbo);
        f->glBufferData(GL_ARRAY_BUFFER, sizeof(data), data, GL_STATIC_DRAW);
    }
    const int set = tc == tc90 ? 0 : tc == tc180 ? 1 : 2;
    const GLsizei stride = 4 * sizeof(GLfloat);
    const char *base = reinterpret_cast<const char *>(set * 16 * sizeof(GLfloat));

    f->glUseProgram(m_rotProgram);
    f->glBindBuffer(GL_ARRAY_BUFFER, m_rotVbo);
    f->glBindTexture(GL_TEXTURE_2D, m_rotTexture);
    f->glUniform1i(f->glGetUniformLocation(m_rotProgram, "tex"), 0);
    f->glEnableVertexAttribArray(0);
    f->glEnableVertexAttribArray(1);
    f->glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, stride, base);
    f->glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride, base + 2 * sizeof(GLfloat));
    f->glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    static int errorReports = 0;
    GLenum err = f->glGetError();
    if (err != GL_NO_ERROR && errorReports < 5) {
        ++errorReports;
        qWarning("QPA_HWC_ROTATION: GL error 0x%x in blit (vao %d)", err, vao);
    }

    // Restore.
    if (!attrib0) f->glDisableVertexAttribArray(0);
    if (!attrib1) f->glDisableVertexAttribArray(1);
    f->glBindTexture(GL_TEXTURE_2D, texture);
    f->glActiveTexture(activeTexture);
    f->glBindBuffer(GL_ARRAY_BUFFER, arrayBuffer);
    f->glUseProgram(program);
    f->glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
#if QT_VERSION >= QT_VERSION_CHECK(5, 6, 0)
    if (ef && vao)
        ef->glBindVertexArray(vao);
#endif
    for (int i = 0; i < 5; ++i) {
        if (capState[i])
            f->glEnable(caps[i]);
    }
}

bool QEglFSContext::makeCurrent(QPlatformSurface *surface)
{
    bool current = QEGLPlatformContext::makeCurrent(surface);
    if (current && rotating(surface) && ensureRotationTarget())
        QOpenGLContext::currentContext()->functions()->glBindFramebuffer(GL_FRAMEBUFFER, m_rotFbo);
    if (current && !m_swapIntervalConfigured) {
        m_swapIntervalConfigured = true;
        int swapInterval = 1;
        QByteArray swapIntervalString = qgetenv("QT_QPA_EGLFS_SWAPINTERVAL");
        if (!swapIntervalString.isEmpty()) {
            bool ok;
            swapInterval = swapIntervalString.toInt(&ok);
            if (!ok)
                swapInterval = 1;
        }
        eglSwapInterval(eglDisplay(), swapInterval);
    }
    return current;
}

EGLSurface QEglFSContext::eglSurfaceForPlatformSurface(QPlatformSurface *surface)
{
    if (surface->surface()->surfaceClass() == QSurface::Window)
        return static_cast<QEglFSWindow *>(surface)->surface();
    else
        return static_cast<QEGLPbuffer *>(surface)->pbuffer();
}

void QEglFSContext::swapBuffers(QPlatformSurface *surface)
{
    if (surface->surface()->surfaceClass() == QSurface::Window) {
        const bool rotate = rotating(surface) && m_rotFbo;
        if (rotate)
            blitRotated();
        m_hwc->swapToWindow(this, surface);
        if (rotate)
            QOpenGLContext::currentContext()->functions()->glBindFramebuffer(GL_FRAMEBUFFER, m_rotFbo);
    } else {
        QEGLPlatformContext::swapBuffers(surface);
    }
}

QT_END_NAMESPACE

